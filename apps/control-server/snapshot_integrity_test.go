package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func integrityFixture(t *testing.T) (*Server, string, string, string) {
	t.Helper()
	s, log := newAgentSessionTestServer(t)
	t.Cleanup(s.Close)
	code, created := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/sessions", `{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}]}`)
	if code != 200 {
		t.Fatal(code, created)
	}
	session := created["sessionId"].(string)
	base := "/api/maps/m1/agent/sessions/" + session
	code, saved := agentSessionRequest(t, s, "POST", base+"/snapshots", `{}`)
	if code != 200 || saved["formatVersion"] != float64(3) || saved["integrity"] != "verified" {
		t.Fatal(code, saved)
	}
	return s, session, saved["snapshotId"].(string), log
}

func TestSnapshotChecksumRejectsMutationDespiteWarmCache(t *testing.T) {
	for _, field := range []string{"tick", "mapId", "checksum", "replayContract"} {
		t.Run(field, func(t *testing.T) {
			s, _, id, log := integrityFixture(t)
			path, _ := s.agentSnapshotPath(id)
			bytes, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			var value map[string]any
			if err := json.Unmarshal(bytes, &value); err != nil {
				t.Fatal(err)
			}
			switch field {
			case "tick":
				value[field] = 123
			case "mapId":
				value[field] = "other"
			default:
				value[field] = "changed"
			}
			data, _ := json.Marshal(value)
			if err := os.WriteFile(path, data, 0600); err != nil {
				t.Fatal(err)
			}
			before := fakeSessionLog(t, log)
			code, response := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/snapshots/"+id+"/restore", `{}`)
			if code != 409 {
				t.Fatal(code, response)
			}
			if fakeSessionLog(t, log) != before {
				t.Fatal("corrupt snapshot reached native worker")
			}
		})
	}
}

func TestSnapshotPinnedMapAndRevisionMismatch(t *testing.T) {
	s, session, id, log := integrityFixture(t)
	entry, _ := s.agentSessions.get(session)
	original := filepath.Join(s.mapsDir(), "m1", "map.zmap")
	if entry.runtime == original || !validSHA256(entry.mapRevision) {
		t.Fatal("session map was not pinned")
	}
	old, err := os.ReadFile(entry.runtime)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(original, []byte("replacement map v2"), 0600); err != nil {
		t.Fatal(err)
	}
	pinned, _ := os.ReadFile(entry.runtime)
	if string(pinned) != string(old) {
		t.Fatal("active session map changed")
	}
	code, saved := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/sessions/"+session+"/snapshots", `{}`)
	if code != 200 || saved["mapRevision"] != entry.mapRevision {
		t.Fatal("snapshot bound to replacement map", code, saved)
	}
	before := fakeSessionLog(t, log)
	code, response := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/snapshots/"+id+"/restore", `{}`)
	if code != 400 || !strings.Contains(response["error"].(string), "map revision") {
		t.Fatal(code, response)
	}
	if fakeSessionLog(t, log) != before {
		t.Fatal("mismatched map started replay")
	}
	if err := os.WriteFile(original, old, 0600); err != nil {
		t.Fatal(err)
	}
	code, response = agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/snapshots/"+id+"/restore", `{}`)
	if code != 200 || response["integrity"] != "verified" {
		t.Fatal(code, response)
	}
}

func TestSnapshotLegacyCompatibilityAndCanonicalChecksum(t *testing.T) {
	s, _, id, _ := integrityFixture(t)
	artifact, err := s.loadAgentSnapshot(id)
	if err != nil {
		t.Fatal(err)
	}
	// JSON whitespace/key order must not change the semantic checksum.
	path, _ := s.agentSnapshotPath(id)
	data, _ := json.Marshal(artifact)
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.loadAgentSnapshot(id); err != nil {
		t.Fatal(err)
	}
	for _, version := range []int{1, 2} {
		artifact.FormatVersion = version
		artifact.Checksum, artifact.MapRevision, artifact.ReplayContract = "", "", ""
		if err := saveJSONFile(path, artifact); err != nil {
			t.Fatal(err)
		}
		code, response := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/snapshots/"+id+"/restore", `{}`)
		if code != 200 || response["integrity"] != "legacy_unverified" {
			t.Fatal(code, response)
		}
	}
}

func TestSnapshotIntegrityAppliedToEmbeddedReplay(t *testing.T) {
	s, _, id, log := integrityFixture(t)
	artifact, err := s.loadAgentSnapshot(id)
	if err != nil {
		t.Fatal(err)
	}
	artifact.Request.Vehicles[0].ToLon = 999
	before := fakeSessionLog(t, log)
	if _, _, err := s.replayAgentSnapshot(context.Background(), artifact, "bad-history"); err == nil {
		t.Fatal("embedded modified snapshot accepted")
	}
	if before != fakeSessionLog(t, log) {
		t.Fatal("invalid history reached worker")
	}
}

func TestSnapshotRejectsUnsupportedReplayContract(t *testing.T) {
	s, _, id, log := integrityFixture(t)
	artifact, err := s.loadAgentSnapshot(id)
	if err != nil {
		t.Fatal(err)
	}
	artifact.ReplayContract = "incompatible-engine-contract"
	artifact.Checksum, err = snapshotDigest(artifact)
	if err != nil {
		t.Fatal(err)
	}
	path, _ := s.agentSnapshotPath(id)
	if err := saveJSONFile(path, artifact); err != nil {
		t.Fatal(err)
	}
	before := fakeSessionLog(t, log)
	code, response := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/snapshots/"+id+"/restore", `{}`)
	if code != 409 || !strings.Contains(response["error"].(string), "contract") {
		t.Fatal(code, response)
	}
	if fakeSessionLog(t, log) != before {
		t.Fatal("incompatible contract reached native worker")
	}
}
