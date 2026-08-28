package main

import (
	"bufio"
	"context"
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
	routeWorkerReady       = "ZEUS_ROUTE_WORKER\t1"
	routeWorkerFramePrefix = "ZEUS_ROUTE_RESPONSE"
	maxRouteFrameBytes     = 16 << 20
)

var errRouteWorkersClosed = errors.New("route worker manager is closed")

type RouteWorkerRequest struct {
	FromLon     float64
	FromLat     float64
	ToLon       float64
	ToLat       float64
	Algorithm   string
	MaxDistance float64
	OutputPath  string
}

type RouteWorkerResult struct {
	Output   string
	ExitCode int
}

type routeWorkerCall struct {
	ctx      context.Context
	request  RouteWorkerRequest
	response chan routeWorkerCallResult
}

type routeWorkerCallResult struct {
	result RouteWorkerResult
	err    error
}

type routeWorker struct {
	executable string
	mapPath    string
	calls      chan routeWorkerCall
	stop       chan struct{}
	done       chan struct{}
	closeOnce  sync.Once
}

func newRouteWorker(executable, mapPath string) *routeWorker {
	worker := &routeWorker{
		executable: executable,
		mapPath:    mapPath,
		calls:      make(chan routeWorkerCall),
		stop:       make(chan struct{}),
		done:       make(chan struct{}),
	}
	go worker.loop()
	return worker
}

func (w *routeWorker) Query(
	ctx context.Context,
	request RouteWorkerRequest,
) (RouteWorkerResult, error) {
	response := make(chan routeWorkerCallResult, 1)
	call := routeWorkerCall{ctx: ctx, request: request, response: response}
	select {
	case w.calls <- call:
	case <-w.stop:
		return RouteWorkerResult{}, errRouteWorkersClosed
	case <-ctx.Done():
		return RouteWorkerResult{}, ctx.Err()
	}
	select {
	case result := <-response:
		return result.result, result.err
	case <-w.stop:
		return RouteWorkerResult{}, errRouteWorkersClosed
	case <-ctx.Done():
		return RouteWorkerResult{}, ctx.Err()
	}
}

func (w *routeWorker) Close() {
	w.closeOnce.Do(func() { close(w.stop) })
	<-w.done
}

func (w *routeWorker) loop() {
	defer close(w.done)
	var session *routeWorkerSession
	defer func() {
		if session != nil {
			session.Close()
		}
	}()
	for {
		select {
		case call := <-w.calls:
			if err := call.ctx.Err(); err != nil {
				call.response <- routeWorkerCallResult{err: err}
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
			var result RouteWorkerResult
			var err error
			for attempt := 0; attempt < 2; attempt++ {
				if session == nil {
					session, err = startRouteWorkerSession(
						callCtx, w.executable, w.mapPath)
					if err != nil {
						break
					}
				}
				result, err = session.RoundTrip(callCtx, call.request)
				if err == nil || callCtx.Err() != nil {
					break
				}
				session.Close()
				session = nil
			}
			cancelCall()
			call.response <- routeWorkerCallResult{result: result, err: err}
		case <-w.stop:
			return
		}
	}
}

type routeWorkerSession struct {
	command   *exec.Cmd
	stdin     io.WriteCloser
	stdout    *bufio.Reader
	stderr    *tailBuffer
	wait      chan error
	closeOnce sync.Once
}

func startRouteWorkerSession(
	ctx context.Context,
	executable, mapPath string,
) (*routeWorkerSession, error) {
	command := exec.Command(executable, "route-worker", mapPath)
	stdin, err := command.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("open route worker stdin: %w", err)
	}
	stdoutPipe, err := command.StdoutPipe()
	if err != nil {
		_ = stdin.Close()
		return nil, fmt.Errorf("open route worker stdout: %w", err)
	}
	stderr := &tailBuffer{limit: 32 << 10}
	command.Stderr = stderr
	if err := command.Start(); err != nil {
		_ = stdin.Close()
		return nil, fmt.Errorf("start route worker: %w", err)
	}
	session := &routeWorkerSession{
		command: command,
		stdin:   stdin,
		stdout:  bufio.NewReader(stdoutPipe),
		stderr:  stderr,
		wait:    make(chan error, 1),
	}
	go func() { session.wait <- command.Wait() }()
	ready := make(chan routeWorkerCallResult, 1)
	go func() {
		line, readErr := session.stdout.ReadString('\n')
		if readErr != nil {
			ready <- routeWorkerCallResult{err: readErr}
			return
		}
		ready <- routeWorkerCallResult{result: RouteWorkerResult{Output: strings.TrimSpace(line)}}
	}()
	select {
	case status := <-ready:
		if status.err != nil || status.result.Output != routeWorkerReady {
			session.Close()
			message := strings.TrimSpace(stderr.String())
			if message == "" {
				message = status.result.Output
			}
			return nil, fmt.Errorf("route worker did not become ready: %s", message)
		}
		return session, nil
	case <-ctx.Done():
		session.Close()
		return nil, ctx.Err()
	}
}

func (s *routeWorkerSession) RoundTrip(
	ctx context.Context,
	request RouteWorkerRequest,
) (RouteWorkerResult, error) {
	line, err := encodeRouteWorkerRequest(request)
	if err != nil {
		return RouteWorkerResult{}, err
	}
	response := make(chan routeWorkerCallResult, 1)
	go func() {
		result, roundTripErr := s.roundTrip(line)
		response <- routeWorkerCallResult{result: result, err: roundTripErr}
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
		return RouteWorkerResult{}, ctx.Err()
	}
}

