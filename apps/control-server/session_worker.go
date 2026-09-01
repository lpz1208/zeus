package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	sessionWorkerReady       = "ZEUS_SESSION_WORKER\t1"
	sessionWorkerFramePrefix = "ZEUS_SESSION_RESPONSE"
	maxSessionFrameBytes     = 64 << 20
)

var errSessionWorkersClosed = errors.New("session worker manager is closed")

// SessionCommandResult carries the JSON payload of one worker response. The
// exit code distinguishes computation outcomes (for example an unroutable
// plan) from protocol errors.
type SessionCommandResult struct {
	Payload  json.RawMessage
	ExitCode int
}

type sessionWorkerCall struct {
	ctx      context.Context
	fields   []string
	response chan sessionWorkerCallResult
}

type sessionWorkerCallResult struct {
	result SessionCommandResult
	err    error
}

type sessionWorker struct {
	executable string
	mapPath    string
	calls      chan sessionWorkerCall
	stop       chan struct{}
	done       chan struct{}
	closeOnce  sync.Once
}

func newSessionWorker(executable, mapPath string) *sessionWorker {
	worker := &sessionWorker{
		executable: executable,
		mapPath:    mapPath,
		calls:      make(chan sessionWorkerCall),
		stop:       make(chan struct{}),
		done:       make(chan struct{}),
	}
	go worker.loop()
	return worker
}

// Command sends one tab-delimited request and waits for its frame.
func (w *sessionWorker) Command(
	ctx context.Context,
	fields ...string,
) (SessionCommandResult, error) {
	response := make(chan sessionWorkerCallResult, 1)
	call := sessionWorkerCall{ctx: ctx, fields: fields, response: response}
	select {
	case w.calls <- call:
	case <-w.stop:
		return SessionCommandResult{}, errSessionWorkersClosed
	case <-ctx.Done():
		return SessionCommandResult{}, ctx.Err()
	}
	select {
	case result := <-response:
		return result.result, result.err
	case <-w.stop:
		return SessionCommandResult{}, errSessionWorkersClosed
	case <-ctx.Done():
		return SessionCommandResult{}, ctx.Err()
	}
}

func (w *sessionWorker) Close() {
	w.closeOnce.Do(func() { close(w.stop) })
	<-w.done
}

func (w *sessionWorker) loop() {
	defer close(w.done)
	var session *sessionWorkerProcess
	defer func() {
		if session != nil {
			session.Close()
		}
	}()
	for {
		select {
		case call := <-w.calls:
			if err := call.ctx.Err(); err != nil {
				call.response <- sessionWorkerCallResult{err: err}
				continue
			}
			callCtx, cancelCall := context.WithCancel(call.ctx)
			go func() {
				select {
				case <-w.stop:
					cancelCall()
				case <-callCtx.Done():
				}
			}()
			var result SessionCommandResult
			var err error
			// A restart loses every session hosted by the process; callers
			// surface that as an "unknown session" error and re-create.
			for attempt := 0; attempt < 2; attempt++ {
				if session == nil {
					session, err = startSessionWorkerProcess(
						callCtx, w.executable, w.mapPath)
					if err != nil {
						break
					}
				}
				result, err = session.roundTrip(callCtx, call.fields)
				if err == nil || callCtx.Err() != nil {
					break
				}
				session.Close()
				session = nil
			}
			cancelCall()
			call.response <- sessionWorkerCallResult{result: result, err: err}
		case <-w.stop:
			return
		}
	}
}

type sessionWorkerProcess struct {
	command   *exec.Cmd
	stdin     io.WriteCloser
	stdout    *bufio.Reader
	stderr    *tailBuffer
	wait      chan error
	closeOnce sync.Once
}

