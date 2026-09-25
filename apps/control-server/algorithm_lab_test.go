package main

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestAlgorithmCapabilitiesAndRequestValidation(t *testing.T) {
	s := NewServer(Config{}, nil)
	defer s.Close()
	// Invoke the handler directly: capabilities require no data directory.
	w := httptest.NewRecorder()
	s.handleAlgorithmCapabilities(w, httptest.NewRequest("GET", "/", nil))
	if w.Code != 200 || !strings.Contains(w.Body.String(), "ctx.neighbors") {
		t.Fatal(w.Body.String())
	}
	lon, lat := 114.0, 30.0
	request := AlgorithmLabRequest{FromLon: &lon, ToLon: &lon, FromLat: &lat, ToLat: &lat, Source: algorithmTemplate}
	if err := validateAlgorithmRequest(&request); err != nil {
		t.Fatal(err)
	}
	request.Steps = 5000001
	if validateAlgorithmRequest(&request) == nil {
		t.Fatal("unbounded steps accepted")
	}
	request.Steps = 1000
	request.FromLat = nil
	if validateAlgorithmRequest(&request) == nil {
		t.Fatal("missing OD accepted")
	}
	bad := httptest.NewRecorder()
	s.handleAlgorithmRun(bad, httptest.NewRequest("POST", "/", strings.NewReader(`{"source":"x","unexpected":true}`)))
	if bad.Code != http.StatusBadRequest {
		t.Fatal("unknown fields accepted")
	}
}
func TestAlgorithmChildExecutionAndCancellation(t *testing.T) {
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 unavailable")
	}
	dir := t.TempDir()
	runner := filepath.Join(dir, "runner.py")
	graph := filepath.Join(dir, "graph.json")
	if err = os.WriteFile(runner, algorithmRunner, 0600); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(graph, []byte(`{"adjacency":{"-1":[[0,0,2,20]],"0":[[-2,1,3,30]]},"nodes":[1,2],"estimates":[3,0],"baseline":{"ok":true,"timeS":5,"lengthM":50}}`), 0600); err != nil {
		t.Fatal(err)
	}
	payload, _ := json.Marshal(map[string]any{"source": algorithmTemplate})
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	output, err := runAlgorithmProcess(ctx, python, runner, graph, payload)
	if err != nil {
		t.Fatal(err)
	}
	var result AlgorithmLabResult
	if err = json.Unmarshal(output, &result); err != nil || !result.OK || len(result.States) != 3 {
		t.Fatalf("%s: %v", output, err)
	}
	ctx, abort := context.WithCancel(context.Background())
	abort()
	if _, err = runAlgorithmProcess(ctx, python, runner, graph, payload); err == nil {
		t.Fatal("cancelled run executed")
	}
	// Parent deadline also bounds code outside the interpreter, e.g. context loading.
	slow := filepath.Join(dir, "slow.py")
	os.WriteFile(slow, []byte("import time\ntime.sleep(10)\n"), 0600)
	ctx, abort = context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer abort()
	started := time.Now()
	if _, err = runAlgorithmProcess(ctx, python, slow, graph, nil); err == nil {
		t.Fatal("deadline ignored")
	}
	if time.Since(started) > time.Second {
		t.Fatal("child was not terminated promptly")
	}
}
func TestAlgorithmOutputBudget(t *testing.T) {
	b := &limitedAlgorithmOutput{limit: 5}
	if _, err := b.Write([]byte("12345")); err != nil {
		t.Fatal(err)
	}
	if _, err := b.Write([]byte("6")); err == nil {
		t.Fatal("output bound ignored")
	}
	if !bytes.Equal(b.Bytes(), []byte("12345")) {
		t.Fatal("oversized output was retained")
	}
}
