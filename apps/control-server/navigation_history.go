package main

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const navigationHistoryLimit = 100
const navigationHistoryMaxBytes = 16 << 20

// Records describe the Web loop's last confirmed stages. Only the embedded
// native snapshot is authoritative for restoring vehicle state.
type navigationHistory struct {
	ID        string                `json:"id"`
	Revision  uint64                `json:"revision"`
	MapID     string                `json:"mapId"`
	SessionID string                `json:"sessionId"`
	CreatedAt time.Time             `json:"createdAt"`
	UpdatedAt time.Time             `json:"updatedAt"`
	Source    string                `json:"source"`
	Records   []json.RawMessage     `json:"records"`
	Snapshot  agentSnapshotArtifact `json:"snapshot"`
	Finished  bool                  `json:"finished"`
}

type navigationHistorySummary struct {
	ID        string    `json:"id"`
	Revision  uint64    `json:"revision"`
	SessionID string    `json:"sessionId"`
	UpdatedAt time.Time `json:"updatedAt"`
	Tick      uint64    `json:"tick"`
	Records   int       `json:"records"`
	Finished  bool      `json:"finished"`
}

func (h navigationHistory) summary() navigationHistorySummary {
	return navigationHistorySummary{h.ID, h.Revision, h.SessionID, h.UpdatedAt, h.Snapshot.Tick, len(h.Records), h.Finished}
}

func validNavigationID(id string) bool {
	if !strings.HasPrefix(id, "nav_") || len(id) != 20 {
		return false
	}
	_, err := hex.DecodeString(id[4:])
	return err == nil
}

func (s *Server) navigationHistoryDir(mapID string) string {
	return filepath.Join(s.config.DataDir, "maps", mapID, "navigation-history")
}

func (s *Server) readNavigationHistory(mapID, id string) (navigationHistory, error) {
	var history navigationHistory
	if !validNavigationID(id) {
		return history, os.ErrNotExist
	}
	file, err := os.Open(filepath.Join(s.navigationHistoryDir(mapID), id+".json"))
	if err != nil {
		return history, err
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, navigationHistoryMaxBytes+1))
	if err != nil {
		return history, err
	}
	if len(data) > navigationHistoryMaxBytes || json.Unmarshal(data, &history) != nil || history.ID != id || history.MapID != mapID {
		return history, errors.New("导航历史文件损坏")
	}
	return history, nil
}

// The session middleware excludes concurrent step/actions while the native
// snapshot is captured. CAS prevents a stale page from overwriting a checkpoint.
func (s *Server) handleSaveNavigationHistory(w http.ResponseWriter, r *http.Request) {
	var request struct {
		ID                  string            `json:"id"`
		Revision            uint64            `json:"revision"`
		BasedOnStateVersion uint64            `json:"basedOnStateVersion"`
		Source              string            `json:"source"`
		Records             []json.RawMessage `json:"records"`
	}
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, navigationHistoryMaxBytes))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&request) != nil || decoder.Decode(new(any)) != io.EOF ||
		request.BasedOnStateVersion == 0 || strings.TrimSpace(request.Source) == "" || len(request.Source) > 32768 || len(request.Records) > 200 ||
		(request.ID != "" && !validNavigationID(request.ID)) {
		writeError(w, 400, "无效导航记录：最多 200 条记录、32 KiB 源码和 16 MiB 请求")
		return
	}
	seen := make(map[string]bool)
	for _, raw := range request.Records {
		var record struct {
			ID              string   `json:"id"`
			Status          string   `json:"status"`
			SessionID       string   `json:"sessionId"`
			MapID           string   `json:"mapId"`
			Source          string   `json:"source"`
			StateVersion    uint64   `json:"stateVersion"`
			SimulationTimeS float64  `json:"simulationTimeS"`
			ComputeMs       *float64 `json:"computeMs"`
			Edges           []uint32 `json:"edges"`
			Logs            []string `json:"logs"`
			Reason          string   `json:"reason"`
			Error           string   `json:"error"`
		}
		if json.Unmarshal(raw, &record) != nil || record.ID == "" || len(record.ID) > 128 || seen[record.ID] ||
			record.SessionID != r.PathValue("session") || record.MapID != r.PathValue("id") ||
			record.StateVersion == 0 || record.StateVersion > request.BasedOnStateVersion || record.SimulationTimeS < 0 ||
			len(record.Source) > 32768 || len(record.Edges) > 10000 || len(record.Logs) > 100 {
			writeError(w, 400, "导航记录必须属于当前地图和会话，且编号唯一")
			return
		}
		switch record.Status {
		case "planning", "planned", "cancelled", "queued", "applied", "kept", "error":
		default:
			writeError(w, 400, "无效导航记录阶段")
			return
		}
		seen[record.ID] = true
	}
	s.navigationHistoryMu.Lock()
	defer s.navigationHistoryMu.Unlock()
	mapID, sessionID := r.PathValue("id"), r.PathValue("session")
	dir := s.navigationHistoryDir(mapID)
	if err := os.MkdirAll(dir, 0700); err != nil {
		writeError(w, 500, "无法创建导航历史目录")
		return
	}
	var history navigationHistory
	if request.ID == "" {
		if request.Revision != 0 {
			writeError(w, 409, "新导航记录的版本必须为 0")
			return
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			writeError(w, 500, "无法读取导航历史")
			return
		}
		count := 0
		for _, entry := range entries {
			if !entry.IsDir() && validNavigationID(strings.TrimSuffix(entry.Name(), ".json")) {
				count++
			}
		}
		if count >= navigationHistoryLimit {
			writeError(w, 409, "导航历史已达 100 次上限，请删除不需要的历史后继续")
			return
		}
		history = navigationHistory{ID: newID("nav"), MapID: mapID, SessionID: sessionID, CreatedAt: time.Now().UTC()}
	} else {
		var err error
		history, err = s.readNavigationHistory(mapID, request.ID)
		if err != nil {
			writeError(w, 404, "导航历史不存在或不可读")
			return
		}
		if history.SessionID != sessionID || history.Revision != request.Revision {
			writeError(w, 409, "导航历史版本或会话不匹配，请刷新历史")
			return
		}
	}
	entry, _ := s.agentSessions.get(sessionID)
	_, observed := s.agentSessionCommand(w, r, "observe", sessionID, "hot")
	if observed == nil {
		return
	}
	var state sessionStateFields
	if json.Unmarshal(observed, &state) != nil {
		writeError(w, 502, "无法读取车辆状态")
		return
	}
	if state.StateVersion != request.BasedOnStateVersion || (!state.Paused && !state.Finished) {
		writeError(w, 409, "只能保存当前暂停边界，请重新读取车辆状态")
		return
	}
	snapshotID := newID("snp")
	_, payload := s.agentSessionCommand(w, r, "snapshot", sessionID, snapshotID)
	if payload == nil {
		return
	}
	// Keep a single atomic durable artifact; do not accumulate process-local
	// snapshots on every auto-navigation checkpoint.
	defer func() {
		_, _ = s.sessionWorkers.Command(context.Background(), entry.runtime, "drop-snapshot", snapshotID)
	}()
	var snapshot struct {
		Tick         uint64              `json:"tick"`
		StateVersion uint64              `json:"stateVersion"`
		Actions      []agentReplayAction `json:"actions"`
	}
	if json.Unmarshal(payload, &snapshot) != nil || snapshot.StateVersion != state.StateVersion {
		writeError(w, 409, "保存期间车辆状态发生变化")
		return
	}
	history.Revision++
	history.UpdatedAt = time.Now().UTC()
	history.Source, history.Records, history.Finished = request.Source, request.Records, state.Finished
	if history.Records == nil {
		history.Records = []json.RawMessage{}
	}
	history.Snapshot = agentSnapshotArtifact{
		FormatVersion: agentSnapshotFormatVersion, SnapshotID: snapshotID, MapID: mapID, SourceSessionID: sessionID,
		CreatedAt: history.UpdatedAt, Tick: snapshot.Tick, StateVersion: snapshot.StateVersion, StepSecond: entry.stepSecond,
		DecisionPending: entry.activeDecision != "", Request: entry.request, AppliedActions: snapshot.Actions,
	}
	if err := sealAgentSnapshot(&history.Snapshot, entry.mapRevision); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	data, err := json.MarshalIndent(history, "", "  ")
	if err != nil || len(data) > navigationHistoryMaxBytes {
		writeError(w, 413, "导航记录与快照超过 16 MiB，请导出后开始新记录")
		return
	}
	if err := saveJSONFile(filepath.Join(dir, history.ID+".json"), history); err != nil {
		writeError(w, 500, "导航历史保存失败，已保留上一次边界")
		return
	}
	writeJSON(w, 200, history.summary())
}

