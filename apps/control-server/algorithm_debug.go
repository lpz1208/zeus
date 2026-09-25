package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

type algorithmDebugEvent struct {
	SessionID     string            `json:"sessionId"`
	Kind          string            `json:"kind"`
	Line          int               `json:"line,omitempty"`
	Sequence      int               `json:"sequence,omitempty"`
	Variables     map[string]string `json:"variables,omitempty"`
	Frames        []string          `json:"frames,omitempty"`
	Steps         int               `json:"steps,omitempty"`
	ExpandedNodes int               `json:"expandedNodes,omitempty"`
	Logs          []string          `json:"logs,omitempty"`
	Result        json.RawMessage   `json:"result,omitempty"`
}

type algorithmDebugCommand struct {
	Action      string `json:"action"`
	Sequence    int    `json:"sequence"`
	Breakpoints []int  `json:"breakpoints,omitempty"`
}

type algorithmDebugSession struct {
	id            string
	request       AlgorithmLabRequest
	prepared      *algorithmSnapshot
	ctx           context.Context
	cancel        context.CancelFunc
	idle          *time.Timer
	input         *os.File
	events        chan algorithmDebugEvent
	done, cleaned chan struct{}
	processErr    chan error
	mu            sync.Mutex // serialize commands; stop never waits for this lock
	sequence      int
}

func (s *Server) closeAlgorithmDebuggers() {
	s.algorithmDebugMu.Lock()
	s.algorithmDebugClosed = true
	sessions := make([]*algorithmDebugSession, 0, len(s.algorithmDebugSessions))
	for _, session := range s.algorithmDebugSessions {
		session.cancel()
		sessions = append(sessions, session)
	}
	s.algorithmDebugMu.Unlock()
	for _, session := range sessions {
		<-session.cleaned
	}
}

// A successful creation takes ownership of the prepared directory and one
// algorithm slot until the process has actually exited and cleanup completes.
func (s *Server) newAlgorithmDebugger(request AlgorithmLabRequest, prepared *algorithmSnapshot) (*algorithmDebugSession, error) {
	prepared.result.ExecutionMode = "debug"
	inputRead, inputWrite, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	outputRead, outputWrite, err := os.Pipe()
	if err != nil {
		inputRead.Close()
		inputWrite.Close()
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
	d := &algorithmDebugSession{id: newID("debug"), request: request, prepared: prepared,
		ctx: ctx, cancel: cancel, input: inputWrite, events: make(chan algorithmDebugEvent, 1),
		done: make(chan struct{}), cleaned: make(chan struct{}), processErr: make(chan error, 1)}
	d.idle = time.AfterFunc(60*time.Second, cancel)
	s.algorithmDebugMu.Lock()
	if s.algorithmDebugClosed {
		s.algorithmDebugMu.Unlock()
		d.idle.Stop()
		cancel()
		inputRead.Close()
		inputWrite.Close()
		outputRead.Close()
		outputWrite.Close()
		return nil, errors.New("服务正在关闭")
	}
	if s.algorithmDebugSessions == nil {
		s.algorithmDebugSessions = make(map[string]*algorithmDebugSession)
	}
	s.algorithmDebugSessions[d.id] = d
	s.algorithmDebugMu.Unlock()
	go func() {
		err := runAlgorithmIO(ctx, s.config.AlgorithmPython, prepared.runner, []string{prepared.graph, "--debug"}, inputRead, outputWrite)
		inputRead.Close()
		outputWrite.Close()
		d.processErr <- err
		close(d.done)
		if err != nil {
			cancel()
		}
	}()
	go func() {
		defer close(d.events)
		scanner := bufio.NewScanner(outputRead)
		scanner.Buffer(make([]byte, 4096), 2<<20)
		for scanner.Scan() {
			var event algorithmDebugEvent
			if json.Unmarshal(scanner.Bytes(), &event) != nil || (event.Kind != "paused" && event.Kind != "finished") {
				cancel()
				return
			}
			select {
			case d.events <- event:
			case <-ctx.Done():
				return
			}
		}
		if scanner.Err() != nil {
			cancel()
		}
	}()
	go func() {
		<-ctx.Done()
		d.idle.Stop()
		inputWrite.Close()
		outputRead.Close()
		<-d.done
		os.RemoveAll(prepared.dir)
		<-s.algorithmSlots
		s.algorithmDebugMu.Lock()
		delete(s.algorithmDebugSessions, d.id)
		s.algorithmDebugMu.Unlock()
		close(d.cleaned)
	}()
	return d, nil
}

func (s *Server) nextAlgorithmDebugEvent(d *algorithmDebugSession) (algorithmDebugEvent, error) {
	d.idle.Stop()
	timeout := time.NewTimer(10 * time.Second)
	defer timeout.Stop()
	select {
	case <-d.ctx.Done():
		return algorithmDebugEvent{}, errors.New("调试已停止或会话已过期")
	case <-timeout.C:
		d.cancel()
		return algorithmDebugEvent{}, errors.New("调试执行超过 10 秒，已终止进程")
	case event, ok := <-d.events:
		if !ok {
			select {
			case <-d.done:
			case <-d.ctx.Done():
				return event, errors.New("调试进程已停止")
			}
			err := <-d.processErr
			d.cancel()
			if err == nil {
				err = errors.New("调试进程未返回完成结果")
			}
			return event, err
		}
		event.SessionID = d.id
		if d.ctx.Err() != nil {
			return event, errors.New("调试已停止或会话已过期")
		}
		if event.Kind == "paused" {
			d.sequence = event.Sequence
			d.idle.Reset(60 * time.Second)
			return event, nil
		}
		// The final result crosses the same native validator and persistence
		// boundary as normal execution; a debugger cannot assert a valid path.
		result := d.prepared.result
		if json.Unmarshal(event.Result, &result) != nil {
			d.cancel()
			return event, errors.New("无效的调试结果")
		}
		ctx, cancel := context.WithTimeout(d.ctx, 20*time.Second)
		checked, err := s.validateAlgorithmResult(ctx, d.prepared, result)
		cancel()
		if err != nil {
			d.cancel()
			return event, err
		}
		if d.ctx.Err() != nil {
			return event, errors.New("调试已停止")
		}
		checked = s.saveAlgorithmExperiment(d.request, checked)
		event.Result, _ = json.Marshal(checked)
		d.cancel()
		return event, nil
	}
}

func (s *Server) handleAlgorithmDebugStart(w http.ResponseWriter, r *http.Request) {
	var request AlgorithmLabRequest
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 128<<10))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&request) != nil || decoder.Decode(new(any)) != io.EOF {
		writeError(w, 400, "无效调试请求")
		return
	}
	if err := validateAlgorithmRequest(&request); err != nil {
		writeError(w, 400, err.Error())
		return
	}
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, 404, "地图不存在")
		return
	}
	select {
	case s.algorithmSlots <- struct{}{}:
	default:
		writeError(w, 429, "已有两个算法在运行，请停止其他实验后重试")
		return
	}
	owned := true
	defer func() {
		if owned {
			<-s.algorithmSlots
		}
	}()
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	prepared, err := s.prepareAlgorithm(ctx, record, request)
	if err != nil {
		writeError(w, 422, err.Error())
		return
	}
	d, err := s.newAlgorithmDebugger(request, prepared)
	if err != nil {
		os.RemoveAll(prepared.dir)
		writeError(w, 503, "无法创建调试进程")
		return
	}
	owned = false
	stopCancellation := context.AfterFunc(ctx, d.cancel)
	defer stopCancellation()
	d.mu.Lock()
	defer d.mu.Unlock()
	if json.NewEncoder(d.input).Encode(map[string]any{"source": request.Source, "steps": request.Steps}) != nil {
		d.cancel()
		writeError(w, 502, "调试进程无法读取源码")
		return
	}
	event, err := s.nextAlgorithmDebugEvent(d)
	if err != nil {
		d.cancel()
		writeError(w, 422, err.Error())
		return
	}
	writeJSON(w, 200, event)
}

