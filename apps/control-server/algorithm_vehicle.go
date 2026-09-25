package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Planning creates a candidate only. The existing decision/action endpoint
// remains the sole authority to commit vehicle movement.
type algorithmVehicleRequest struct {
	VehicleID           int    `json:"vehicleId"`
	BasedOnStateVersion uint64 `json:"basedOnStateVersion"`
	Source              string `json:"source"`
	Steps               int    `json:"steps"`
}

func (s *Server) handleAlgorithmVehiclePlan(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 64<<10)
	var req algorithmVehicleRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if decoder.Decode(&req) != nil || decoder.Decode(new(any)) != io.EOF {
		writeError(w, 400, "无效的车辆算法请求")
		return
	}
	if req.VehicleID < 0 || uint64(req.VehicleID) > 0xffffffff || req.BasedOnStateVersion == 0 {
		writeError(w, 400, "必须指定车辆和当前状态版本")
		return
	}
	if req.Steps == 0 {
		req.Steps = 2000000
	}
	if strings.TrimSpace(req.Source) == "" || len(req.Source) > 32768 || req.Steps < 1000 || req.Steps > 5000000 {
		writeError(w, 400, "代码须为 1–32768 字节，执行步数须为 1000–5000000")
		return
	}
	select {
	case s.algorithmSlots <- struct{}{}:
		defer func() { <-s.algorithmSlots }()
	default:
		writeError(w, 429, "已有两个算法在运行，请稍后重试")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	r = r.WithContext(ctx)
	dir, err := os.MkdirTemp("", "zeus-vehicle-algorithm-")
	if err != nil {
		writeError(w, 500, "无法创建执行目录")
		return
	}
	defer os.RemoveAll(dir)
	prepared := &algorithmSnapshot{dir: dir, graph: filepath.Join(dir, "context.json"), runner: filepath.Join(dir, "runner.py")}
	vehicle, version := strconv.Itoa(req.VehicleID), strconv.FormatUint(req.BasedOnStateVersion, 10)
	_, snapshot := s.agentSessionCommand(w, r, "algorithm-context", r.PathValue("session"), vehicle, version, prepared.graph)
	if snapshot == nil {
		return
	}
	if err = prepared.initialize(r.PathValue("id"), req.Source); err != nil {
		writeError(w, 502, err.Error())
		return
	}
	payload, _ := json.Marshal(map[string]any{"source": req.Source, "steps": req.Steps})
	runCtx, stop := context.WithTimeout(ctx, 10*time.Second)
	output, runErr := runAlgorithmProcess(runCtx, s.config.AlgorithmPython, prepared.runner, prepared.graph, payload)
	stop()
	result := prepared.result
	result.ExecutionMode = "vehicle"
	var contextInfo struct {
		Observation json.RawMessage `json:"observation"`
	}
	if json.Unmarshal(snapshot, &contextInfo) != nil {
		writeError(w, 502, "车辆观察数据不可读")
		return
	}
	result.Observation = contextInfo.Observation
	if runErr != nil {
		result.Error = runErr.Error()
		writeJSON(w, 200, map[string]any{"result": result})
		return
	}
	if json.Unmarshal(output, &result) != nil {
		writeError(w, 502, "执行器返回了无效结果")
		return
	}
	if !result.OK {
		writeJSON(w, 200, map[string]any{"result": result})
		return
	}
	result.Phase = "validation"
	if result.KeepCurrentRoute {
		result.Phase = "kept"
		writeJSON(w, 200, map[string]any{"result": result})
		return
	}
	if result.States == nil {
		result.OK = !result.Baseline.OK
		if result.OK {
			result.Phase = "unreachable"
		} else {
			result.Error = "算法返回无路可达，但同一快照上的 Dijkstra 找到了路线"
		}
		writeJSON(w, 200, map[string]any{"result": result})
		return
	}
	if len(result.States) < 2 || len(result.States) > 10000 {
		writeError(w, 422, "无效的路线状态数量")
		return
	}
	states := make([]string, len(result.States))
	for i, value := range result.States {
		states[i] = strconv.Itoa(value)
	}
	routeFile := filepath.Join(dir, "route.geojson")
	_, checked := s.agentSessionCommand(w, r, "algorithm-candidate", r.PathValue("session"), vehicle, version, strings.Join(states, ","), routeFile)
	if checked == nil {
		return
	}
	var candidate struct {
		OK          bool    `json:"ok"`
		CandidateID string  `json:"candidateId"`
		TimeS       float64 `json:"timeS"`
		LengthM     float64 `json:"lengthM"`
		Edges       []int   `json:"edges"`
	}
	if json.Unmarshal(checked, &candidate) != nil || !candidate.OK || candidate.CandidateID == "" {
		writeError(w, 502, "车辆路径校验返回了无效结果")
		return
	}
	result.Phase = "verified"
	result.GeoJSON, err = os.ReadFile(routeFile)
	if err != nil || !json.Valid(result.GeoJSON) {
		writeError(w, 502, "无法读取已验证的车辆路径")
		return
	}
	result.TimeS, result.LengthM, result.Edges = candidate.TimeS, candidate.LengthM, len(candidate.Edges)
	writeJSON(w, 200, map[string]any{"result": result, "candidate": checked})
}