func startSessionWorkerProcess(
	ctx context.Context,
	executable, mapPath string,
) (*sessionWorkerProcess, error) {
	command := exec.Command(executable, "session-worker", mapPath)
	stdin, err := command.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("open session worker stdin: %w", err)
	}
	stdoutPipe, err := command.StdoutPipe()
	if err != nil {
		_ = stdin.Close()
		return nil, fmt.Errorf("open session worker stdout: %w", err)
	}
	stderr := &tailBuffer{limit: 32 << 10}
	command.Stderr = stderr
	if err := command.Start(); err != nil {
		_ = stdin.Close()
		return nil, fmt.Errorf("start session worker: %w", err)
	}
	session := &sessionWorkerProcess{
		command: command,
		stdin:   stdin,
		stdout:  bufio.NewReader(stdoutPipe),
		stderr:  stderr,
		wait:    make(chan error, 1),
	}
	go func() { session.wait <- command.Wait() }()
	ready := make(chan error, 1)
	go func() {
		line, readErr := session.stdout.ReadString('\n')
		if readErr != nil {
			ready <- readErr
			return
		}
		if strings.TrimSpace(line) != sessionWorkerReady {
			ready <- fmt.Errorf("unexpected banner %q", strings.TrimSpace(line))
			return
		}
		ready <- nil
	}()
	select {
	case readyErr := <-ready:
		if readyErr != nil {
			session.Close()
			message := strings.TrimSpace(stderr.String())
			if message == "" {
				message = readyErr.Error()
			}
			return nil, fmt.Errorf("session worker did not become ready: %s", message)
		}
		return session, nil
	case <-ctx.Done():
		session.Close()
		return nil, ctx.Err()
	}
}

func (s *sessionWorkerProcess) roundTrip(
	ctx context.Context,
	fields []string,
) (SessionCommandResult, error) {
	line, err := encodeSessionWorkerRequest(fields)
	if err != nil {
		return SessionCommandResult{}, err
	}
	response := make(chan sessionWorkerCallResult, 1)
	go func() {
		result, roundTripErr := s.roundTripBlocking(line)
		response <- sessionWorkerCallResult{result: result, err: roundTripErr}
	}()
	select {
	case result := <-response:
		if result.err != nil {
			message := strings.TrimSpace(s.stderr.String())
			if message != "" {
				result.err = fmt.Errorf("%w: %s", result.err, message)
			}
		}
		return result.result, result.err
	case <-ctx.Done():
		s.Close()
		return SessionCommandResult{}, ctx.Err()
	}
}

func (s *sessionWorkerProcess) roundTripBlocking(
	line string,
) (SessionCommandResult, error) {
	if _, err := io.WriteString(s.stdin, line+"\n"); err != nil {
		return SessionCommandResult{}, fmt.Errorf("write session worker request: %w", err)
	}
	header, err := s.stdout.ReadString('\n')
	if err != nil {
		return SessionCommandResult{}, fmt.Errorf("read session worker response header: %w", err)
	}
	parts := strings.Split(strings.TrimSpace(header), "\t")
	if len(parts) != 3 || parts[0] != sessionWorkerFramePrefix {
		return SessionCommandResult{}, fmt.Errorf(
			"invalid session worker response header %q", header)
	}
	exitCode, err := strconv.Atoi(parts[1])
	if err != nil {
		return SessionCommandResult{}, fmt.Errorf("invalid session worker exit code: %w", err)
	}
	size, err := strconv.Atoi(parts[2])
	if err != nil || size < 0 || size > maxSessionFrameBytes {
		return SessionCommandResult{}, fmt.Errorf("invalid session worker frame size %q", parts[2])
	}
	payload := make([]byte, size)
	if _, err := io.ReadFull(s.stdout, payload); err != nil {
		return SessionCommandResult{}, fmt.Errorf("read session worker response body: %w", err)
	}
	// The worker terminates every payload with one newline.
	if trailing, readErr := s.stdout.ReadByte(); readErr == nil && trailing != '\n' {
		return SessionCommandResult{}, errors.New(
			"session worker frame is not newline terminated")
	}
	if !json.Valid(payload) {
		return SessionCommandResult{}, errors.New("session worker payload is not valid JSON")
	}
	return SessionCommandResult{Payload: payload, ExitCode: exitCode}, nil
}

func (s *sessionWorkerProcess) Close() {
	s.closeOnce.Do(func() {
		_ = s.stdin.Close()
		if s.command.Process != nil {
			_ = s.command.Process.Kill()
		}
		<-s.wait
	})
}

