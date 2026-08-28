package main

import (
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
	if _, ok, reason := parseRestriction(viaWay); ok || reason != "via_way" {
		t.Fatalf("via-way relation was not classified: %v %q", ok, reason)
	}
}

func TestParseBounds(t *testing.T) {
	area, err := parseBounds("113.7,29.9,115.1,31.4")
	if err != nil || !area.contains(point{114.3, 30.5}) || area.contains(point{116, 30.5}) {
		t.Fatalf("unexpected bounds: %#v, %v", area, err)
	}
}
