package main

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestALTRouteMetrics(t *testing.T) {
	response := parseRoute("route=ok\nalgorithm=alt\nlandmark_count=8\nlandmark_bytes=123456\nlandmark_preprocess_ms=12.125\nlandmark_reused=0\n")
	if response.LandmarkCount != 8 || response.LandmarkBytes != 123456 || response.LandmarkPreprocessMs != 12.125 || response.LandmarkReused {
		t.Fatalf("lost ALT preprocessing metrics: %+v", response)
	}
	encoded, err := json.Marshal(response)
	if err != nil || !strings.Contains(string(encoded), `"landmarkReused":false`) {
		t.Fatalf("cold preprocessing must remain explicit: %s, %v", encoded, err)
	}
	if !parseRoute("route=ok\nalgorithm=alt\nlandmark_reused=1\n").LandmarkReused {
		t.Fatal("lost warm index reuse flag")
	}
}