func (s *Server) handleAlgorithmDebugCommand(w http.ResponseWriter, r *http.Request) {
	s.algorithmDebugMu.Lock()
	d := s.algorithmDebugSessions[r.PathValue("debug")]
	s.algorithmDebugMu.Unlock()
	if d == nil || d.prepared.result.MapID != r.PathValue("id") {
		writeError(w, 404, "调试会话不存在或已过期")
		return
	}
	if r.Method == http.MethodDelete {
		d.cancel()
		writeJSON(w, 200, map[string]bool{"ok": true})
		return
	}
	var command algorithmDebugCommand
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 8192))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&command) != nil || decoder.Decode(new(any)) != io.EOF || (command.Action != "step" && command.Action != "continue") || len(command.Breakpoints) > 64 {
		writeError(w, 400, "只支持 step/continue，最多 64 个断点")
		return
	}
	lines := strings.Count(d.request.Source, "\n") + 1
	for _, line := range command.Breakpoints {
		if line < 1 || line > lines {
			writeError(w, 400, "断点行号超出源码范围")
			return
		}
	}
	if !d.mu.TryLock() {
		writeError(w, 409, "上一条调试命令仍在执行")
		return
	}
	defer d.mu.Unlock()
	stopCancellation := context.AfterFunc(r.Context(), d.cancel)
	defer stopCancellation()
	if d.ctx.Err() != nil {
		writeError(w, 410, "调试会话已结束")
		return
	}
	if command.Sequence != d.sequence {
		writeError(w, 409, "调试现场已变化，此命令未执行")
		return
	}
	if json.NewEncoder(d.input).Encode(command) != nil {
		d.cancel()
		writeError(w, 410, "调试连接已关闭")
		return
	}
	event, err := s.nextAlgorithmDebugEvent(d)
	if err != nil {
		d.cancel()
		writeError(w, 422, err.Error())
		return
	}
	writeJSON(w, 200, event)
}
