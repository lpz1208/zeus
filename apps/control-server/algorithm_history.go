package main

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// Persist only results produced by our executor, never client-supplied verdicts.
type AlgorithmExperiment struct {
	ID        string              `json:"id"`
	CreatedAt time.Time           `json:"createdAt"`
	Request   AlgorithmLabRequest `json:"request"`
	Result    AlgorithmLabResult  `json:"result"`
}

type AlgorithmExperimentSummary struct {
	ExecutionMode string    `json:"executionMode"`
	ID            string    `json:"id"`
	CreatedAt     time.Time `json:"createdAt"`
	OK            bool      `json:"ok"`
	Phase         string    `json:"phase"`
	CodeRevision  string    `json:"codeRevision"`
	SnapshotID    string    `json:"snapshotId"`
	TimeS         float64   `json:"timeS"`
	LengthM       float64   `json:"lengthM"`
	ComputeMs     float64   `json:"computeMs"`
	ExpandedNodes int       `json:"expandedNodes"`
}

func (s *Server) algorithmHistoryDir(mapID string) string {
	return filepath.Join(s.config.DataDir, "maps", mapID, "algorithm-experiments")
}

func validAlgorithmRunID(id string) bool {
	if !strings.HasPrefix(id, "run_") || len(id) != 20 {
		return false
	}
	_, err := hex.DecodeString(id[4:])
	return err == nil
}

func (s *Server) finishAlgorithmRun(w http.ResponseWriter, r *http.Request, request AlgorithmLabRequest, result AlgorithmLabResult) {
	if r.Context().Err() != nil {
		return
	}
	writeJSON(w, 200, s.saveAlgorithmExperiment(request, result))
}

func (s *Server) saveAlgorithmExperiment(request AlgorithmLabRequest, result AlgorithmLabResult) AlgorithmLabResult {
	s.algorithmHistoryMu.Lock()
	dir := s.algorithmHistoryDir(result.MapID)
	err := os.MkdirAll(dir, 0700)
	if err == nil {
		var entries []os.DirEntry
		entries, err = os.ReadDir(dir)
		count := 0
		for _, entry := range entries {
			if strings.HasSuffix(entry.Name(), ".json") && !entry.IsDir() {
				count++
			}
		}
		if err == nil && count >= 100 {
			err = errors.New("当前地图已有 100 次实验，请删除不需要的记录后重试保存")
		}
	}
	if err == nil {
		result.RunID = newID("run")
		err = saveJSONFile(filepath.Join(dir, result.RunID+".json"), AlgorithmExperiment{
			ID: result.RunID, CreatedAt: time.Now().UTC(), Request: request, Result: result,
		})
	}
	if err != nil {
		result.RunID = ""
		result.StorageError = "实验未保存：" + err.Error()
	}
	s.algorithmHistoryMu.Unlock()
	return result
}

func (s *Server) handleAlgorithmHistory(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, 404, "地图不存在")
		return
	}
	s.algorithmHistoryMu.Lock()
	defer s.algorithmHistoryMu.Unlock()
	dir := s.algorithmHistoryDir(record.ID)
	if runID := r.PathValue("run"); runID != "" {
		if !validAlgorithmRunID(runID) {
			writeError(w, 400, "无效实验编号")
			return
		}
		path := filepath.Join(dir, runID+".json")
		if r.Method == http.MethodDelete {
			if err = os.Remove(path); err != nil {
				if os.IsNotExist(err) {
					writeError(w, 404, "实验不存在")
				} else {
					writeError(w, 500, "删除实验失败")
				}
				return
			}
			writeJSON(w, 200, map[string]bool{"ok": true})
			return
		}
		data, err := os.ReadFile(path)
		if err != nil {
			writeError(w, 404, "实验不存在")
			return
		}
		var experiment AlgorithmExperiment
		if json.Unmarshal(data, &experiment) != nil {
			writeError(w, 500, "实验文件损坏")
			return
		}
		writeJSON(w, 200, experiment)
		return
	}
	entries, err := os.ReadDir(dir)
	if err != nil && !os.IsNotExist(err) {
		writeError(w, 500, "无法读取实验记录")
		return
	}
	items := []AlgorithmExperimentSummary{}
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") || !validAlgorithmRunID(strings.TrimSuffix(entry.Name(), ".json")) {
			continue
		}
		data, err := os.ReadFile(filepath.Join(dir, entry.Name()))
		if err != nil {
			writeError(w, 500, "无法读取实验文件")
			return
		}
		var experiment AlgorithmExperiment
		if json.Unmarshal(data, &experiment) != nil {
			writeError(w, 500, "实验文件损坏")
			return
		}
		result := experiment.Result
		items = append(items, AlgorithmExperimentSummary{ID: experiment.ID, CreatedAt: experiment.CreatedAt,
			OK: result.OK, Phase: result.Phase, CodeRevision: result.CodeRevision, SnapshotID: result.SnapshotID,
			TimeS: result.TimeS, LengthM: result.LengthM, ComputeMs: result.ComputeMs, ExpandedNodes: result.ExpandedNodes, ExecutionMode: result.ExecutionMode})
	}
	sort.Slice(items, func(i, j int) bool { return items[i].CreatedAt.After(items[j].CreatedAt) })
	writeJSON(w, 200, items)
}
