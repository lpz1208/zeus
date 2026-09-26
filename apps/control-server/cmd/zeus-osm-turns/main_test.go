package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/qedus/osmpbf"
)

func TestParseRestriction(t *testing.T) {
	relation := &osmpbf.Relation{
		ID:   9,
		Tags: map[string]string{"type": "restriction", "restriction": "no_left_turn"},
		Members: []osmpbf.Member{
			{ID: 10, Type: osmpbf.WayType, Role: "from"},
			{ID: 20, Type: osmpbf.NodeType, Role: "via"},
			{ID: 30, Type: osmpbf.WayType, Role: "to"},
		},
	}
	result, ok, reason := parseRestriction(relation)
	if !ok || reason != "" || result.fromWay != 10 || result.viaNode != 20 ||
		result.toWay != 30 || result.kind != "no" {
		t.Fatalf("unexpected parsed restriction: %#v, %v, %q", result, ok, reason)
	}
}

func TestRestrictionExceptionsAndViaWays(t *testing.T) {
	excepted := &osmpbf.Relation{
		Tags: map[string]string{
			"type": "restriction", "restriction": "only_right_turn", "except": "bus;motorcar",
		},
	}
	if _, ok, reason := parseRestriction(excepted); ok || reason != "motorcar_excepted" {
		t.Fatalf("motorcar exception was not honored: %v %q", ok, reason)
	}
	viaWay := &osmpbf.Relation{
		Tags: map[string]string{"type": "restriction", "restriction": "no_straight_on"},
		Members: []osmpbf.Member{
			{ID: 1, Type: osmpbf.WayType, Role: "from"},
			{ID: 2, Type: osmpbf.WayType, Role: "via"},
			{ID: 3, Type: osmpbf.WayType, Role: "to"},
		},
	}
	if rule, ok, reason := parseRestriction(viaWay); !ok || len(rule.viaWays) != 1 || rule.viaWays[0] != 2 {
		t.Fatalf("via-way relation was not retained: %+v %v %q", rule, ok, reason)
	}
}

func TestParseBounds(t *testing.T) {
	area, err := parseBounds("113.7,29.9,115.1,31.4")
	if err != nil || !area.contains(point{114.3, 30.5}) || area.contains(point{116, 30.5}) {
		t.Fatalf("unexpected bounds: %#v, %v", area, err)
	}
}

func TestResolveViaWays(t *testing.T) {
	ways := map[int64][]int64{1: {10, 20}, 2: {20, 30, 40}, 3: {40, 50}, 4: {50, 60}}
	rule := restriction{fromWay: 1, toWay: 4, viaWays: []int64{3, 2}}
	if !resolveViaWays(&rule, ways) || rule.viaNode != 20 || rule.viaWays[0] != 2 || rule.viaWays[1] != 3 {
		t.Fatalf("unordered via members lost their unique topology: %+v", rule)
	}
	ways[3] = []int64{80, 90}
	if resolveViaWays(&rule, ways) {
		t.Fatal("accepted disconnected via chain")
	}
	ways[3] = []int64{20, 40, 50}
	if resolveViaWays(&rule, ways) {
		t.Fatal("guessed an ambiguous way connection")
	}
}

func TestWriteViaWayCSV(t *testing.T) {
	path := filepath.Join(t.TempDir(), "turns.csv")
	rules := []restriction{{fromWay: 1, viaNode: 20, toWay: 4, viaWays: []int64{2, 3}, kind: "only"},
		{fromWay: 5, viaNode: 20, toWay: 6, kind: "no"}, {fromWay: 7, viaNode: 99, toWay: 8, kind: "no"}}
	written, missing, err := writeCSV(path, rules, map[int64]point{20: {114, 30}}, bounds{})
	if err != nil || written != 2 || missing != 1 {
		t.Fatalf("write: %d %d %v", written, missing, err)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(content), "1,114.0000000,30.0000000,4,only_via,2;3\n") ||
		!strings.Contains(string(content), "5,114.0000000,30.0000000,6,no\n") {
		t.Fatalf("unexpected CSV: %s", content)
	}
}
