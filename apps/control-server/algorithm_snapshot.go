package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type algorithmSnapshot struct {
	dir, graph, runner string
	args               []string
	result             AlgorithmLabResult
}

func (s *Server) prepareAlgorithm(ctx context.Context, record MapRecord, req AlgorithmLabRequest) (*algorithmSnapshot, error) {
	dir, err := os.MkdirTemp("", "zeus-algorithm-")
	if err != nil {
		return nil, errors.New("无法创建执行目录")
	}
	prepared := &algorithmSnapshot{dir: dir, graph: filepath.Join(dir, "context.json"), runner: filepath.Join(dir, "runner.py")}
	ok := false
	defer func() {
		if !ok {
			os.RemoveAll(dir)
		}
	}()
	prepared.args = []string{record.Runtime, "--lon", strconv.FormatFloat(*req.FromLon, 'g', -1, 64), "--lat", strconv.FormatFloat(*req.FromLat, 'g', -1, 64), "--dest-lon", strconv.FormatFloat(*req.ToLon, 'g', -1, 64), "--dest-lat", strconv.FormatFloat(*req.ToLat, 'g', -1, 64)}
	if len(req.ClosedEdges) > 0 {
		edges := make([]string, len(req.ClosedEdges))
		for i, e := range req.ClosedEdges {
			edges[i] = strconv.Itoa(e)
		}
		prepared.args = append(prepared.args, "--closed", strings.Join(edges, ","))
	}
	if _, err = s.runMapCommand(ctx, append(append([]string{"algorithm-context"}, prepared.args...), "--output", prepared.graph)...); err != nil {
		return nil, fmt.Errorf("无法创建路网快照：%w", err)
	}
	if err = prepared.initialize(record.ID, req.Source); err != nil {
		return nil, err
	}
	ok = true
	return prepared, nil
}

func (prepared *algorithmSnapshot) initialize(mapID, source string) error {
	file, err := os.Open(prepared.graph)
	if err != nil {
		return errors.New("路网快照不可读")
	}
	hasher := sha256.New()
	n, hashErr := io.Copy(hasher, io.LimitReader(file, (128<<20)+1))
	file.Close()
	if hashErr != nil || n > 128<<20 {
		return errors.New("路网快照超过 128 MiB 上限")
	}
	revision := sha256.Sum256([]byte(source))
	prepared.result = AlgorithmLabResult{Phase: "execution", ExecutionMode: "run", MapID: mapID, CodeRevision: hex.EncodeToString(revision[:]), SnapshotID: hex.EncodeToString(hasher.Sum(nil))}
	if err = os.WriteFile(prepared.runner, algorithmRunner, 0600); err != nil {
		return errors.New("无法准备执行器")
	}
	return nil
}

func (s *Server) validateAlgorithmResult(ctx context.Context, prepared *algorithmSnapshot, result AlgorithmLabResult) (AlgorithmLabResult, error) {
	if !result.OK {
		return result, nil
	}
	result.Phase = "validation"
	if result.States == nil {
		result.OK = !result.Baseline.OK
		if result.OK {
			result.Phase = "unreachable"
		} else {
			result.Error = "算法返回无路可达，但同一快照上的 Dijkstra 找到了路线"
		}
		return result, nil
	}
	if len(result.States) > 10000 {
		result.OK = false
		result.Error = "路线超过 10000 个状态"
		return result, nil
	}
	var states strings.Builder
	for _, state := range result.States {
		fmt.Fprintln(&states, state)
	}
	stateFile := filepath.Join(prepared.dir, "states.txt")
	if err := os.WriteFile(stateFile, []byte(states.String()), 0600); err != nil {
		return result, errors.New("无法准备路线校验")
	}
	routeFile := filepath.Join(prepared.dir, "route.geojson")
	checked, err := s.runMapCommand(ctx, append(append([]string{"algorithm-validate"}, prepared.args...), "--states", stateFile, "--output", routeFile)...)
	if err != nil {
		result.OK = false
		result.Error = err.Error()
		return result, nil
	}
	var metrics struct {
		TimeS   float64 `json:"timeS"`
		LengthM float64 `json:"lengthM"`
		Edges   int     `json:"edges"`
	}
	if json.Unmarshal([]byte(checked), &metrics) != nil {
		return result, errors.New("路线校验返回了无效结果")
	}
	result.GeoJSON, err = os.ReadFile(routeFile)
	if err != nil {
		return result, errors.New("无法读取已验证路线")
	}
	result.TimeS, result.LengthM, result.Edges = metrics.TimeS, metrics.LengthM, metrics.Edges
	result.Phase = "verified"
	return result, nil
}
