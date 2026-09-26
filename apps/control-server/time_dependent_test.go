package main

import (
	"math"
	"strings"
	"testing"
)

func TestTimeDependentRouteProtocol(t *testing.T) {
	request := RouteWorkerRequest{Algorithm: "tddijkstra", MaxDistance: 100,
		DepartureTimeSeconds: 12.125, SpeedChanges: []RouteSpeedChange{{7, 15.5, .25}, {7, 40, 1}}}
	encoded, err := encodeRouteWorkerRequest(request)
	if err != nil {
		t.Fatal(err)
	}
	fields := strings.Split(encoded, "\t")
	if len(fields) != 11 || fields[9] != "12.125" || fields[10] != "7,15.5,0.25;7,40,1" {
		t.Fatalf("temporal route fields lost precision: %q", encoded)
	}
	request.Algorithm = "dijkstra"
	if _, err := encodeRouteWorkerRequest(request); err == nil {
		t.Fatal("static algorithm ignored a forecast")
	}
	request.DepartureTimeSeconds, request.SpeedChanges = 0, nil
	encoded, err = encodeRouteWorkerRequest(request)
	if err != nil || len(strings.Split(encoded, "\t")) != 9 {
		t.Fatal("legacy route protocol changed", err)
	}
	response := parseRoute("route=ok\nalgorithm=tddijkstra\ndeparture_time_s=12.125\narrival_time_s=42.5\ntime_s=30.375\n")
	if response.DepartureTimeSeconds == nil || *response.DepartureTimeSeconds != 12.125 ||
		response.ArrivalTimeSeconds == nil || *response.ArrivalTimeSeconds != 42.5 || response.TimeS != 30.375 {
		t.Fatalf("lost time-dependent response fields: %+v", response)
	}
}

func TestTimeDependentRouteRejectsInvalidForecast(t *testing.T) {
	for _, departure := range []float64{-1, 1e10, math.NaN(), math.Inf(1)} {
		if err := validateTimeDependentRequest("tddijkstra", departure, nil); err == nil {
			t.Fatal("accepted invalid departure", departure)
		}
	}
	for _, change := range []RouteSpeedChange{{0, -1, 1}, {0, 1e10, 1}, {0, 0, 0}, {0, 0, 3.1}, {0, 0, math.NaN()}} {
		if err := validateTimeDependentRequest("tddijkstra", 0, []RouteSpeedChange{change}); err == nil {
			t.Fatal("accepted invalid speed change", change)
		}
	}
}
