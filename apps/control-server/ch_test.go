package main

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestCHRouteMetrics(t *testing.T) {
	response := parseRoute("route=ok\nalgorithm=ch\neffective_algorithm=ch\nch_shortcuts=125\nch_core_states=25\nch_bytes=65536\nch_preprocess_ms=10.125\nch_reused=0\n")
	if response.CHShortcuts != 125 || response.CHCoreStates != 25 || response.CHBytes != 65536 || response.CHPreprocessMs != 10.125 || response.CHReused {
		t.Fatalf("lost CH metrics: %+v", response)
	}
	encoded, err := json.Marshal(response)
	if err != nil || !strings.Contains(string(encoded), `"chReused":false`) {
		t.Fatalf("lost cold initialization flag: %s, %v", encoded, err)
	}
	warm := parseRoute("route=ok\nalgorithm=ch\nch_reused=1\n")
	if !warm.CHReused {
		t.Fatal("lost CH reuse flag")
	}
	fallback := parseRoute("route=ok\nalgorithm=ch\neffective_algorithm=bidijkstra\nfallback_reason=ch_dynamic_weights\n")
	if fallback.Algorithm != "ch" || fallback.EffectiveAlgorithm != "bidijkstra" || fallback.FallbackReason != "ch_dynamic_weights" {
		t.Fatalf("CH fallback must expose the actual algorithm: %+v", fallback)
	}
}