func (s *routeWorkerSession) roundTrip(line string) (RouteWorkerResult, error) {
	if _, err := io.WriteString(s.stdin, line+"\n"); err != nil {
		return RouteWorkerResult{}, fmt.Errorf("write route worker request: %w", err)
	}
	header, err := s.stdout.ReadString('\n')
	if err != nil {
		return RouteWorkerResult{}, fmt.Errorf("read route worker response header: %w", err)
	}
	parts := strings.Split(strings.TrimSpace(header), "\t")
	if len(parts) != 3 || parts[0] != routeWorkerFramePrefix {
		return RouteWorkerResult{}, fmt.Errorf("invalid route worker response header %q", header)
	}
	exitCode, err := strconv.Atoi(parts[1])
	if err != nil {
		return RouteWorkerResult{}, fmt.Errorf("invalid route worker exit code: %w", err)
	}
	size, err := strconv.Atoi(parts[2])
	if err != nil || size < 0 || size > maxRouteFrameBytes {
		return RouteWorkerResult{}, fmt.Errorf("invalid route worker frame size %q", parts[2])
	}
	payload := make([]byte, size)
	if _, err := io.ReadFull(s.stdout, payload); err != nil {
		return RouteWorkerResult{}, fmt.Errorf("read route worker response body: %w", err)
	}
	return RouteWorkerResult{Output: strings.TrimSpace(string(payload)), ExitCode: exitCode}, nil
}

func (s *routeWorkerSession) Close() {
	s.closeOnce.Do(func() {
		_ = s.stdin.Close()
		if s.command.Process != nil {
			_ = s.command.Process.Kill()
		}
		<-s.wait
	})
}

func encodeRouteWorkerRequest(request RouteWorkerRequest) (string, error) {
	if strings.ContainsAny(request.OutputPath, "\t\r\n") {
		return "", errors.New("route output path contains a protocol delimiter")
	}
	fields := []string{
		formatFloat(request.FromLon),
		formatFloat(request.FromLat),
		formatFloat(request.ToLon),
		formatFloat(request.ToLat),
		request.Algorithm,
		formatFloat(request.MaxDistance),
		request.OutputPath,
	}
	for _, field := range fields[:6] {
		if field == "" || strings.ContainsAny(field, "\t\r\n") {
			return "", errors.New("route request contains an invalid protocol field")
		}
	}
	return strings.Join(fields, "\t"), nil
}

type tailBuffer struct {
	mu    sync.Mutex
	data  []byte
	limit int
}

func (b *tailBuffer) Write(value []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.data = append(b.data, value...)
	if len(b.data) > b.limit {
		b.data = append([]byte(nil), b.data[len(b.data)-b.limit:]...)
	}
	return len(value), nil
}

func (b *tailBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return string(b.data)
}

type routeWorkerEntry struct {
	worker   *routeWorker
	refs     int
	lastUsed uint64
}

type RouteWorkerManager struct {
	executable string
	timeout    time.Duration
	maxMaps    int

	mu      sync.Mutex
	workers map[string]*routeWorkerEntry
	changed chan struct{}
	clock   uint64
	closed  bool
}

func NewRouteWorkerManager(executable string, timeout time.Duration, maxMaps int) *RouteWorkerManager {
	if timeout <= 0 {
		timeout = 2 * time.Minute
	}
	if maxMaps <= 0 {
		maxMaps = 4
	}
	return &RouteWorkerManager{
		executable: executable,
		timeout:    timeout,
		maxMaps:    maxMaps,
		workers:    make(map[string]*routeWorkerEntry),
		changed:    make(chan struct{}),
	}
}

func (m *RouteWorkerManager) Route(
	parent context.Context,
	mapPath string,
	request RouteWorkerRequest,
) (RouteWorkerResult, error) {
	ctx, cancel := context.WithTimeout(parent, m.timeout)
	defer cancel()
	entry, err := m.acquire(ctx, mapPath)
	if err != nil {
		return RouteWorkerResult{}, err
	}
	defer m.release(mapPath, entry)
	return entry.worker.Query(ctx, request)
}

func (m *RouteWorkerManager) acquire(
	ctx context.Context,
	mapPath string,
) (*routeWorkerEntry, error) {
	for {
		m.mu.Lock()
		if m.closed {
			m.mu.Unlock()
			return nil, errRouteWorkersClosed
		}
		m.clock++
		if entry := m.workers[mapPath]; entry != nil {
			entry.refs++
			entry.lastUsed = m.clock
			m.mu.Unlock()
			return entry, nil
		}
		if len(m.workers) < m.maxMaps {
			entry := &routeWorkerEntry{
				worker: newRouteWorker(m.executable, mapPath), refs: 1, lastUsed: m.clock,
			}
			m.workers[mapPath] = entry
			m.mu.Unlock()
			return entry, nil
		}
		var victimPath string
		var victim *routeWorkerEntry
		for path, entry := range m.workers {
			if entry.refs == 0 && (victim == nil || entry.lastUsed < victim.lastUsed) {
				victimPath, victim = path, entry
			}
		}
		if victim != nil {
			delete(m.workers, victimPath)
			entry := &routeWorkerEntry{
				worker: newRouteWorker(m.executable, mapPath), refs: 1, lastUsed: m.clock,
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

func (m *RouteWorkerManager) release(mapPath string, entry *routeWorkerEntry) {
	m.mu.Lock()
	if current := m.workers[mapPath]; current == entry && entry.refs > 0 {
		entry.refs--
	}
	m.notifyLocked()
	m.mu.Unlock()
}

func (m *RouteWorkerManager) notifyLocked() {
	close(m.changed)
	m.changed = make(chan struct{})
}

func (m *RouteWorkerManager) Close() {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	m.closed = true
	workers := make([]*routeWorker, 0, len(m.workers))
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
