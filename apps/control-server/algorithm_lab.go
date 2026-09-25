package main

import (
	"bytes"
	"context"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"time"
)

//go:embed algorithm_runner.py
var algorithmRunner []byte

//go:embed algorithm_template.py
var algorithmTemplate string

func (s *Server) handleAlgorithmCapabilities(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"version": "zeus-routing-v1", "language": "Python 子集", "template": algorithmTemplate,
		"methods": []map[string]string{
			{"signature": "ctx.observation()", "description": "观察数据的独立副本。mode 为 static 或 vehicle；车辆模式含 tick、stateVersion、simulationTimeS、decisionReason、vehicle（位置、剩余路线、是否失效等）。"},
			{"signature": "ctx.keep()", "description": "仅车辆模式：return ctx.keep() 明确保持当前路线；不会搜索或替换路径。返回 None 仍表示无路可达。"},
			{"signature": "ctx.start() / ctx.goal()", "description": "虚拟起点 -1、终点 -2；其他状态为有向道路索引，表示沿该道路到达路口。"},
			{"signature": "ctx.neighbors(state)", "description": "只读合法后继列表：state、edge、cost（秒，含转向）、length_m（米）。封闭道路和禁转已过滤，起终点偏移已计入。"},
			{"signature": "ctx.estimate(state)", "description": "到吸附终点的直线距离 / 全图最高速度，单位秒。可用于 A* 的可采纳启发式；虚拟状态返回 0。"},
			{"signature": "ctx.queue()", "description": "稳定最小优先队列：push(state, priority)、pop() → (state, priority)、empty()。相同优先级按加入顺序弹出。"},
			{"signature": "ctx.record(state, g, f)", "description": "记录搜索事件供地图回放；g、f 为用户报告的搜索值，路线代价由平台独立复核。最多展示前 5000 条。"},
			{"signature": "ctx.route(states)", "description": "构造路线：完整状态列表以 start 开始、goal 结束，最多 10000 项。无路可达时 return None。"},
			{"signature": "ctx.log(value)", "description": "输出数字、字符串、布尔值或 None；最多 100 条，每条 1000 字符。"},
		},
		"syntax": "支持函数（最多 16 个）、if、for、while、break、continue、列表、字典、元组、解包、算术与比较。内置 len/range/min/max/abs；列表 append/reverse；字典 get。禁止 import、文件、网络、类、反射、推导式和第三方库。",
		"limits": map[string]int{"sourceBytes": 32768, "seconds": 5, "memoryMiB": 1024, "steps": 2000000, "maxSteps": 5000000, "expansions": 100000, "trace": 5000, "concurrentRuns": 2},
	})
}

type AlgorithmLabRequest struct {
	FromLon     *float64 `json:"fromLon"`
	FromLat     *float64 `json:"fromLat"`
	ToLon       *float64 `json:"toLon"`
	ToLat       *float64 `json:"toLat"`
	Source      string   `json:"source"`
	ClosedEdges []int    `json:"closedEdges"`
	Steps       int      `json:"steps"`
}
type AlgorithmLabResult struct {
	KeepCurrentRoute bool            `json:"keepCurrentRoute,omitempty"`
	Observation      json.RawMessage `json:"observation,omitempty"`
	ExecutionMode    string          `json:"executionMode"`
	RunID            string          `json:"runId,omitempty"`
	StorageError     string          `json:"storageError,omitempty"`
	OK               bool            `json:"ok"`
	Error            string          `json:"error,omitempty"`
	Phase            string          `json:"phase"`
	Line             int             `json:"line,omitempty"`
	States           []int           `json:"states,omitempty"`
	Logs             []string        `json:"logs"`
	Steps            int             `json:"steps"`
	ExpandedNodes    int             `json:"expandedNodes"`
	ComputeMs        float64         `json:"computeMs"`
	SearchTrace      json.RawMessage `json:"searchTrace,omitempty"`
	Baseline         struct {
		OK      bool    `json:"ok"`
		TimeS   float64 `json:"timeS"`
		LengthM float64 `json:"lengthM"`
	} `json:"baseline"`
	TimeS        float64         `json:"timeS"`
	LengthM      float64         `json:"lengthM"`
	Edges        int             `json:"edges"`
	GeoJSON      json.RawMessage `json:"geojson,omitempty"`
	MapID        string          `json:"mapId"`
	CodeRevision string          `json:"codeRevision"`
	SnapshotID   string          `json:"snapshotId"`
}

