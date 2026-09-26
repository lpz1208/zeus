package main

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"

	"github.com/qedus/osmpbf"
)

type restriction struct {
	relationID int64
	fromWay    int64
	viaNode    int64
	viaWays    []int64
	toWay      int64
	kind       string
}

type point struct {
	lon float64
	lat float64
}

type bounds struct {
	enabled        bool
	minLon, minLat float64
	maxLon, maxLat float64
}

func (b bounds) contains(p point) bool {
	return !b.enabled ||
		(p.lon >= b.minLon && p.lon <= b.maxLon &&
			p.lat >= b.minLat && p.lat <= b.maxLat)
}

func parseBounds(raw string) (bounds, error) {
	if strings.TrimSpace(raw) == "" {
		return bounds{}, nil
	}
	parts := strings.Split(raw, ",")
	if len(parts) != 4 {
		return bounds{}, errors.New("bbox must be min_lon,min_lat,max_lon,max_lat")
	}
	values := make([]float64, 4)
	for i, part := range parts {
		value, err := strconv.ParseFloat(strings.TrimSpace(part), 64)
		if err != nil {
			return bounds{}, fmt.Errorf("invalid bbox coordinate %q", part)
		}
		values[i] = value
	}
	if values[0] > values[2] || values[1] > values[3] {
		return bounds{}, errors.New("bbox minimum exceeds maximum")
	}
	return bounds{true, values[0], values[1], values[2], values[3]}, nil
}

func containsToken(raw, wanted string) bool {
	for _, token := range strings.FieldsFunc(
		strings.ToLower(raw), func(r rune) bool { return r == ';' || r == ',' || r == ' ' }) {
		if token == wanted {
			return true
		}
	}
	return false
}

func parseRestriction(relation *osmpbf.Relation) (restriction, bool, string) {
	if relation.Tags["type"] != "restriction" {
		return restriction{}, false, "not_restriction"
	}
	value := relation.Tags["restriction:motorcar"]
	if value == "" {
		value = relation.Tags["restriction"]
		if containsToken(relation.Tags["except"], "motorcar") ||
			containsToken(relation.Tags["except"], "motor_vehicle") {
			return restriction{}, false, "motorcar_excepted"
		}
	}
	kind := ""
	if strings.HasPrefix(value, "no_") {
		kind = "no"
	} else if strings.HasPrefix(value, "only_") {
		kind = "only"
	} else {
		return restriction{}, false, "unsupported_type"
	}

	result := restriction{relationID: relation.ID, kind: kind}
	fromCount, viaCount, toCount := 0, 0, 0

	for _, member := range relation.Members {
		switch member.Role {
		case "from":
			if member.Type == osmpbf.WayType {
				result.fromWay = member.ID
				fromCount++
			}
		case "to":
			if member.Type == osmpbf.WayType {
				result.toWay = member.ID
				toCount++
			}
		case "via":
			if member.Type == osmpbf.NodeType {
				result.viaNode = member.ID
				viaCount++
			} else if member.Type == osmpbf.WayType {
				result.viaWays = append(result.viaWays, member.ID)
			}
		}
	}
	if len(result.viaWays) > 16 || (len(result.viaWays) > 0 && viaCount > 0) {
		return restriction{}, false, "invalid_members"
	}
	if fromCount != 1 || toCount != 1 || (viaCount != 1 && len(result.viaWays) == 0) {
		return restriction{}, false, "invalid_members"
	}
	return result, true, ""
}

func decoder(path string) (*os.File, *osmpbf.Decoder, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, nil, err
	}
	result := osmpbf.NewDecoder(file)
	result.SetBufferSize(osmpbf.MaxBlobSize)
	if err := result.Start(runtime.GOMAXPROCS(0)); err != nil {
		file.Close()
		return nil, nil, err
	}
	return file, result, nil
}

func readRestrictions(path string) ([]restriction, map[string]int, error) {
	file, input, err := decoder(path)
	if err != nil {
		return nil, nil, err
	}
	defer file.Close()
	result := []restriction{}
	skipped := map[string]int{}
	for {
		value, decodeErr := input.Decode()
		if decodeErr == io.EOF {
			break
		}
		if decodeErr != nil {
			return nil, nil, decodeErr
		}
		relation, ok := value.(*osmpbf.Relation)
		if !ok || relation.Tags["type"] != "restriction" {
			continue
		}
		parsed, accepted, reason := parseRestriction(relation)
		if accepted {
			result = append(result, parsed)
		} else {
			skipped[reason]++
		}
	}
	sort.Slice(result, func(i, j int) bool { return result[i].relationID < result[j].relationID })
	return result, skipped, nil
}

