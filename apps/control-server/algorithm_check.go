package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

func (s *Server) handleAlgorithmCheck(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Source string `json:"source"`
	}
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 128<<10))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&request) != nil || decoder.Decode(new(any)) != io.EOF || len(request.Source) > 32768 {
		writeError(w, 400, "无效的代码检查请求（最多 32 KiB）")
		return
	}
	select {
	case s.algorithmSlots <- struct{}{}:
		defer func() { <-s.algorithmSlots }()
	default:
		writeError(w, 429, "执行器忙碌，请稍后检查")
		return
	}
	dir, err := os.MkdirTemp("", "zeus-code-check-")
	if err != nil {
		writeError(w, 500, "无法准备语法检查")
		return
	}
	defer os.RemoveAll(dir)
	runner := filepath.Join(dir, "runner.py")
	if os.WriteFile(runner, algorithmRunner, 0600) != nil {
		writeError(w, 500, "无法准备语法检查")
		return
	}
	payload, _ := json.Marshal(request)
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	output, err := runAlgorithmProcess(ctx, s.config.AlgorithmPython, runner, "--check", payload)
	if err != nil {
		writeError(w, 503, "语法检查暂不可用")
		return
	}
	if !json.Valid(output) {
		writeError(w, 502, "语法检查结果无效")
		return
	}
	writeJSON(w, 200, json.RawMessage(output))
}