func (s *Server) handleNavigationHistory(w http.ResponseWriter, r *http.Request) {
	mapID := r.PathValue("id")
	if _, err := s.mapRecord(mapID); err != nil {
		writeError(w, 404, "地图不存在")
		return
	}
	s.navigationHistoryMu.Lock()
	defer s.navigationHistoryMu.Unlock()
	if id := r.PathValue("history"); id != "" {
		history, err := s.readNavigationHistory(mapID, id)
		if err != nil {
			writeError(w, 404, "导航历史不存在或不可读")
			return
		}
		if r.Method == http.MethodDelete {
			if err := os.Remove(filepath.Join(s.navigationHistoryDir(mapID), id+".json")); err != nil {
				writeError(w, 500, "删除导航历史失败")
				return
			}
			writeJSON(w, 200, map[string]bool{"deleted": true})
			return
		}
		writeJSON(w, 200, history)
		return
	}
	entries, err := os.ReadDir(s.navigationHistoryDir(mapID))
	if err != nil && !os.IsNotExist(err) {
		writeError(w, 500, "无法读取导航历史")
		return
	}
	items := []navigationHistorySummary{}
	for _, entry := range entries {
		id := strings.TrimSuffix(entry.Name(), ".json")
		if entry.IsDir() || !validNavigationID(id) {
			continue
		}
		history, err := s.readNavigationHistory(mapID, id)
		if err != nil {
			writeError(w, 500, "导航历史文件不可读")
			return
		}
		items = append(items, history.summary())
	}
	sort.Slice(items, func(i, j int) bool { return items[i].UpdatedAt.After(items[j].UpdatedAt) })
	writeJSON(w, 200, items)
}

func (s *Server) handleRestoreNavigationHistory(w http.ResponseWriter, r *http.Request) {
	if _, err := s.mapRecord(r.PathValue("id")); err != nil {
		writeError(w, 404, "地图不存在")
		return
	}
	var request struct {
		Revision uint64 `json:"revision"`
	}
	if decodeJSON(r, &request) != nil || request.Revision == 0 {
		writeError(w, 400, "恢复时必须提供导航历史版本")
		return
	}
	s.navigationHistoryMu.Lock()
	history, err := s.readNavigationHistory(r.PathValue("id"), r.PathValue("history"))
	s.navigationHistoryMu.Unlock()
	if err != nil {
		writeError(w, 404, "导航历史不存在或不可读")
		return
	}
	if request.Revision != history.Revision {
		writeError(w, 409, "导航历史已更新，请刷新后恢复")
		return
	}
	// Restore into a new paused session. Neither archived UI records nor source
	// are interpreted as actions; replay uses only the native action log.
	s.restoreAgentArtifact(w, r, history.Snapshot)
}