func validateAlgorithmRequest(req *AlgorithmLabRequest) error {
	values := []*float64{req.FromLon, req.FromLat, req.ToLon, req.ToLat}
	for i, value := range values {
		bound := 180.0
		if i%2 == 1 {
			bound = 90
		}
		if value == nil || math.IsNaN(*value) || math.IsInf(*value, 0) || math.Abs(*value) > bound {
			return errors.New("起终点必须是有效经纬度")
		}
	}
	if strings.TrimSpace(req.Source) == "" || len(req.Source) > 32768 {
		return errors.New("代码不能为空且不得超过 32 KiB")
	}
	if req.Steps == 0 {
		req.Steps = 2000000
	}
	if req.Steps < 1000 || req.Steps > 5000000 {
		return errors.New("执行步数必须在 1000 至 5000000 之间")
	}
	if len(req.ClosedEdges) > 256 {
		return errors.New("最多临时封闭 256 条有向道路")
	}
	for _, edge := range req.ClosedEdges {
		if edge < 0 {
			return errors.New("道路索引不得为负数")
		}
	}
	return nil
}

// Context cancellation kills the process, not just the HTTP response. The
// AST interpreter cannot spawn child processes. Parent also bounds output.
func runAlgorithmProcess(ctx context.Context, python, runner, graph string, request []byte) ([]byte, error) {
	output := &limitedAlgorithmOutput{limit: 2 << 20}
	err := runAlgorithmIO(ctx, python, runner, []string{graph}, bytes.NewReader(request), output)
	return output.Bytes(), err
}

func runAlgorithmIO(ctx context.Context, python, runner string, arguments []string, input io.Reader, stdout io.Writer) error {
	ctx, cancelProcess := context.WithCancel(ctx)
	defer cancelProcess()
	command := exec.CommandContext(ctx, python, append([]string{"-I", "-B", runner}, arguments...)...)
	command.Env = []string{"PATH=" + os.Getenv("PATH"), "LANG=C.UTF-8"}
	command.Stdin = input
	stderr := &limitedAlgorithmOutput{limit: 4096}
	command.Stdout, command.Stderr = stdout, stderr
	if err := command.Start(); err != nil {
		return err
	}
	done := make(chan error, 1)
	go func() { done <- command.Wait() }()
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	var err error
wait:
	for {
		select {
		case err = <-done:
			break wait
		case <-ctx.Done():
			<-done
			return ctx.Err()
		case <-ticker.C:
			if runtime.GOOS != "darwin" {
				continue
			}
			probeCtx, stopProbe := context.WithTimeout(ctx, time.Second)
			rss, probeErr := exec.CommandContext(probeCtx, "/bin/ps", "-o", "rss=", "-p", strconv.Itoa(command.Process.Pid)).Output()
			stopProbe()
			// A short-lived process may exit between the timer and the probe.
			if probeErr != nil {
				select {
				case err = <-done:
					break wait
				case <-time.After(20 * time.Millisecond):
				}
				cancelProcess()
				<-done
				return errors.New("无法监控算法进程内存，已停止运行")
			}
			kib, parseErr := strconv.ParseInt(strings.TrimSpace(string(rss)), 10, 64)
			if parseErr != nil || kib > 1024*1024 {
				cancelProcess()
				<-done
				return errors.New("算法进程内存超过 1 GiB 或无法读取，已停止运行")
			}
		}
	}
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if err != nil {
		return fmt.Errorf("算法执行进程退出（检查 Python 3、内存或 CPU 限制）: %w", err)
	}
	return nil
}

type limitedAlgorithmOutput struct {
	bytes.Buffer
	limit int
}

func (b *limitedAlgorithmOutput) Write(p []byte) (int, error) {
	if b.Len()+len(p) > b.limit {
		return 0, errors.New("algorithm output limit exceeded")
	}
	return b.Buffer.Write(p)
}

func (s *Server) handleAlgorithmRun(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 128<<10)
	var req AlgorithmLabRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		writeError(w, 400, "无效的算法请求")
		return
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		writeError(w, 400, "请求只能包含一个 JSON 对象")
		return
	}
	if err := validateAlgorithmRequest(&req); err != nil {
		writeError(w, 400, err.Error())
		return
	}
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, 404, err.Error())
		return
	}
	select {
	case s.algorithmSlots <- struct{}{}:
		defer func() { <-s.algorithmSlots }()
	default:
		writeError(w, 429, "已有两个算法在运行，请稍后重试")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	prepared, err := s.prepareAlgorithm(ctx, record, req)
	if err != nil {
		writeError(w, 422, err.Error())
		return
	}
	defer os.RemoveAll(prepared.dir)
	payload, _ := json.Marshal(map[string]any{"source": req.Source, "steps": req.Steps})
	runCtx, stop := context.WithTimeout(ctx, 10*time.Second)
	output, runErr := runAlgorithmProcess(runCtx, s.config.AlgorithmPython, prepared.runner, prepared.graph, payload)
	stop()
	result := prepared.result
	if runErr != nil {
		result.Error = runErr.Error()
		s.finishAlgorithmRun(w, r, req, result)
		return
	}
	if json.Unmarshal(output, &result) != nil {
		writeError(w, 502, "执行器返回了无效结果")
		return
	}
	result, err = s.validateAlgorithmResult(ctx, prepared, result)
	if err != nil {
		writeError(w, 502, err.Error())
		return
	}
	s.finishAlgorithmRun(w, r, req, result)
}
