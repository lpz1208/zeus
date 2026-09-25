package main

import (
	"math"
	"strings"
	"testing"
)

func TestRecoverySettingsValidationAndCLI(t *testing.T) {
	lon, lat := 114.0, 30.0
	request := SimulateRequest{FromLon: &lon, FromLat: &lat, ToLon: &lon, ToLat: &lat,
		RerouteRecoveryIntervalSeconds: 5, RerouteMinGainSeconds: 12, RerouteCooldownSeconds: 30}
	args, err := buildSimulateArgs(request)
	if err != nil {
		t.Fatal(err)
	}
	command := strings.Join(args, " ")
	for _, flag := range []string{"--reroute-recovery-interval 5", "--reroute-min-gain 12", "--reroute-cooldown 30"} {
		if !strings.Contains(command, flag) {
			t.Fatalf("missing %s: %s", flag, command)
		}
	}
	for _, value := range []float64{-1, 3601, math.NaN(), math.Inf(1)} {
		for index := 0; index < 3; index++ {
			values := []float64{0, 0, 0}
			values[index] = value
			if err := validateRecoverySettings(values[0], values[1], values[2]); err == nil {
				t.Fatalf("accepted invalid recovery setting %v", values)
			}
		}
	}
}

func TestRecoverySettingsSessionAndSnapshot(t *testing.T) {
	s, log := newAgentSessionTestServer(t)
	defer s.Close()
	path := "/api/maps/m1/agent/sessions"
	code, created := agentSessionRequest(t, s, "POST", path,
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4}],"rerouteRecoveryIntervalSeconds":5,"rerouteMinGainSeconds":12,"rerouteCooldownSeconds":30}`)
	if code != 200 {
		t.Fatal(code, created)
	}
	if !strings.Contains(fakeSessionLog(t, log), "\t5\t12\t30") {
		t.Fatal("recovery settings not passed to native worker")
	}
	session := created["sessionId"].(string)
	code, saved := agentSessionRequest(t, s, "POST", path+"/"+session+"/snapshots", `{}`)
	if code != 200 {
		t.Fatal(code, saved)
	}
	artifact, err := s.loadAgentSnapshot(saved["snapshotId"].(string))
	if err != nil {
		t.Fatal(err)
	}
	if artifact.Request.RerouteRecoveryIntervalSeconds != 5 || artifact.Request.RerouteMinGainSeconds != 12 || artifact.Request.RerouteCooldownSeconds != 30 {
		t.Fatal("snapshot lost recovery settings")
	}
	before := fakeSessionLog(t, log)
	code, _ = agentSessionRequest(t, s, "POST", path,
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4}],"rerouteCooldownSeconds":-1}`)
	if code != 400 || fakeSessionLog(t, log) != before {
		t.Fatal("invalid settings reached native worker")
	}
}