func readWays(path string, wanted map[int64]struct{}) (map[int64][]int64, error) {
	file, input, err := decoder(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	result := make(map[int64][]int64, len(wanted))
	for len(result) < len(wanted) {
		value, err := input.Decode()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		way, ok := value.(*osmpbf.Way)
		if ok {
			if _, needed := wanted[way.ID]; needed {
				result[way.ID] = way.NodeIDs
			}
		}
	}
	return result, nil
}

// Resolve the unique connected via-way order, independent of relation member
// order. Ambiguous junctions/chains are reported instead of guessed.
func resolveViaWays(rule *restriction, ways map[int64][]int64) bool {
	common := func(a, b int64) int64 {
		nodes := make(map[int64]bool, len(ways[a]))
		for _, id := range ways[a] {
			nodes[id] = true
		}
		var found int64
		for _, id := range ways[b] {
			if nodes[id] {
				if found != 0 && found != id {
					return 0
				}
				found = id
			}
		}
		return found
	}
	candidates := append([]int64(nil), rule.viaWays...)
	sort.Slice(candidates, func(i, j int) bool { return candidates[i] < candidates[j] })
	var chain, chosen []int64
	var firstNode, chosenNode int64
	found, work := 0, 0
	var walk func(int64, int64, uint32)
	walk = func(current, entry int64, used uint32) {
		work++
		if found > 1 || work > 10000 {
			return
		}
		if len(chain) == len(candidates) {
			exit := common(current, rule.toWay)
			if exit != 0 && exit != entry {
				found++
				chosen = append([]int64(nil), chain...)
				chosenNode = firstNode
			}
			return
		}
		for i, next := range candidates {
			if used&(1<<i) != 0 {
				continue
			}
			node := common(current, next)
			if node == 0 || node == entry {
				continue
			}
			if len(chain) == 0 {
				firstNode = node
			}
			chain = append(chain, next)
			walk(next, node, used|(1<<i))
			chain = chain[:len(chain)-1]
		}
	}
	walk(rule.fromWay, 0, 0)
	if found != 1 || work > 10000 {
		return false
	}
	rule.viaWays, rule.viaNode = chosen, chosenNode
	return true
}

func readViaNodes(path string, wanted map[int64]struct{}) (map[int64]point, error) {
	file, input, err := decoder(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	result := make(map[int64]point, len(wanted))
	for len(result) < len(wanted) {
		value, decodeErr := input.Decode()
		if decodeErr == io.EOF {
			break
		}
		if decodeErr != nil {
			return nil, decodeErr
		}
		node, ok := value.(*osmpbf.Node)
		if !ok {
			continue
		}
		if _, exists := wanted[node.ID]; exists {
			result[node.ID] = point{lon: node.Lon, lat: node.Lat}
		}
	}
	return result, nil
}

func writeCSV(path string, restrictions []restriction, nodes map[int64]point, area bounds) (int, int, error) {
	file, err := os.Create(path)
	if err != nil {
		return 0, 0, err
	}
	defer file.Close()
	output := bufio.NewWriter(file)
	if _, err := fmt.Fprintln(output,
		"# from_source_id,via_x,via_y,to_source_id,type[,penalty_s or via_way_ids separated by ;]"); err != nil {
		return 0, 0, err
	}
	written, missing := 0, 0
	for _, rule := range restrictions {
		via, ok := nodes[rule.viaNode]
		if !ok {
			missing++
			continue
		}
		if !area.contains(via) {
			continue
		}
		kind, suffix := rule.kind, ""
		if len(rule.viaWays) > 0 {
			ids := make([]string, len(rule.viaWays))
			for i, id := range rule.viaWays {
				ids[i] = strconv.FormatInt(id, 10)
			}
			kind += "_via"
			suffix = "," + strings.Join(ids, ";")
		}
		if _, err := fmt.Fprintf(output, "%d,%.7f,%.7f,%d,%s%s\n",
			rule.fromWay, via.lon, via.lat, rule.toWay, kind, suffix); err != nil {
			return 0, 0, err
		}
		written++
	}
	if err := output.Flush(); err != nil {
		return 0, 0, err
	}
	return written, missing, nil
}

func run(args []string) error {
	flags := flag.NewFlagSet("zeus-osm-turns", flag.ContinueOnError)
	input := flags.String("input", "", "input .osm.pbf file")
	output := flags.String("output", "", "output Zeus turn CSV")
	bboxValue := flags.String("bbox", "", "optional min_lon,min_lat,max_lon,max_lat")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *input == "" || *output == "" {
		return errors.New("--input and --output are required")
	}
	area, err := parseBounds(*bboxValue)
	if err != nil {
		return err
	}
	restrictions, skipped, err := readRestrictions(*input)
	if err != nil {
		return fmt.Errorf("read restriction relations: %w", err)
	}
	wantedWays := map[int64]struct{}{}
	for _, rule := range restrictions {
		if len(rule.viaWays) == 0 {
			continue
		}
		wantedWays[rule.fromWay] = struct{}{}
		wantedWays[rule.toWay] = struct{}{}
		for _, id := range rule.viaWays {
			wantedWays[id] = struct{}{}
		}
	}
	if len(wantedWays) > 0 {
		ways, err := readWays(*input, wantedWays)
		if err != nil {
			return fmt.Errorf("read via ways: %w", err)
		}
		resolved := restrictions[:0]
		for _, rule := range restrictions {
			if len(rule.viaWays) > 0 && !resolveViaWays(&rule, ways) {
				skipped["unresolved_via_way"]++
				continue
			}
			resolved = append(resolved, rule)
		}
		restrictions = resolved
	}
	wanted := make(map[int64]struct{}, len(restrictions))
	for _, restriction := range restrictions {
		wanted[restriction.viaNode] = struct{}{}
	}
	nodes, err := readViaNodes(*input, wanted)
	if err != nil {
		return fmt.Errorf("read via nodes: %w", err)
	}
	written, missing, err := writeCSV(*output, restrictions, nodes, area)
	if err != nil {
		return fmt.Errorf("write turn CSV: %w", err)
	}
	fmt.Printf("relations=%d\nwritten=%d\nmissing_via_nodes=%d\n", len(restrictions), written, missing)
	reasons := make([]string, 0, len(skipped))
	for reason := range skipped {
		reasons = append(reasons, reason)
	}
	sort.Strings(reasons)
	for _, reason := range reasons {
		fmt.Printf("skipped.%s=%d\n", reason, skipped[reason])
	}
	fmt.Printf("output=%s\n", *output)
	return nil
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "zeus-osm-turns:", err)
		os.Exit(1)
	}
}