func encodeSessionWorkerRequest(fields []string) (string, error) {
	for _, field := range fields {
		if strings.ContainsAny(field, "\t\r\n") {
			return "", errors.New("session worker request contains a protocol delimiter")
		}
	}
	return strings.Join(fields, "\t"), nil
}

type sessionWorkerEntry struct {
	worker   *sessionWorker
	refs     int
	lastUsed uint64
}

// SessionWorkerManager keeps one resident session-worker process per recently
// used map, mirroring RouteWorkerManager. Sessions inside a process are lost
// when the process is evicted or restarts; callers re-create them.
type SessionWorkerManager struct {
	executable string
	timeout    time.Duration
	maxMaps    int

	mu      sync.Mutex
	workers map[string]*sessionWorkerEntry
	changed chan struct{}
	clock   uint64
	closed  bool
}

func NewSessionWorkerManager(
	executable string,
	timeout time.Duration,
	maxMaps int,
) *SessionWorkerManager {
	if timeout <= 0 {
		timeout = 10 * time.Minute
	}
	if maxMaps <= 0 {
		maxMaps = 2
	}
	return &SessionWorkerManager{
		executable: executable,
		timeout:    timeout,
		maxMaps:    maxMaps,
		workers:    make(map[string]*sessionWorkerEntry),
		changed:    make(chan struct{}),
	}
}

func (m *SessionWorkerManager) Command(
	parent context.Context,
	mapPath string,
	fields ...string,
) (SessionCommandResult, error) {
	ctx, cancel := context.WithTimeout(parent, m.timeout)
	defer cancel()
	entry, err := m.acquire(ctx, mapPath)
	if err != nil {
		return SessionCommandResult{}, err
	}
	defer m.release(mapPath, entry)
	return entry.worker.Command(ctx, fields...)
}

func (m *SessionWorkerManager) acquire(
	ctx context.Context,
	mapPath string,
) (*sessionWorkerEntry, error) {
	for {
		m.mu.Lock()
		if m.closed {
			m.mu.Unlock()
			return nil, errSessionWorkersClosed
		}
		m.clock++
		if entry := m.workers[mapPath]; entry != nil {
			entry.refs++
			entry.lastUsed = m.clock
			m.mu.Unlock()
			return entry, nil
		}
		if len(m.workers) < m.maxMaps {
			entry := &sessionWorkerEntry{
				worker: newSessionWorker(m.executable, mapPath), refs: 1, lastUsed: m.clock,
			}
			m.workers[mapPath] = entry
			m.mu.Unlock()
			return entry, nil
		}
		var victimPath string
		var victim *sessionWorkerEntry
		for path, entry := range m.workers {
			if entry.refs == 0 && (victim == nil || entry.lastUsed < victim.lastUsed) {
				victimPath, victim = path, entry
			}
		}
		if victim != nil {
			delete(m.workers, victimPath)
			entry := &sessionWorkerEntry{
				worker: newSessionWorker(m.executable, mapPath), refs: 1, lastUsed: m.clock,
			}
			m.workers[mapPath] = entry
			m.mu.Unlock()
			victim.worker.Close()
			return entry, nil
		}
		changed := m.changed
		m.mu.Unlock()
		select {
		case <-changed:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
}

func (m *SessionWorkerManager) release(mapPath string, entry *sessionWorkerEntry) {
	m.mu.Lock()
	if current := m.workers[mapPath]; current == entry && entry.refs > 0 {
		entry.refs--
	}
	m.notifyLocked()
	m.mu.Unlock()
}

func (m *SessionWorkerManager) notifyLocked() {
	close(m.changed)
	m.changed = make(chan struct{})
}

func (m *SessionWorkerManager) Close() {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	m.closed = true
	workers := make([]*sessionWorker, 0, len(m.workers))
	for _, entry := range m.workers {
		workers = append(workers, entry.worker)
	}
	m.workers = nil
	m.notifyLocked()
	m.mu.Unlock()
	for _, worker := range workers {
		worker.Close()
	}
}
