package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"math"
	"mime/multipart"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const maxUploadBytes = 512 << 20

type Config struct {
	Addr            string
	DataDir         string
	ZeusMap         string
	WebDir          string
	CmdLimit        time.Duration
	ImportWorkers   int
	SimulateWorkers int
	RouteWorkerMaps int
	// SessionWorkerMaps bounds resident session-worker processes (one per
	// recently used map); each hosts many agent sessions.
	SessionWorkerMaps int
	// AgentDecisionWallTTL bounds real-time agent reasoning. Simulation time
	// does not advance while a request-driven decision is pending.
	AgentDecisionWallTTL time.Duration
}

type Server struct {
	config Config
	logger *slog.Logger
	jobs   *JobManager
	// simSlots bounds concurrent C++ simulate processes: each one loads the
	// full .zmap and builds its own spatial index.
	simSlots chan struct{}
	// routeWorkers keeps one framed C++ process per recently used immutable
	// map version, so HTTP routing reuses MapRuntime and RoutePlanner indexes.
	routeWorkers *RouteWorkerManager
	// sessionWorkers keeps resident C++ session-worker processes hosting
	// stateful agent simulation sessions per recently used map version.
	sessionWorkers *SessionWorkerManager
	// agentSessions tracks server-side metadata of live agent sessions; the
	// authoritative state always lives inside the C++ worker process.
	agentSessions agentSessionRegistry
	// decisions coordinates active Agent decision barriers. It owns only
	// pending waits; authoritative simulation state remains in the C++ worker.
	decisions       *DecisionCoordinator
	decisionWallTTL time.Duration
}

type APIError struct {
	Error string `json:"error"`
}

type InspectResult struct {
	UploadID             string           `json:"uploadId"`
	SourceFile           string           `json:"sourceFile"`
	Shapefile            string           `json:"shapefile,omitempty"`
	TurnRestrictionsFile string           `json:"turnRestrictionsFile,omitempty"`
	Files                []string         `json:"files"`
	Driver               string           `json:"driver"`
	Layer                string           `json:"layer"`
	FeatureCount         int64            `json:"featureCount"`
	Geometry             string           `json:"geometry"`
	GeometryCounts       map[string]int64 `json:"geometryCounts,omitempty"`
	CRS                  string           `json:"crs"`
	Fields               []string         `json:"fields"`
	NavigationCompatible bool             `json:"navigationCompatible"`
	SuggestedUsage       string           `json:"suggestedUsage"`
	OSMRoadData          bool             `json:"osmRoadData"`
	Raw                  string           `json:"raw"`
}

type Mapping struct {
	IDField              string  `json:"idField"`
	OnewayField          string  `json:"onewayField"`
	SpeedField           string  `json:"speedField"`
	LanesField           string  `json:"lanesField"`
	RoadClassField       string  `json:"roadClassField"`
	ZLevelField          string  `json:"zLevelField"`
	BridgeField          string  `json:"bridgeField"`
	TunnelField          string  `json:"tunnelField"`
	TargetCRS            string  `json:"targetCrs"`
	DefaultSpeedKPH      float64 `json:"defaultSpeedKph"`
	SnapToleranceMeters  float64 `json:"snapToleranceMeters"`
	DefaultBidirectional bool    `json:"defaultBidirectional"`
}

type ImportRequest struct {
	UploadID             string               `json:"uploadId"`
	SourceFile           string               `json:"sourceFile"`
	Shapefile            string               `json:"shapefile,omitempty"`
	TurnRestrictionsFile string               `json:"turnRestrictionsFile,omitempty"`
	Name                 string               `json:"name"`
	Mapping              Mapping              `json:"mapping"`
	OSMPreprocess        OSMPreprocessOptions `json:"osmPreprocess"`
}

type OSMPreprocessOptions struct {
	Enabled         bool    `json:"enabled"`
	IncludeService  bool    `json:"includeService"`
	IncludeTrack    bool    `json:"includeTrack"`
	IncludePrivate  bool    `json:"includePrivate"`
	MinLengthMeters float64 `json:"minLengthMeters"`
}

type OSMCleaningNormalization struct {
	GeometryCollectionsConverted int64 `json:"geometryCollectionsConverted"`
	DefaultSpeedApplied          int64 `json:"defaultSpeedApplied"`
	MPHSpeedConverted            int64 `json:"mphSpeedConverted"`
	ImpliedOnewayApplied         int64 `json:"impliedOnewayApplied"`
	ReverseOnewayNormalized      int64 `json:"reverseOnewayNormalized"`
	DuplicateGeometriesRemoved   int64 `json:"duplicateGeometriesRemoved"`
}

type OSMCleaningSummary struct {
	Profile          string                   `json:"profile"`
	Options          OSMPreprocessOptions     `json:"options"`
	InputFeatures    int64                    `json:"inputFeatures"`
	OutputFeatures   int64                    `json:"outputFeatures"`
	FilteredFeatures int64                    `json:"filteredFeatures"`
	Normalization    OSMCleaningNormalization `json:"normalization"`
	ExcludedByReason map[string]int64         `json:"excludedByReason"`
	OutputByClass    map[string]int64         `json:"outputByClass"`
}

type ReferenceLayerStyle struct {
	Color   string  `json:"color"`
	Opacity float64 `json:"opacity"`
}

type ReferenceLayerRequest struct {
	UploadID   string              `json:"uploadId"`
	SourceFile string              `json:"sourceFile"`
	Name       string              `json:"name"`
	Style      ReferenceLayerStyle `json:"style"`
}

type ReferenceLayerUpdateRequest struct {
	Name  *string              `json:"name"`
	Style *ReferenceLayerStyle `json:"style"`
}

type ReferenceLayerRecord struct {
	ID           string              `json:"id"`
	Name         string              `json:"name"`
	CreatedAt    time.Time           `json:"createdAt"`
	Source       string              `json:"source"`
	Geometry     string              `json:"geometry"`
	FeatureCount int64               `json:"featureCount"`
	CRS          string              `json:"crs"`
	Style        ReferenceLayerStyle `json:"style"`
	GeoJSON      string              `json:"-"`
}

type ValidationSummary struct {
	Nodes                 int `json:"nodes"`
	DirectedEdges         int `json:"directedEdges"`
	Components            int `json:"components"`
	LargestComponentNodes int `json:"largestComponentNodes"`
	TurnTransitions       int `json:"turnTransitions"`
	Fatal                 int `json:"fatal"`
	Errors                int `json:"errors"`
	Warnings              int `json:"warnings"`
	Info                  int `json:"info"`
}

type ValidationIssue struct {
	Severity string     `json:"severity"`
	Code     string     `json:"code"`
	Source   string     `json:"source"`
	Location [2]float64 `json:"location"`
	Message  string     `json:"message"`
}

type MapRecord struct {
	ID               string              `json:"id"`
	Name             string              `json:"name"`
	CreatedAt        time.Time           `json:"createdAt"`
	Source           string              `json:"source"`
	TurnRestrictions string              `json:"turnRestrictions,omitempty"`
	Summary          ValidationSummary   `json:"summary"`
	Issues           []ValidationIssue   `json:"issues"`
	Cleaning         *OSMCleaningSummary `json:"cleaning,omitempty"`
	Runtime          string              `json:"-"`
	GeoJSON          string              `json:"-"`
	NodesGeoJSON     string              `json:"-"`
	IssuesGeoJSON    string              `json:"-"`
}

type QueryRequest struct {
	X           *float64 `json:"x"`
	Y           *float64 `json:"y"`
	Lon         *float64 `json:"lon"`
	Lat         *float64 `json:"lat"`
	Heading     *float64 `json:"heading"`
	MaxDistance float64  `json:"maxDistance"`
	Limit       int      `json:"limit"`
}

type MatchCandidate struct {
	Edge             uint32     `json:"edge"`
	RoadID           string     `json:"roadId"`
	Source           string     `json:"source"`
	OffsetS          float64    `json:"offsetS"`
	Distance         float64    `json:"distance"`
	HeadingDelta     float64    `json:"headingDelta"`
	Confidence       float64    `json:"confidence"`
	ProjectedRuntime [2]float64 `json:"projectedRuntime"`
}

type QueryResponse struct {
	RuntimePoint [2]float64       `json:"runtimePoint"`
	Matches      []MatchCandidate `json:"matches"`
}

var safeIDPattern = regexp.MustCompile(`^[a-zA-Z0-9_-]+$`)
var colorPattern = regexp.MustCompile(`^#[0-9a-fA-F]{6}$`)
var issuePattern = regexp.MustCompile(`^issue=([^: ]+):([^ ]+) source=([^ ]+) location=([^,]+),([^ ]+) message="(.*)"$`)
var matchPattern = regexp.MustCompile(`^match\[[0-9]+\]\.edge=([^ ]+) road_id=([^ ]+) source=([^ ]+) offset_s=([^ ]+) distance=([^ ]+) heading_delta=([^ ]+) confidence=([^ ]+) projected=([^,]+),([^ ]+)$`)

type RouteRequest struct {
	FromLon     *float64 `json:"fromLon"`
	FromLat     *float64 `json:"fromLat"`
	ToLon       *float64 `json:"toLon"`
	ToLat       *float64 `json:"toLat"`
	Algorithm   string   `json:"algorithm"`
	MaxDistance float64  `json:"maxDistance"`
}

type RouteMatch struct {
	Edge       uint32  `json:"edge"`
	RoadID     string  `json:"roadId"`
	Source     string  `json:"source"`
	OffsetS    float64 `json:"offsetS"`
	Distance   float64 `json:"distance"`
	Confidence float64 `json:"confidence"`
}

type RouteResponse struct {
	OK        bool   `json:"ok"`
	Algorithm string `json:"algorithm"`
	// EffectiveAlgorithm is what actually ran; bidirectional selections
	// downgrade to the forward search on turn-restricted maps.
	EffectiveAlgorithm string          `json:"effectiveAlgorithm"`
	Reason             string          `json:"reason,omitempty"`
	Message            string          `json:"message,omitempty"`
	Origin             RouteMatch      `json:"origin"`
	Destination        RouteMatch      `json:"destination"`
	Edges              int             `json:"edges"`
	LengthM            float64         `json:"lengthM"`
	TimeS              float64         `json:"timeS"`
	ExpandedNodes      int64           `json:"expandedNodes"`
	ComputeMs          float64         `json:"computeMs"`
	GeoJSON            json.RawMessage `json:"geojson,omitempty"`
}

var routeMatchPattern = regexp.MustCompile(
	`^(origin|dest)\.edge=([^ ]+) road_id=([^ ]+) source=([^ ]+) offset_s=([^ ]+) distance=([^ ]+) confidence=([^ ]+)$`)

type SimulateRequest struct {
	FromLon                *float64                    `json:"fromLon"`
	FromLat                *float64                    `json:"fromLat"`
	ToLon                  *float64                    `json:"toLon"`
	ToLat                  *float64                    `json:"toLat"`
	Count                  int                         `json:"count"`
	SpreadSeconds          float64                     `json:"spreadSeconds"`
	DurationSeconds        float64                     `json:"durationSeconds"`
	StepSeconds            float64                     `json:"stepSeconds"`
	SampleIntervalSeconds  float64                     `json:"sampleIntervalSeconds"`
	ExitHeadwayFfSeconds   float64                     `json:"exitHeadwayFfSeconds"`
	ExitHeadwayJamSeconds  float64                     `json:"exitHeadwayJamSeconds"`
	RerouteIntervalSeconds float64                     `json:"rerouteIntervalSeconds"`
	RerouteCostRatio       float64                     `json:"rerouteCostRatio"`
	Algorithm              string                      `json:"algorithm"`
	VehicleControls        []VehicleSimulationControl  `json:"vehicleControls,omitempty"`
	RoadControls           []RoadSimulationControl     `json:"roadControls,omitempty"`
	JunctionControls       []JunctionSimulationControl `json:"junctionControls,omitempty"`
	SignalPlans            []JunctionSignalPlan        `json:"signalPlans,omitempty"`
}

type VehicleSimulationControl struct {
	TimeSeconds float64 `json:"timeSeconds"`
	VehicleID   int     `json:"vehicleId"`
	Action      string  `json:"action"`
	Value       float64 `json:"value,omitempty"`
}

type RoadSimulationControl struct {
	TimeSeconds float64  `json:"timeSeconds"`
	EdgeIDs     []uint32 `json:"edgeIds"`
	Action      string   `json:"action"`
	Value       float64  `json:"value,omitempty"`
}

type JunctionSimulationControl struct {
	TimeSeconds float64 `json:"timeSeconds"`
	NodeID      uint32  `json:"nodeId"`
	Action      string  `json:"action"`
}

type SignalMovement struct {
	FromEdgeID uint32 `json:"fromEdgeId"`
	ToEdgeID   uint32 `json:"toEdgeId"`
}

type SignalPhase struct {
	GreenSeconds      float64          `json:"greenSeconds"`
	SaturationFlowVPH float64          `json:"saturationFlowVph"`
	Movements         []SignalMovement `json:"movements"`
}

type JunctionSignalPlan struct {
	NodeID        uint32        `json:"nodeId"`
	OffsetSeconds float64       `json:"offsetSeconds"`
	YellowSeconds float64       `json:"yellowSeconds"`
	AllRedSeconds float64       `json:"allRedSeconds"`
	Phases        []SignalPhase `json:"phases"`
}

type SimulateResponse struct {
	OK                         bool            `json:"ok"`
	Reason                     string          `json:"reason,omitempty"`
	Message                    string          `json:"message,omitempty"`
	Vehicles                   int64           `json:"vehicles"`
	Arrived                    int64           `json:"arrived"`
	Unroutable                 int64           `json:"unroutable"`
	WaitingAtEnd               int64           `json:"waitingAtEnd"`
	DrivingAtEnd               int64           `json:"drivingAtEnd"`
	Ticks                      int64           `json:"ticks"`
	RoutePlans                 int64           `json:"routePlans"`
	Samples                    int64           `json:"samples"`
	AvgTravelS                 float64         `json:"avgTravelS"`
	MinTravelS                 float64         `json:"minTravelS"`
	MaxTravelS                 float64         `json:"maxTravelS"`
	TotalDistanceM             float64         `json:"totalDistanceM"`
	Deadlock                   bool            `json:"deadlock"`
	Cancelled                  bool            `json:"cancelled"`
	BarrierWaitMs              float64         `json:"barrierWaitMs"`
	ComputeMs                  float64         `json:"computeMs"`
	ControlEvents              int64           `json:"controlEvents"`
	VehicleControls            int64           `json:"vehicleControls"`
	RoadControls               int64           `json:"roadControls"`
	JunctionControls           int64           `json:"junctionControls"`
	RerouteAttempts            int64           `json:"rerouteAttempts"`
	RerouteSucceeded           int64           `json:"rerouteSucceeded"`
	RerouteFailed              int64           `json:"rerouteFailed"`
	SignalPlans                int64           `json:"signalPlans"`
	SignalPhases               int64           `json:"signalPhases"`
	SignalWaitEvents           int64           `json:"signalWaitEvents"`
	SignalRedWaitEvents        int64           `json:"signalRedWaitEvents"`
	SignalSaturationWaitEvents int64           `json:"signalSaturationWaitEvents"`
	SignalMovementsPassed      int64           `json:"signalMovementsPassed"`
	GeoJSON                    json.RawMessage `json:"geojson,omitempty"`
	Playback                   json.RawMessage `json:"playback,omitempty"`
}

func main() {
	config := Config{}
	flag.StringVar(&config.Addr, "addr", ":8080", "HTTP listen address")
	flag.StringVar(&config.DataDir, "data-dir", "data", "persistent data directory")
	flag.StringVar(&config.ZeusMap, "zeus-map", "build/zeus-map", "path to zeus-map executable")
	flag.StringVar(&config.WebDir, "web-dir", "apps/web/dist", "built web application directory")
	flag.DurationVar(&config.CmdLimit, "command-timeout", 2*time.Minute, "C++ map command timeout")
	flag.IntVar(&config.ImportWorkers, "import-workers", 2, "maximum concurrent map imports")
	flag.IntVar(&config.SimulateWorkers, "simulate-workers", 2, "maximum concurrent simulations")
	flag.IntVar(&config.RouteWorkerMaps, "route-worker-maps", 4, "maximum resident route map workers")
	flag.IntVar(&config.SessionWorkerMaps, "session-worker-maps", 2, "maximum resident agent session map workers")
	flag.DurationVar(&config.AgentDecisionWallTTL, "agent-decision-wall-ttl", 5*time.Minute, "maximum wall time for one agent decision")
	flag.Parse()

	server := NewServer(config, slog.Default())
	defer server.Close()
	if err := server.ensureDirectories(); err != nil {
		slog.Error("initialize data directories", "error", err)
		os.Exit(1)
	}

	httpServer := &http.Server{
		Addr:              config.Addr,
		Handler:           server.routes(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       5 * time.Minute,
		WriteTimeout:      0,
		IdleTimeout:       2 * time.Minute,
	}
	shutdownSignal, stopSignals := signal.NotifyContext(
		context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stopSignals()
	go func() {
		<-shutdownSignal.Done()
		shutdownContext, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := httpServer.Shutdown(shutdownContext); err != nil {
			slog.Error("graceful HTTP shutdown", "error", err)
		}
		server.Close()
	}()
	slog.Info("Zeus control server listening", "addr", config.Addr)
	if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		slog.Error("control server stopped", "error", err)
		os.Exit(1)
	}
}

func NewServer(config Config, logger *slog.Logger) *Server {
	if config.CmdLimit == 0 {
		config.CmdLimit = 2 * time.Minute
	}
	if config.AgentDecisionWallTTL <= 0 {
		config.AgentDecisionWallTTL = 5 * time.Minute
	}
	simSlots := config.SimulateWorkers
	if simSlots <= 0 {
		simSlots = 2
	}
	return &Server{
		config:         config,
		logger:         logger,
		jobs:           NewJobManager(config.ImportWorkers),
		simSlots:       make(chan struct{}, simSlots),
		routeWorkers:   NewRouteWorkerManager(config.ZeusMap, config.CmdLimit, config.RouteWorkerMaps),
		sessionWorkers: NewSessionWorkerManager(config.ZeusMap, 10*config.CmdLimit, config.SessionWorkerMaps),
		agentSessions: agentSessionRegistry{
			sessions:  make(map[string]agentSessionEntry),
			snapshots: make(map[string]agentSnapshotEntry),
		},
		decisions:       NewDecisionCoordinator(),
		decisionWallTTL: config.AgentDecisionWallTTL,
	}
}

func (s *Server) Close() {
	if s.decisions != nil {
		s.decisions.Close()
	}
	if s.sessionWorkers != nil {
		s.sessionWorkers.Close()
	}
	if s.routeWorkers != nil {
		s.routeWorkers.Close()
	}
}

func (s *Server) ensureDirectories() error {
	for _, directory := range []string{
		s.uploadsDir(), s.mapsDir(), s.referenceLayersDir(), s.agentSnapshotsDir(),
	} {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			return err
		}
	}
	return nil
}

func (s *Server) uploadsDir() string { return filepath.Join(s.config.DataDir, "uploads") }
func (s *Server) mapsDir() string    { return filepath.Join(s.config.DataDir, "maps") }
func (s *Server) referenceLayersDir() string {
	return filepath.Join(s.config.DataDir, "reference-layers")
}

func (s *Server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/health", s.handleHealth)
	mux.HandleFunc("GET /api/maps", s.handleListMaps)
	mux.HandleFunc("POST /api/maps/inspect", s.handleInspect)
	mux.HandleFunc("POST /api/maps/import", s.handleImport)
	mux.HandleFunc("GET /api/reference-layers", s.handleListReferenceLayers)
	mux.HandleFunc("POST /api/reference-layers", s.handleCreateReferenceLayer)
	mux.HandleFunc("PATCH /api/reference-layers/{id}", s.handleUpdateReferenceLayer)
	mux.HandleFunc("DELETE /api/reference-layers/{id}", s.handleDeleteReferenceLayer)
	mux.HandleFunc("GET /api/reference-layers/{id}/geojson", s.handleReferenceLayerGeoJSON)
	mux.HandleFunc("GET /api/jobs/{id}", s.handleGetJob)
	mux.HandleFunc("GET /api/jobs/{id}/events", s.handleJobEvents)
	mux.HandleFunc("POST /api/jobs/{id}/cancel", s.handleCancelJob)
	mux.HandleFunc("GET /api/maps/{id}", s.handleGetMap)
	mux.HandleFunc("GET /api/maps/{id}/geojson", s.handleGeoJSON)
	mux.HandleFunc("GET /api/maps/{id}/nodes.geojson", s.handleNodesGeoJSON)
	mux.HandleFunc("GET /api/maps/{id}/issues.geojson", s.handleIssuesGeoJSON)
	mux.HandleFunc("POST /api/maps/{id}/query", s.handleQuery)
	mux.HandleFunc("POST /api/maps/{id}/route", s.handleRoute)
	mux.HandleFunc("POST /api/maps/{id}/simulate", s.handleSimulate)
	mux.HandleFunc("POST /api/maps/{id}/agent/sessions", s.handleCreateAgentSession)
	mux.HandleFunc("GET /api/maps/{id}/agent/tools", s.handleAgentTools)
	mux.HandleFunc("GET /api/maps/{id}/agent/sessions/{session}", s.handleObserveAgentSession)
	mux.HandleFunc("GET /api/maps/{id}/agent/sessions/{session}/agent/{vehicle}", s.handleAgentObserveVehicle)
	mux.HandleFunc("POST /api/maps/{id}/agent/sessions/{session}/plan", s.handleAgentPlan)
	mux.HandleFunc("POST /api/maps/{id}/agent/sessions/{session}/step", s.handleAgentStep)
	mux.HandleFunc("POST /api/maps/{id}/agent/sessions/{session}/actions", s.handleAgentAction)
	mux.HandleFunc("POST /api/maps/{id}/agent/sessions/{session}/run", s.handleAgentRunToEnd)
	mux.HandleFunc("POST /api/maps/{id}/agent/sessions/{session}/pause", s.handleAgentPause)
	mux.HandleFunc("POST /api/maps/{id}/agent/sessions/{session}/snapshots", s.handleCreateAgentSnapshot)
	mux.HandleFunc("POST /api/maps/{id}/agent/snapshots/{snapshot}/restore", s.handleRestoreAgentSnapshot)
	mux.HandleFunc("DELETE /api/maps/{id}/agent/snapshots/{snapshot}", s.handleDeleteAgentSnapshot)
	mux.HandleFunc("GET /api/maps/{id}/agent/sessions/{session}/result", s.handleAgentSessionResult)
	mux.HandleFunc("DELETE /api/maps/{id}/agent/sessions/{session}", s.handleCloseAgentSession)
	mux.Handle("/", spaHandler(s.config.WebDir))
	return requestLogger(s.logger, mux)
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "service": "zeus-control-server"})
}

func (s *Server) handleInspect(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxUploadBytes)
	if err := r.ParseMultipartForm(32 << 20); err != nil {
		writeError(w, http.StatusBadRequest, "invalid multipart upload: "+err.Error())
		return
	}
	files := r.MultipartForm.File["files"]
	if len(files) == 0 {
		writeError(w, http.StatusBadRequest, "upload a GeoJSON file or a complete Shapefile bundle")
		return
	}

	uploadID := newID("upl")
	uploadDir := filepath.Join(s.uploadsDir(), uploadID)
	if err := os.MkdirAll(uploadDir, 0o755); err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	accepted := false
	defer func() {
		if !accepted {
			if err := os.RemoveAll(uploadDir); err != nil {
				s.logger.Warn("remove rejected upload", "upload_id", uploadID, "error", err)
			}
		}
	}()
	var saved []string
	for _, file := range files {
		name := filepath.Base(file.Filename)
		if !allowedUploadExtension(filepath.Ext(name)) {
			continue
		}
		if err := saveMultipartFile(file, filepath.Join(uploadDir, name)); err != nil {
			writeError(w, http.StatusInternalServerError, "save upload: "+err.Error())
			return
		}
		saved = append(saved, name)
	}
	sourceFile, err := selectVectorSource(saved)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	turnRestrictionsFile, err := selectTurnRestrictionsFile(saved)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	output, err := s.runMapCommand(r.Context(), "inspect", filepath.Join(uploadDir, sourceFile))
	if err != nil {
		writeError(w, http.StatusUnprocessableEntity, err.Error())
		return
	}
	result := parseInspect(output)
	result.UploadID = uploadID
	result.SourceFile = sourceFile
	result.TurnRestrictionsFile = turnRestrictionsFile
	if strings.EqualFold(filepath.Ext(sourceFile), ".shp") {
		result.Shapefile = sourceFile
	}
	result.Files = saved
	if err := saveJSONFile(filepath.Join(uploadDir, "inspect.json"), result); err != nil {
		writeError(w, http.StatusInternalServerError, "save inspection metadata: "+err.Error())
		return
	}
	accepted = true
	writeJSON(w, http.StatusOK, result)
}

func (s *Server) handleImport(w http.ResponseWriter, r *http.Request) {
	var request ImportRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !safeIDPattern.MatchString(request.UploadID) {
		writeError(w, http.StatusBadRequest, "invalid upload ID")
		return
	}
	if request.SourceFile == "" {
		request.SourceFile = request.Shapefile
	}
	request.SourceFile = filepath.Base(request.SourceFile)
	if !supportedVectorSourceExtension(filepath.Ext(request.SourceFile)) {
		writeError(w, http.StatusBadRequest, "source file must be .shp, .geojson or .json")
		return
	}
	if err := validateMapping(request.Mapping); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	source := filepath.Join(s.uploadsDir(), request.UploadID, request.SourceFile)
	if _, err := os.Stat(source); err != nil {
		writeError(w, http.StatusNotFound, "uploaded vector source was not found")
		return
	}
	inspection, err := loadInspectResult(filepath.Join(s.uploadsDir(), request.UploadID, "inspect.json"))
	if err != nil {
		output, inspectErr := s.runMapCommand(r.Context(), "inspect", source)
		if inspectErr != nil {
			writeError(w, http.StatusUnprocessableEntity, inspectErr.Error())
			return
		}
		inspection = parseInspect(output)
	}
	inspection.NavigationCompatible, inspection.SuggestedUsage =
		classifyInspection(inspection.Geometry, inspection.GeometryCounts)
	inspection.OSMRoadData = hasField(inspection.Fields, "highway")
	if inspection.SourceFile != "" && inspection.SourceFile != request.SourceFile {
		writeError(w, http.StatusBadRequest, "source file does not match the inspected upload")
		return
	}
	turnRestrictions, err := resolveTurnRestrictionsFile(
		filepath.Join(s.uploadsDir(), request.UploadID), request.TurnRestrictionsFile, inspection.Files)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !inspection.NavigationCompatible {
		writeError(w, http.StatusUnprocessableEntity, fmt.Sprintf(
			"geometry %q cannot build a navigation graph; upload LineString or MultiLineString road data",
			inspection.Geometry))
		return
	}
	request.OSMPreprocess = normalizeOSMPreprocessOptions(request.OSMPreprocess)
	if request.OSMPreprocess.Enabled && !inspection.OSMRoadData {
		writeError(w, http.StatusUnprocessableEntity,
			"OSM car preprocessing requires a highway field in the inspected road layer")
		return
	}
	if err := validateOSMPreprocessOptions(request.OSMPreprocess); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	job := s.jobs.Submit(func(ctx context.Context, publish func(JobProgress)) (MapRecord, error) {
		return s.runImport(ctx, request, source, turnRestrictions, publish)
	})
	writeJSON(w, http.StatusAccepted, job)
}

func (s *Server) handleCreateReferenceLayer(w http.ResponseWriter, r *http.Request) {
	var request ReferenceLayerRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !safeIDPattern.MatchString(request.UploadID) {
		writeError(w, http.StatusBadRequest, "invalid upload ID")
		return
	}
	request.SourceFile = filepath.Base(request.SourceFile)
	if !supportedVectorSourceExtension(filepath.Ext(request.SourceFile)) {
		writeError(w, http.StatusBadRequest, "source file must be .shp, .geojson or .json")
		return
	}
	source := filepath.Join(s.uploadsDir(), request.UploadID, request.SourceFile)
	if _, err := os.Stat(source); err != nil {
		writeError(w, http.StatusNotFound, "uploaded vector source was not found")
		return
	}
	inspection, err := loadInspectResult(
		filepath.Join(s.uploadsDir(), request.UploadID, "inspect.json"))
	if err != nil {
		output, inspectErr := s.runMapCommand(r.Context(), "inspect", source)
		if inspectErr != nil {
			writeError(w, http.StatusUnprocessableEntity, inspectErr.Error())
			return
		}
		inspection = parseInspect(output)
	}
	inspection.NavigationCompatible, inspection.SuggestedUsage =
		classifyInspection(inspection.Geometry, inspection.GeometryCounts)
	if inspection.SourceFile != "" && inspection.SourceFile != request.SourceFile {
		writeError(w, http.StatusBadRequest, "source file does not match the inspected upload")
		return
	}
	if inspection.SuggestedUsage != "reference-layer" {
		writeError(w, http.StatusUnprocessableEntity, fmt.Sprintf(
			"geometry %q is not a point or polygon reference layer", inspection.Geometry))
		return
	}
	style, err := normalizeReferenceLayerStyle(request.Style, inspection.Geometry)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	name := strings.TrimSpace(request.Name)
	if name == "" {
		name = inspection.Layer
	}
	if name == "" {
		name = strings.TrimSuffix(request.SourceFile, filepath.Ext(request.SourceFile))
	}
	if len(name) > 120 || strings.ContainsAny(name, "\r\n") {
		writeError(w, http.StatusBadRequest, "reference layer name is invalid")
		return
	}

	layerID := newID("ref")
	layerDir := filepath.Join(s.referenceLayersDir(), layerID)
	if err := os.MkdirAll(layerDir, 0o755); err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	published := false
	defer func() {
		if !published {
			if err := os.RemoveAll(layerDir); err != nil {
				s.logger.Warn("remove incomplete reference layer", "layer_id", layerID, "error", err)
			}
		}
	}()
	geoJSONPath := filepath.Join(layerDir, "layer.geojson")
	output, err := s.runMapCommand(
		r.Context(), "reference-geojson", source, "--output", geoJSONPath)
	if err != nil {
		writeError(w, http.StatusUnprocessableEntity, err.Error())
		return
	}
	featureCount := inspection.FeatureCount
	if converted, ok := parseOutputInt64(output, "features"); ok {
		featureCount = converted
	}
	record := ReferenceLayerRecord{
		ID:           layerID,
		Name:         name,
		CreatedAt:    time.Now().UTC(),
		Source:       request.SourceFile,
		Geometry:     inspection.Geometry,
		FeatureCount: featureCount,
		CRS:          "WGS 84",
		Style:        style,
		GeoJSON:      geoJSONPath,
	}
	if err := saveJSONFile(filepath.Join(layerDir, "record.json"), record); err != nil {
		writeError(w, http.StatusInternalServerError, "save reference layer: "+err.Error())
		return
	}
	published = true
	writeJSON(w, http.StatusCreated, record)
}

func (s *Server) handleUpdateReferenceLayer(w http.ResponseWriter, r *http.Request) {
	record, err := s.referenceLayerRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	var request ReferenceLayerUpdateRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if request.Name == nil && request.Style == nil {
		writeError(w, http.StatusBadRequest, "update requires name or style")
		return
	}
	if request.Name != nil {
		name := strings.TrimSpace(*request.Name)
		if name == "" || len(name) > 120 || strings.ContainsAny(name, "\r\n") {
			writeError(w, http.StatusBadRequest, "reference layer name is invalid")
			return
		}
		record.Name = name
	}
	if request.Style != nil {
		style, err := normalizeReferenceLayerStyle(*request.Style, record.Geometry)
		if err != nil {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
		record.Style = style
	}
	if err := saveJSONFile(
		filepath.Join(s.referenceLayersDir(), record.ID, "record.json"), record); err != nil {
		writeError(w, http.StatusInternalServerError, "update reference layer: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, record)
}

func (s *Server) handleDeleteReferenceLayer(w http.ResponseWriter, r *http.Request) {
	record, err := s.referenceLayerRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	if err := os.RemoveAll(filepath.Join(s.referenceLayersDir(), record.ID)); err != nil {
		writeError(w, http.StatusInternalServerError, "delete reference layer: "+err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) runImport(
	ctx context.Context,
	request ImportRequest,
	source string,
	turnRestrictions string,
	publish func(JobProgress),
) (MapRecord, error) {
	mapID := newID("map")
	mapDir := filepath.Join(s.mapsDir(), mapID)
	if err := os.MkdirAll(mapDir, 0o755); err != nil {
		return MapRecord{}, err
	}
	published := false
	defer func() {
		if !published {
			if err := os.RemoveAll(mapDir); err != nil {
				s.logger.Warn("remove incomplete map import", "map_id", mapID, "error", err)
			}
		}
	}()
	effectiveSource := source
	effectiveMapping := request.Mapping
	var cleaning *OSMCleaningSummary
	if request.OSMPreprocess.Enabled {
		publish(JobProgress{Phase: "preprocessing", Progress: 10, Message: "按汽车画像清洗 OSM 可行车路网"})
		cleanedSource := filepath.Join(mapDir, "osm-drivable.geojson")
		cleaningReport := filepath.Join(mapDir, "osm-cleaning-report.json")
		options := request.OSMPreprocess
		if _, err := s.runMapCommand(
			ctx, "preprocess-osm", source,
			"--output", cleanedSource,
			"--report", cleaningReport,
			"--profile", "car",
			"--include-service", strconv.FormatBool(options.IncludeService),
			"--include-track", strconv.FormatBool(options.IncludeTrack),
			"--include-private", strconv.FormatBool(options.IncludePrivate),
			"--min-length", strconv.FormatFloat(options.MinLengthMeters, 'f', -1, 64),
		); err != nil {
			return MapRecord{}, err
		}
		parsed, err := loadOSMCleaningSummary(cleaningReport)
		if err != nil {
			return MapRecord{}, fmt.Errorf("read OSM cleaning report: %w", err)
		}
		cleaning = &parsed
		effectiveSource = cleanedSource
		effectiveMapping = osmCanonicalMapping(request.Mapping)
		publish(JobProgress{
			Phase: "preprocessing", Progress: 22,
			Message: fmt.Sprintf("保留 %d 条道路，过滤 %d 条", parsed.OutputFeatures, parsed.FilteredFeatures),
		})
	}
	publish(JobProgress{Phase: "mapping", Progress: 26, Message: "写入字段映射配置"})
	mappingPath := filepath.Join(mapDir, "mapping.conf")
	if err := os.WriteFile(mappingPath, []byte(renderMapping(effectiveMapping)), 0o644); err != nil {
		return MapRecord{}, err
	}
	publish(JobProgress{Phase: "topology", Progress: 34, Message: "构建节点、有向边与空间索引"})
	runtimePath := filepath.Join(mapDir, "map.zmap")
	issuesGeoJSONPath := filepath.Join(mapDir, "issues.geojson")
	importArgs := []string{
		"import", effectiveSource, "--mapping", mappingPath, "--output", runtimePath,
		"--issues-output", issuesGeoJSONPath,
	}
	if turnRestrictions != "" {
		persistedTurns := filepath.Join(mapDir, "turn-restrictions.csv")
		contents, readErr := os.ReadFile(turnRestrictions)
		if readErr != nil {
			return MapRecord{}, fmt.Errorf("read turn restrictions: %w", readErr)
		}
		if writeErr := os.WriteFile(persistedTurns, contents, 0o644); writeErr != nil {
			return MapRecord{}, fmt.Errorf("persist turn restrictions: %w", writeErr)
		}
		importArgs = append(importArgs, "--turn-restrictions", persistedTurns)
	}
	output, err := s.runMapCommand(ctx, importArgs...)
	if err != nil {
		return MapRecord{}, err
	}
	publish(JobProgress{Phase: "validation", Progress: 70, Message: "解析拓扑质检报告"})
	summary, issues := parseValidation(output)
	geoJSONPath := filepath.Join(mapDir, "roads.geojson")
	publish(JobProgress{Phase: "visualization", Progress: 76, Message: "生成 WGS84 路网视图"})
	if _, err := s.runMapCommand(
		ctx, "geojson", runtimePath, "--output", geoJSONPath); err != nil {
		return MapRecord{}, err
	}
	nodesGeoJSONPath := filepath.Join(mapDir, "nodes.geojson")
	publish(JobProgress{Phase: "nodes", Progress: 83, Message: "生成拓扑节点图层"})
	if _, err := s.runMapCommand(
		ctx, "nodes-geojson", runtimePath, "--output", nodesGeoJSONPath); err != nil {
		return MapRecord{}, err
	}
	publish(JobProgress{Phase: "issues", Progress: 89, Message: "生成可定位质检事件图层"})
	if _, err := os.Stat(issuesGeoJSONPath); err != nil {
		return MapRecord{}, fmt.Errorf("validation issue layer was not created: %w", err)
	}

	name := strings.TrimSpace(request.Name)
	if name == "" {
		name = strings.TrimSuffix(request.SourceFile, filepath.Ext(request.SourceFile))
	}
	record := MapRecord{
		ID:               mapID,
		Name:             name,
		CreatedAt:        time.Now().UTC(),
		Source:           request.SourceFile,
		TurnRestrictions: request.TurnRestrictionsFile,
		Summary:          summary,
		Issues:           issues,
		Cleaning:         cleaning,
		Runtime:          runtimePath,
		GeoJSON:          geoJSONPath,
		NodesGeoJSON:     nodesGeoJSONPath,
		IssuesGeoJSON:    issuesGeoJSONPath,
	}
	publish(JobProgress{Phase: "persisting", Progress: 95, Message: "原子发布地图版本记录"})
	if err := saveRecord(filepath.Join(mapDir, "record.json"), record); err != nil {
		return MapRecord{}, err
	}
	published = true
	return record, nil
}

func (s *Server) handleGetJob(w http.ResponseWriter, r *http.Request) {
	job, ok := s.jobs.Get(r.PathValue("id"))
	if !ok {
		writeError(w, http.StatusNotFound, "import job not found")
		return
	}
	writeJSON(w, http.StatusOK, job)
}

func (s *Server) handleCancelJob(w http.ResponseWriter, r *http.Request) {
	job, ok := s.jobs.Cancel(r.PathValue("id"))
	if !ok {
		writeError(w, http.StatusNotFound, "import job not found")
		return
	}
	writeJSON(w, http.StatusAccepted, job)
}

func (s *Server) handleJobEvents(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeError(w, http.StatusInternalServerError, "streaming is unavailable")
		return
	}
	initial, updates, unsubscribe, ok := s.jobs.Subscribe(r.PathValue("id"))
	if !ok {
		writeError(w, http.StatusNotFound, "import job not found")
		return
	}
	defer unsubscribe()
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache, no-transform")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")
	_, _ = io.WriteString(w, "retry: 1500\n\n")
	if err := writeJobEvent(w, initial); err != nil {
		return
	}
	flusher.Flush()
	if terminalJob(initial.Status) {
		return
	}

	heartbeat := time.NewTicker(15 * time.Second)
	defer heartbeat.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case job := <-updates:
			if err := writeJobEvent(w, job); err != nil {
				return
			}
			flusher.Flush()
			if terminalJob(job.Status) {
				return
			}
		case <-heartbeat.C:
			if _, err := io.WriteString(w, ": keep-alive\n\n"); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}

func (s *Server) handleListMaps(w http.ResponseWriter, _ *http.Request) {
	entries, err := os.ReadDir(s.mapsDir())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	records := make([]MapRecord, 0, len(entries))
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		record, err := loadRecord(filepath.Join(s.mapsDir(), entry.Name(), "record.json"))
		if err == nil {
			records = append(records, record)
		}
	}
	sort.Slice(records, func(i, j int) bool { return records[i].CreatedAt.After(records[j].CreatedAt) })
	writeJSON(w, http.StatusOK, records)
}

func (s *Server) handleListReferenceLayers(w http.ResponseWriter, _ *http.Request) {
	entries, err := os.ReadDir(s.referenceLayersDir())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	records := make([]ReferenceLayerRecord, 0, len(entries))
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		record, err := loadReferenceLayerRecord(
			filepath.Join(s.referenceLayersDir(), entry.Name(), "record.json"))
		if err == nil {
			records = append(records, record)
		}
	}
	sort.Slice(records, func(i, j int) bool {
		return records[i].CreatedAt.After(records[j].CreatedAt)
	})
	writeJSON(w, http.StatusOK, records)
}

func (s *Server) handleGetMap(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, record)
}

func (s *Server) handleGeoJSON(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/geo+json")
	http.ServeFile(w, r, record.GeoJSON)
}

func (s *Server) handleIssuesGeoJSON(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/geo+json")
	http.ServeFile(w, r, record.IssuesGeoJSON)
}

func (s *Server) handleNodesGeoJSON(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/geo+json")
	http.ServeFile(w, r, record.NodesGeoJSON)
}

func (s *Server) handleReferenceLayerGeoJSON(w http.ResponseWriter, r *http.Request) {
	record, err := s.referenceLayerRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/geo+json")
	http.ServeFile(w, r, record.GeoJSON)
}

func (s *Server) handleQuery(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	var request QueryRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	args := []string{"query", record.Runtime}
	if request.Lon != nil && request.Lat != nil {
		args = append(args, "--lon", formatFloat(*request.Lon), "--lat", formatFloat(*request.Lat))
	} else if request.X != nil && request.Y != nil {
		args = append(args, "--x", formatFloat(*request.X), "--y", formatFloat(*request.Y))
	} else {
		writeError(w, http.StatusBadRequest, "query requires lon/lat or x/y")
		return
	}
	if request.Heading != nil {
		args = append(args, "--heading", formatFloat(*request.Heading))
	}
	if request.MaxDistance > 0 {
		args = append(args, "--max-distance", formatFloat(request.MaxDistance))
	}
	if request.Limit > 0 {
		args = append(args, "--limit", strconv.Itoa(min(request.Limit, 20)))
	}
	output, err := s.runMapCommand(r.Context(), args...)
	if err != nil {
		writeError(w, http.StatusUnprocessableEntity, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, parseQuery(output))
}

func (s *Server) handleRoute(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	var request RouteRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if request.FromLon == nil || request.FromLat == nil ||
		request.ToLon == nil || request.ToLat == nil {
		writeError(w, http.StatusBadRequest, "route requires fromLon/fromLat and toLon/toLat")
		return
	}
	algorithm := request.Algorithm
	if algorithm == "" {
		algorithm = "dijkstra"
	}
	if algorithm != "dijkstra" && algorithm != "astar" &&
		algorithm != "bidijkstra" && algorithm != "biastar" {
		writeError(w, http.StatusBadRequest,
			"algorithm must be dijkstra, astar, bidijkstra or biastar")
		return
	}
	maxDistance := request.MaxDistance
	if maxDistance <= 0 {
		maxDistance = 100
	}
	if maxDistance > 1000 {
		maxDistance = 1000
	}

	outputFile, err := os.CreateTemp("", "zeus-route-*.geojson")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create route output: "+err.Error())
		return
	}
	outputPath := outputFile.Name()
	if err := outputFile.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "close route output: "+err.Error())
		return
	}
	defer os.Remove(outputPath)

	workerResult, err := s.routeWorkers.Route(r.Context(), record.Runtime, RouteWorkerRequest{
		FromLon:     *request.FromLon,
		FromLat:     *request.FromLat,
		ToLon:       *request.ToLon,
		ToLat:       *request.ToLat,
		Algorithm:   algorithm,
		MaxDistance: maxDistance,
		OutputPath:  outputPath,
	})
	if err != nil {
		status := http.StatusUnprocessableEntity
		if errors.Is(err, context.DeadlineExceeded) {
			status = http.StatusGatewayTimeout
		}
		writeError(w, status, err.Error())
		return
	}
	output, exitCode := workerResult.Output, workerResult.ExitCode
	response := parseRoute(output)
	if !response.OK {
		// Unreachable or unmatched endpoints are computation results reported
		// through exit code 3, not command failures.
		if exitCode == 3 && response.Reason != "" {
			writeJSON(w, http.StatusOK, response)
			return
		}
		writeError(w, http.StatusUnprocessableEntity, output)
		return
	}
	if geojson, readErr := os.ReadFile(outputPath); readErr == nil && json.Valid(geojson) {
		response.GeoJSON = json.RawMessage(geojson)
	}
	writeJSON(w, http.StatusOK, response)
}

func (s *Server) mapRecord(id string) (MapRecord, error) {
	if !safeIDPattern.MatchString(id) {
		return MapRecord{}, errors.New("invalid map ID")
	}
	record, err := loadRecord(filepath.Join(s.mapsDir(), id, "record.json"))
	if err != nil {
		return MapRecord{}, errors.New("map not found")
	}
	record.Runtime = filepath.Join(s.mapsDir(), id, "map.zmap")
	record.GeoJSON = filepath.Join(s.mapsDir(), id, "roads.geojson")
	record.NodesGeoJSON = filepath.Join(s.mapsDir(), id, "nodes.geojson")
	record.IssuesGeoJSON = filepath.Join(s.mapsDir(), id, "issues.geojson")
	return record, nil
}

func (s *Server) referenceLayerRecord(id string) (ReferenceLayerRecord, error) {
	if !safeIDPattern.MatchString(id) {
		return ReferenceLayerRecord{}, errors.New("invalid reference layer ID")
	}
	record, err := loadReferenceLayerRecord(
		filepath.Join(s.referenceLayersDir(), id, "record.json"))
	if err != nil {
		return ReferenceLayerRecord{}, errors.New("reference layer not found")
	}
	record.GeoJSON = filepath.Join(s.referenceLayersDir(), id, "layer.geojson")
	return record, nil
}

func (s *Server) runMapCommand(parent context.Context, args ...string) (string, error) {
	text, exitCode, err := s.runMapCommandWithExit(parent, args...)
	if err != nil {
		return text, err
	}
	if exitCode != 0 {
		return text, fmt.Errorf("map command failed: %s", text)
	}
	return text, nil
}

// runMapCommandWithExit runs the C++ CLI and reports the process exit code so
// handlers can distinguish first-class computation results (exit code 3, for
// example a route that cannot be matched or connected) from real failures.
func (s *Server) runMapCommandWithExit(
	parent context.Context,
	args ...string,
) (string, int, error) {
	ctx, cancel := context.WithTimeout(parent, s.config.CmdLimit)
	defer cancel()
	command := exec.CommandContext(ctx, s.config.ZeusMap, args...)
	output, err := command.CombinedOutput()
	text := strings.TrimSpace(string(output))
	if ctx.Err() != nil {
		return text, -1, fmt.Errorf("map command timed out: %w", ctx.Err())
	}
	if err != nil {
		var exitError *exec.ExitError
		if errors.As(err, &exitError) {
			return text, exitError.ExitCode(), nil
		}
		if text == "" {
			text = err.Error()
		}
		return text, -1, fmt.Errorf("map command failed: %s", text)
	}
	return text, 0, nil
}

func renderMapping(mapping Mapping) string {
	if mapping.DefaultSpeedKPH <= 0 {
		mapping.DefaultSpeedKPH = 40
	}
	if mapping.SnapToleranceMeters <= 0 {
		mapping.SnapToleranceMeters = 0.5
	}
	return fmt.Sprintf(
		"id_field=%s\noneway_field=%s\nspeed_field=%s\nlanes_field=%s\nroad_class_field=%s\nz_level_field=%s\nbridge_field=%s\ntunnel_field=%s\ntarget_crs=%s\ndefault_speed_kph=%g\nsnap_tolerance_m=%g\ndefault_bidirectional=%t\n",
		mapping.IDField, mapping.OnewayField, mapping.SpeedField, mapping.LanesField, mapping.RoadClassField,
		mapping.ZLevelField, mapping.BridgeField, mapping.TunnelField, mapping.TargetCRS,
		mapping.DefaultSpeedKPH, mapping.SnapToleranceMeters, mapping.DefaultBidirectional)
}

func validateMapping(mapping Mapping) error {
	values := []string{
		mapping.IDField, mapping.OnewayField, mapping.SpeedField, mapping.LanesField, mapping.RoadClassField,
		mapping.ZLevelField, mapping.BridgeField, mapping.TunnelField, mapping.TargetCRS,
	}
	for _, value := range values {
		if len(value) > 256 || strings.ContainsAny(value, "\r\n=") {
			return errors.New("mapping contains an invalid field or CRS value")
		}
	}
	if mapping.SnapToleranceMeters < 0 || mapping.SnapToleranceMeters > 100 {
		return errors.New("snap tolerance must be between 0 and 100 meters")
	}
	if mapping.DefaultSpeedKPH < 0 || mapping.DefaultSpeedKPH > 500 {
		return errors.New("default speed must be between 0 and 500 km/h")
	}
	return nil
}

func normalizeOSMPreprocessOptions(options OSMPreprocessOptions) OSMPreprocessOptions {
	if options.Enabled && options.MinLengthMeters == 0 {
		options.MinLengthMeters = 2
	}
	return options
}

func validateOSMPreprocessOptions(options OSMPreprocessOptions) error {
	if !options.Enabled {
		return nil
	}
	if options.MinLengthMeters < 0.1 || options.MinLengthMeters > 1000 {
		return errors.New("OSM minimum road length must be between 0.1 and 1000 meters")
	}
	return nil
}

func osmCanonicalMapping(base Mapping) Mapping {
	return Mapping{
		IDField:              "road_id",
		OnewayField:          "oneway",
		SpeedField:           "speed_kph",
		LanesField:           "lanes",
		RoadClassField:       "road_class",
		ZLevelField:          "z_level",
		BridgeField:          "bridge",
		TunnelField:          "tunnel",
		TargetCRS:            base.TargetCRS,
		DefaultSpeedKPH:      40,
		SnapToleranceMeters:  base.SnapToleranceMeters,
		DefaultBidirectional: true,
	}
}

func hasField(fields []string, wanted string) bool {
	for _, field := range fields {
		if strings.EqualFold(strings.TrimSpace(field), wanted) {
			return true
		}
	}
	return false
}

func parseInspect(output string) InspectResult {
	result := InspectResult{Raw: output}
	for _, line := range strings.Split(output, "\n") {
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		switch {
		case key == "driver":
			result.Driver = value
		case strings.HasSuffix(key, ".name"):
			result.Layer = value
		case strings.HasSuffix(key, ".features"):
			result.FeatureCount, _ = strconv.ParseInt(value, 10, 64)
		case strings.HasSuffix(key, ".geometry"):
			result.Geometry = value
		case strings.HasSuffix(key, ".geometry_counts"):
			result.GeometryCounts = parseGeometryCounts(value)
		case strings.HasSuffix(key, ".crs"):
			result.CRS = value
		case strings.Contains(key, ".field["):
			field, _, _ := strings.Cut(value, ":")
			result.Fields = append(result.Fields, field)
		}
	}
	result.NavigationCompatible, result.SuggestedUsage =
		classifyInspection(result.Geometry, result.GeometryCounts)
	result.OSMRoadData = hasField(result.Fields, "highway")
	return result
}

// parseGeometryCounts parses "Line String:68138,Multi Line String:421" into a
// per-geometry-type count map. Malformed entries are skipped.
func parseGeometryCounts(value string) map[string]int64 {
	counts := make(map[string]int64)
	for _, entry := range strings.Split(value, ",") {
		name, count, ok := strings.Cut(entry, ":")
		if !ok {
			continue
		}
		parsed, err := strconv.ParseInt(strings.TrimSpace(count), 10, 64)
		if err != nil {
			continue
		}
		counts[strings.TrimSpace(name)] = parsed
	}
	if len(counts) == 0 {
		return nil
	}
	return counts
}

// classifyInspection prefers per-feature geometry counts, which the C++ inspect
// emits when the layer type is "Unknown (any)" (mixed geometry layers). It
// falls back to the layer-level geometry label otherwise.
func classifyInspection(geometry string, counts map[string]int64) (bool, string) {
	if len(counts) == 0 {
		return classifyGeometry(geometry)
	}
	lines, polygons, points, others := false, false, false, false
	for name := range counts {
		normalized := strings.ToLower(name)
		switch {
		case strings.Contains(normalized, "line string"):
			lines = true
		case strings.Contains(normalized, "polygon"):
			polygons = true
		case strings.Contains(normalized, "point"):
			points = true
		default:
			others = true
		}
	}
	switch {
	case lines && !polygons && !points && !others:
		return true, "road-network"
	case (polygons || points) && !lines && !others:
		return false, "reference-layer"
	default:
		return false, "unsupported"
	}
}

func classifyGeometry(geometry string) (bool, string) {
	normalized := strings.ToLower(strings.TrimSpace(geometry))
	if strings.Contains(normalized, "line string") || strings.Contains(normalized, "linestring") {
		return true, "road-network"
	}
	if strings.Contains(normalized, "polygon") || strings.Contains(normalized, "point") {
		return false, "reference-layer"
	}
	return false, "unsupported"
}

func normalizeReferenceLayerStyle(
	style ReferenceLayerStyle,
	geometry string,
) (ReferenceLayerStyle, error) {
	normalizedGeometry := strings.ToLower(geometry)
	if style.Color == "" {
		if strings.Contains(normalizedGeometry, "point") {
			style.Color = "#ffb24a"
		} else {
			style.Color = "#55c7b2"
		}
	}
	if !colorPattern.MatchString(style.Color) {
		return ReferenceLayerStyle{}, errors.New("reference layer color must use #RRGGBB")
	}
	if style.Opacity == 0 {
		if strings.Contains(normalizedGeometry, "point") {
			style.Opacity = 0.9
		} else {
			style.Opacity = 0.24
		}
	}
	if style.Opacity < 0.05 || style.Opacity > 1 {
		return ReferenceLayerStyle{}, errors.New(
			"reference layer opacity must be between 0.05 and 1")
	}
	return style, nil
}

func parseOutputInt64(output, wanted string) (int64, bool) {
	for _, line := range strings.Split(output, "\n") {
		key, value, ok := strings.Cut(line, "=")
		if !ok || key != wanted {
			continue
		}
		parsed, err := strconv.ParseInt(value, 10, 64)
		return parsed, err == nil
	}
	return 0, false
}

func parseValidation(output string) (ValidationSummary, []ValidationIssue) {
	summary := ValidationSummary{}
	issues := []ValidationIssue{}
	for _, line := range strings.Split(output, "\n") {
		key, value, ok := strings.Cut(line, "=")
		if ok && !strings.HasPrefix(line, "issue=") {
			number, _ := strconv.Atoi(value)
			switch key {
			case "nodes":
				summary.Nodes = number
			case "directed_edges":
				summary.DirectedEdges = number
			case "components":
				summary.Components = number
			case "largest_component_nodes":
				summary.LargestComponentNodes = number
			case "turn_transitions":
				summary.TurnTransitions = number
			case "fatal":
				summary.Fatal = number
			case "errors":
				summary.Errors = number
			case "warnings":
				summary.Warnings = number
			case "info":
				summary.Info = number
			}
		}
		matches := issuePattern.FindStringSubmatch(line)
		if len(matches) == 7 {
			x, _ := strconv.ParseFloat(matches[4], 64)
			y, _ := strconv.ParseFloat(matches[5], 64)
			issues = append(issues, ValidationIssue{
				Severity: matches[1], Code: matches[2], Source: matches[3],
				Location: [2]float64{x, y}, Message: matches[6],
			})
		}
	}
	return summary, issues
}

func parseQuery(output string) QueryResponse {
	response := QueryResponse{Matches: []MatchCandidate{}}
	for _, line := range strings.Split(output, "\n") {
		if strings.HasPrefix(line, "query_runtime_xy=") {
			parts := strings.Split(strings.TrimPrefix(line, "query_runtime_xy="), ",")
			if len(parts) == 2 {
				response.RuntimePoint[0], _ = strconv.ParseFloat(parts[0], 64)
				response.RuntimePoint[1], _ = strconv.ParseFloat(parts[1], 64)
			}
			continue
		}
		matches := matchPattern.FindStringSubmatch(line)
		if len(matches) != 10 {
			continue
		}
		edge, _ := strconv.ParseUint(matches[1], 10, 32)
		candidate := MatchCandidate{Edge: uint32(edge), RoadID: matches[2], Source: matches[3]}
		candidate.OffsetS, _ = strconv.ParseFloat(matches[4], 64)
		candidate.Distance, _ = strconv.ParseFloat(matches[5], 64)
		candidate.HeadingDelta, _ = strconv.ParseFloat(matches[6], 64)
		candidate.Confidence, _ = strconv.ParseFloat(matches[7], 64)
		candidate.ProjectedRuntime[0], _ = strconv.ParseFloat(matches[8], 64)
		candidate.ProjectedRuntime[1], _ = strconv.ParseFloat(matches[9], 64)
		response.Matches = append(response.Matches, candidate)
	}
	return response
}

func (s *Server) handleSimulate(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	var request SimulateRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	args, err := buildSimulateArgs(request)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	controlFileContent, err := buildSimulationControls(request, record.Summary)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	args = append([]string{"simulate", record.Runtime}, args...)
	if controlFileContent != "" {
		controlFile, createErr := os.CreateTemp("", "zeus-sim-controls-*.csv")
		if createErr != nil {
			writeError(w, http.StatusInternalServerError, "create simulation controls: "+createErr.Error())
			return
		}
		controlPath := controlFile.Name()
		if _, writeErr := controlFile.WriteString(controlFileContent); writeErr != nil {
			controlFile.Close()
			os.Remove(controlPath)
			writeError(w, http.StatusInternalServerError, "write simulation controls: "+writeErr.Error())
			return
		}
		if closeErr := controlFile.Close(); closeErr != nil {
			os.Remove(controlPath)
			writeError(w, http.StatusInternalServerError, "close simulation controls: "+closeErr.Error())
			return
		}
		defer os.Remove(controlPath)
		args = append(args, "--controls", controlPath)
	}
	signalFileContent, err := buildSignalPlans(request, record.Summary)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if signalFileContent != "" {
		signalFile, createErr := os.CreateTemp("", "zeus-sim-signals-*.csv")
		if createErr != nil {
			writeError(w, http.StatusInternalServerError, "create signal plans: "+createErr.Error())
			return
		}
		signalPath := signalFile.Name()
		if _, writeErr := signalFile.WriteString(signalFileContent); writeErr != nil {
			signalFile.Close()
			os.Remove(signalPath)
			writeError(w, http.StatusInternalServerError, "write signal plans: "+writeErr.Error())
			return
		}
		if closeErr := signalFile.Close(); closeErr != nil {
			os.Remove(signalPath)
			writeError(w, http.StatusInternalServerError, "close signal plans: "+closeErr.Error())
			return
		}
		defer os.Remove(signalPath)
		args = append(args, "--signals", signalPath)
	}

	trajectoryFile, err := os.CreateTemp("", "zeus-sim-*.geojson")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create trajectory output: "+err.Error())
		return
	}
	trajectoryPath := trajectoryFile.Name()
	if err := trajectoryFile.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "close trajectory output: "+err.Error())
		return
	}
	defer os.Remove(trajectoryPath)
	playbackFile, err := os.CreateTemp("", "zeus-sim-*.json")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create playback output: "+err.Error())
		return
	}
	playbackPath := playbackFile.Name()
	if err := playbackFile.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "close playback output: "+err.Error())
		return
	}
	defer os.Remove(playbackPath)
	args = append(args, "--output", trajectoryPath, "--playback", playbackPath)

	// Bound concurrent simulate processes; each loads the whole .zmap.
	select {
	case s.simSlots <- struct{}{}:
		defer func() { <-s.simSlots }()
	case <-r.Context().Done():
		writeError(w, http.StatusServiceUnavailable, "simulation queue cancelled by client")
		return
	}

	output, exitCode, err := s.runMapCommandWithExit(r.Context(), args...)
	if err != nil {
		writeError(w, http.StatusUnprocessableEntity, err.Error())
		return
	}
	response := parseSimulate(output)
	if !response.OK {
		// A demand set that cannot route at all is a computation result.
		if exitCode == 3 && response.Reason != "" {
			writeJSON(w, http.StatusOK, response)
			return
		}
		writeError(w, http.StatusUnprocessableEntity, output)
		return
	}
	if geojson, readErr := os.ReadFile(trajectoryPath); readErr == nil && json.Valid(geojson) {
		response.GeoJSON = json.RawMessage(geojson)
	}
	if playback, readErr := os.ReadFile(playbackPath); readErr == nil && json.Valid(playback) {
		response.Playback = json.RawMessage(playback)
	}
	writeJSON(w, http.StatusOK, response)
}

// buildSimulateArgs validates the request and returns the CLI arguments
// (without the leading "simulate <map>" pair).
func buildSimulateArgs(request SimulateRequest) ([]string, error) {
	if request.FromLon == nil || request.FromLat == nil ||
		request.ToLon == nil || request.ToLat == nil {
		return nil, errors.New("simulate requires fromLon/fromLat and toLon/toLat")
	}
	for _, value := range []float64{
		*request.FromLon, *request.FromLat, *request.ToLon, *request.ToLat,
		request.SpreadSeconds, request.DurationSeconds, request.StepSeconds,
		request.SampleIntervalSeconds, request.ExitHeadwayFfSeconds,
		request.ExitHeadwayJamSeconds, request.RerouteIntervalSeconds,
		request.RerouteCostRatio,
	} {
		if math.IsNaN(value) || math.IsInf(value, 0) {
			return nil, errors.New("simulation coordinates and timing values must be finite")
		}
	}
	algorithm := request.Algorithm
	if algorithm == "" {
		algorithm = "dijkstra"
	}
	switch algorithm {
	case "dijkstra", "astar", "bidijkstra", "biastar":
	default:
		return nil, errors.New("algorithm must be dijkstra, astar, bidijkstra or biastar")
	}
	count := request.Count
	if count == 0 {
		count = 1
	}
	if count < 1 || count > 10000 {
		return nil, errors.New("count must be between 1 and 10000")
	}
	duration := request.DurationSeconds
	if duration == 0 {
		duration = 900
	}
	if duration < 60 || duration > 28800 {
		return nil, errors.New("durationSeconds must be between 60 and 28800")
	}
	step := request.StepSeconds
	if step == 0 {
		step = 1
	}
	if step < 0.1 || step > 10 {
		return nil, errors.New("stepSeconds must be between 0.1 and 10")
	}
	if request.SpreadSeconds < 0 || request.SpreadSeconds > duration {
		return nil, errors.New("spreadSeconds must be between 0 and durationSeconds")
	}
	if request.ExitHeadwayFfSeconds < 0 || request.ExitHeadwayFfSeconds > 60 {
		return nil, errors.New("exitHeadwayFfSeconds must be between 0 and 60")
	}
	if request.ExitHeadwayJamSeconds < 0 || request.ExitHeadwayJamSeconds > 60 {
		return nil, errors.New("exitHeadwayJamSeconds must be between 0 and 60")
	}
	if request.ExitHeadwayJamSeconds > 0 &&
		request.ExitHeadwayJamSeconds < request.ExitHeadwayFfSeconds {
		return nil, errors.New(
			"exitHeadwayJamSeconds must be zero or at least exitHeadwayFfSeconds")
	}
	if request.RerouteIntervalSeconds < 0 || request.RerouteIntervalSeconds > 3600 {
		return nil, errors.New("rerouteIntervalSeconds must be between 0 and 3600")
	}
	if request.RerouteIntervalSeconds > 0 && request.RerouteIntervalSeconds < step {
		return nil, errors.New(
			"rerouteIntervalSeconds must be zero or at least stepSeconds")
	}
	if request.RerouteCostRatio != 0 &&
		(request.RerouteCostRatio < 1.01 || request.RerouteCostRatio > 10) {
		return nil, errors.New("rerouteCostRatio must be zero or between 1.01 and 10")
	}
	sampleInterval := request.SampleIntervalSeconds
	if sampleInterval == 0 {
		sampleInterval = 15
	}
	minimumSampleInterval := math.Max(step, 1)
	if sampleInterval < minimumSampleInterval {
		return nil, fmt.Errorf(
			"sampleIntervalSeconds must be at least %g", minimumSampleInterval)
	}
	// Playback payload guard: keep the inline response bounded.
	if float64(count)*(duration/sampleInterval) > 400000 {
		return nil, errors.New(
			"sample budget exceeded: lower count or duration, or raise sampleIntervalSeconds")
	}

	args := []string{
		"--lon", formatFloat(*request.FromLon), "--lat", formatFloat(*request.FromLat),
		"--dest-lon", formatFloat(*request.ToLon), "--dest-lat", formatFloat(*request.ToLat),
		"--algorithm", algorithm,
		"--count", strconv.Itoa(count),
		"--duration", formatFloat(duration),
		"--step", formatFloat(step),
		"--sample-interval", formatFloat(sampleInterval),
	}
	if request.SpreadSeconds > 0 {
		args = append(args, "--spread", formatFloat(request.SpreadSeconds))
	}
	if request.ExitHeadwayFfSeconds > 0 {
		args = append(args, "--exit-headway-ff", formatFloat(request.ExitHeadwayFfSeconds))
	}
	if request.ExitHeadwayJamSeconds > 0 {
		args = append(args, "--exit-headway-jam", formatFloat(request.ExitHeadwayJamSeconds))
	}
	if request.RerouteIntervalSeconds > 0 {
		args = append(args, "--reroute-interval", formatFloat(request.RerouteIntervalSeconds))
	}
	if request.RerouteCostRatio > 0 {
		args = append(args, "--reroute-cost-ratio", formatFloat(request.RerouteCostRatio))
	}
	return args, nil
}

// buildSimulationControls validates API-level control commands and serializes
// the small, deterministic CSV protocol consumed by zeus-map simulate.
func buildSimulationControls(request SimulateRequest, summary ValidationSummary) (string, error) {
	count := request.Count
	if count == 0 {
		count = 1
	}
	duration := request.DurationSeconds
	if duration == 0 {
		duration = 900
	}

	validateTime := func(value float64) error {
		if math.IsNaN(value) || math.IsInf(value, 0) || value < 0 || value >= duration {
			return fmt.Errorf("control timeSeconds must be finite and in [0, %g)", duration)
		}
		return nil
	}
	factor := func(action string, value float64) (string, float64, error) {
		switch action {
		case "speedFactor":
			if math.IsNaN(value) || math.IsInf(value, 0) || value < 0.05 || value > 3 {
				return "", 0, errors.New("speedFactor value must be between 0.05 and 3")
			}
			return "speed_factor", value, nil
		case "capacityFactor":
			if math.IsNaN(value) || math.IsInf(value, 0) || value < 0.05 || value > 10 {
				return "", 0, errors.New("capacityFactor value must be between 0.05 and 10")
			}
			return "capacity_factor", value, nil
		default:
			return action, 1, nil
		}
	}

	eventCount := len(request.VehicleControls) + len(request.JunctionControls)
	for _, control := range request.RoadControls {
		eventCount += len(control.EdgeIDs)
	}
	if eventCount > 10000 {
		return "", errors.New("control event budget exceeded: at most 10000 target events")
	}
	if len(request.VehicleControls) == 0 && len(request.RoadControls) == 0 &&
		len(request.JunctionControls) == 0 {
		return "", nil
	}

	var output strings.Builder
	output.WriteString("# time_s,scope,target_id,action,value\n")
	for index, control := range request.VehicleControls {
		if err := validateTime(control.TimeSeconds); err != nil {
			return "", fmt.Errorf("vehicleControls[%d]: %w", index, err)
		}
		if control.VehicleID < 0 || control.VehicleID >= count {
			return "", fmt.Errorf("vehicleControls[%d].vehicleId must be between 0 and %d", index, count-1)
		}
		if control.Action != "hold" && control.Action != "release" && control.Action != "speedFactor" {
			return "", fmt.Errorf("vehicleControls[%d].action must be hold, release or speedFactor", index)
		}
		action, value, err := factor(control.Action, control.Value)
		if err != nil {
			return "", fmt.Errorf("vehicleControls[%d]: %w", index, err)
		}
		fmt.Fprintf(&output, "%s,vehicle,%d,%s,%s\n", formatFloat(control.TimeSeconds), control.VehicleID, action, formatFloat(value))
	}
	for index, control := range request.RoadControls {
		if err := validateTime(control.TimeSeconds); err != nil {
			return "", fmt.Errorf("roadControls[%d]: %w", index, err)
		}
		if len(control.EdgeIDs) == 0 {
			return "", fmt.Errorf("roadControls[%d].edgeIds must not be empty", index)
		}
		if control.Action != "close" && control.Action != "open" &&
			control.Action != "speedFactor" && control.Action != "capacityFactor" {
			return "", fmt.Errorf("roadControls[%d].action must be close, open, speedFactor or capacityFactor", index)
		}
		action, value, err := factor(control.Action, control.Value)
		if err != nil {
			return "", fmt.Errorf("roadControls[%d]: %w", index, err)
		}
		seen := make(map[uint32]struct{}, len(control.EdgeIDs))
		for _, edgeID := range control.EdgeIDs {
			if summary.DirectedEdges > 0 && uint64(edgeID) >= uint64(summary.DirectedEdges) {
				return "", fmt.Errorf("roadControls[%d] edge %d is outside map edge range [0, %d)", index, edgeID, summary.DirectedEdges)
			}
			if _, duplicate := seen[edgeID]; duplicate {
				continue
			}
			seen[edgeID] = struct{}{}
			fmt.Fprintf(&output, "%s,edge,%d,%s,%s\n", formatFloat(control.TimeSeconds), edgeID, action, formatFloat(value))
		}
	}
	for index, control := range request.JunctionControls {
		if err := validateTime(control.TimeSeconds); err != nil {
			return "", fmt.Errorf("junctionControls[%d]: %w", index, err)
		}
		if summary.Nodes > 0 && uint64(control.NodeID) >= uint64(summary.Nodes) {
			return "", fmt.Errorf("junctionControls[%d].nodeId %d is outside map node range [0, %d)", index, control.NodeID, summary.Nodes)
		}
		if control.Action != "close" && control.Action != "open" {
			return "", fmt.Errorf("junctionControls[%d].action must be close or open", index)
		}
		fmt.Fprintf(&output, "%s,junction,%d,%s,1\n", formatFloat(control.TimeSeconds), control.NodeID, control.Action)
	}
	return output.String(), nil
}

// buildSignalPlans validates the nested API representation and writes one row
// per allowed movement. The C++ engine performs the final topology check.
func buildSignalPlans(request SimulateRequest, summary ValidationSummary) (string, error) {
	if len(request.SignalPlans) == 0 {
		return "", nil
	}
	if len(request.SignalPlans) > 1000 {
		return "", errors.New("signal plan budget exceeded: at most 1000 junctions")
	}
	finiteInRange := func(value, minimum, maximum float64) bool {
		return !math.IsNaN(value) && !math.IsInf(value, 0) &&
			value >= minimum && value <= maximum
	}
	seenNodes := make(map[uint32]struct{}, len(request.SignalPlans))
	movementCount := 0
	var output strings.Builder
	output.WriteString("# node_id,phase,green_s,yellow_s,all_red_s,offset_s,from_edge,to_edge,saturation_flow_vph\n")
	for planIndex, plan := range request.SignalPlans {
		if summary.Nodes > 0 && uint64(plan.NodeID) >= uint64(summary.Nodes) {
			return "", fmt.Errorf("signalPlans[%d].nodeId %d is outside map node range [0, %d)", planIndex, plan.NodeID, summary.Nodes)
		}
		if _, duplicate := seenNodes[plan.NodeID]; duplicate {
			return "", fmt.Errorf("signalPlans[%d].nodeId duplicates junction %d", planIndex, plan.NodeID)
		}
		seenNodes[plan.NodeID] = struct{}{}
		if !finiteInRange(plan.OffsetSeconds, 0, 86400) {
			return "", fmt.Errorf("signalPlans[%d].offsetSeconds must be between 0 and 86400", planIndex)
		}
		if !finiteInRange(plan.YellowSeconds, 0, 60) ||
			!finiteInRange(plan.AllRedSeconds, 0, 60) {
			return "", fmt.Errorf("signalPlans[%d] clearance times must be between 0 and 60", planIndex)
		}
		if len(plan.Phases) == 0 || len(plan.Phases) > 32 {
			return "", fmt.Errorf("signalPlans[%d].phases must contain between 1 and 32 phases", planIndex)
		}
		for phaseIndex, phase := range plan.Phases {
			if !finiteInRange(phase.GreenSeconds, 0.1, 600) {
				return "", fmt.Errorf("signalPlans[%d].phases[%d].greenSeconds must be between 0.1 and 600", planIndex, phaseIndex)
			}
			if len(phase.Movements) == 0 {
				return "", fmt.Errorf("signalPlans[%d].phases[%d].movements must not be empty", planIndex, phaseIndex)
			}
			saturationFlowVPH := phase.SaturationFlowVPH
			if saturationFlowVPH == 0 {
				saturationFlowVPH = 1800
			}
			if !finiteInRange(saturationFlowVPH, 60, 7200) {
				return "", fmt.Errorf("signalPlans[%d].phases[%d].saturationFlowVph must be zero or between 60 and 7200", planIndex, phaseIndex)
			}
			seenMovements := make(map[[2]uint32]struct{}, len(phase.Movements))
			for movementIndex, movement := range phase.Movements {
				if summary.DirectedEdges > 0 &&
					(uint64(movement.FromEdgeID) >= uint64(summary.DirectedEdges) ||
						uint64(movement.ToEdgeID) >= uint64(summary.DirectedEdges)) {
					return "", fmt.Errorf("signalPlans[%d].phases[%d].movements[%d] contains an edge outside map range [0, %d)", planIndex, phaseIndex, movementIndex, summary.DirectedEdges)
				}
				key := [2]uint32{movement.FromEdgeID, movement.ToEdgeID}
				if _, duplicate := seenMovements[key]; duplicate {
					continue
				}
				seenMovements[key] = struct{}{}
				movementCount++
				if movementCount > 10000 {
					return "", errors.New("signal movement budget exceeded: at most 10000 movements")
				}
				fmt.Fprintf(&output, "%d,%d,%s,%s,%s,%s,%d,%d,%s\n",
					plan.NodeID, phaseIndex, formatFloat(phase.GreenSeconds),
					formatFloat(plan.YellowSeconds), formatFloat(plan.AllRedSeconds),
					formatFloat(plan.OffsetSeconds), movement.FromEdgeID, movement.ToEdgeID,
					formatFloat(saturationFlowVPH))
			}
		}
	}
	return output.String(), nil
}

func parseSimulate(output string) SimulateResponse {
	response := SimulateResponse{}
	for _, line := range strings.Split(output, "\n") {
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		switch key {
		case "simulate":
			response.OK = value == "ok"
		case "reason":
			response.Reason = value
		case "message":
			response.Message = value
		case "vehicles":
			response.Vehicles, _ = strconv.ParseInt(value, 10, 64)
		case "arrived":
			response.Arrived, _ = strconv.ParseInt(value, 10, 64)
		case "unroutable":
			response.Unroutable, _ = strconv.ParseInt(value, 10, 64)
		case "waiting_at_end":
			response.WaitingAtEnd, _ = strconv.ParseInt(value, 10, 64)
		case "driving_at_end":
			response.DrivingAtEnd, _ = strconv.ParseInt(value, 10, 64)
		case "ticks":
			response.Ticks, _ = strconv.ParseInt(value, 10, 64)
		case "route_plans":
			response.RoutePlans, _ = strconv.ParseInt(value, 10, 64)
		case "samples":
			response.Samples, _ = strconv.ParseInt(value, 10, 64)
		case "avg_travel_s":
			response.AvgTravelS, _ = strconv.ParseFloat(value, 64)
		case "min_travel_s":
			response.MinTravelS, _ = strconv.ParseFloat(value, 64)
		case "max_travel_s":
			response.MaxTravelS, _ = strconv.ParseFloat(value, 64)
		case "total_distance_m":
			response.TotalDistanceM, _ = strconv.ParseFloat(value, 64)
		case "deadlock":
			response.Deadlock = value == "1"
		case "cancelled":
			response.Cancelled = value == "1"
		case "barrier_wait_ms":
			response.BarrierWaitMs, _ = strconv.ParseFloat(value, 64)
		case "compute_ms":
			response.ComputeMs, _ = strconv.ParseFloat(value, 64)
		case "control_events":
			response.ControlEvents, _ = strconv.ParseInt(value, 10, 64)
		case "vehicle_controls":
			response.VehicleControls, _ = strconv.ParseInt(value, 10, 64)
		case "edge_controls":
			response.RoadControls, _ = strconv.ParseInt(value, 10, 64)
		case "junction_controls":
			response.JunctionControls, _ = strconv.ParseInt(value, 10, 64)
		case "reroute_attempts":
			response.RerouteAttempts, _ = strconv.ParseInt(value, 10, 64)
		case "reroute_succeeded":
			response.RerouteSucceeded, _ = strconv.ParseInt(value, 10, 64)
		case "reroute_failed":
			response.RerouteFailed, _ = strconv.ParseInt(value, 10, 64)
		case "signal_plans":
			response.SignalPlans, _ = strconv.ParseInt(value, 10, 64)
		case "signal_phases":
			response.SignalPhases, _ = strconv.ParseInt(value, 10, 64)
		case "signal_wait_events":
			response.SignalWaitEvents, _ = strconv.ParseInt(value, 10, 64)
		case "signal_red_wait_events":
			response.SignalRedWaitEvents, _ = strconv.ParseInt(value, 10, 64)
		case "signal_saturation_wait_events":
			response.SignalSaturationWaitEvents, _ = strconv.ParseInt(value, 10, 64)
		case "signal_movements_passed":
			response.SignalMovementsPassed, _ = strconv.ParseInt(value, 10, 64)
		}
	}
	return response
}

func parseRoute(output string) RouteResponse {
	response := RouteResponse{Algorithm: "dijkstra"}
	for _, line := range strings.Split(output, "\n") {
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		switch key {
		case "route":
			response.OK = value == "ok"
		case "algorithm":
			response.Algorithm = value
		case "effective_algorithm":
			response.EffectiveAlgorithm = value
		case "reason":
			response.Reason = value
		case "message":
			response.Message = value
		case "edges":
			response.Edges, _ = strconv.Atoi(value)
		case "length_m":
			response.LengthM, _ = strconv.ParseFloat(value, 64)
		case "time_s":
			response.TimeS, _ = strconv.ParseFloat(value, 64)
		case "expanded_nodes":
			response.ExpandedNodes, _ = strconv.ParseInt(value, 10, 64)
		case "compute_ms":
			response.ComputeMs, _ = strconv.ParseFloat(value, 64)
		case "origin.edge", "dest.edge":
			matches := routeMatchPattern.FindStringSubmatch(line)
			if len(matches) != 8 {
				continue
			}
			edge, _ := strconv.ParseUint(matches[2], 10, 32)
			candidate := RouteMatch{
				Edge:   uint32(edge),
				RoadID: matches[3],
				Source: matches[4],
			}
			candidate.OffsetS, _ = strconv.ParseFloat(matches[5], 64)
			candidate.Distance, _ = strconv.ParseFloat(matches[6], 64)
			candidate.Confidence, _ = strconv.ParseFloat(matches[7], 64)
			if matches[1] == "origin" {
				response.Origin = candidate
			} else {
				response.Destination = candidate
			}
		}
	}
	return response
}

func allowedUploadExtension(extension string) bool {
	switch strings.ToLower(extension) {
	case ".shp", ".shx", ".dbf", ".prj", ".cpg", ".geojson", ".json", ".csv":
		return true
	default:
		return false
	}
}

func resolveTurnRestrictionsFile(uploadDir, requested string, uploaded []string) (string, error) {
	if requested == "" {
		return "", nil
	}
	if filepath.Base(requested) != requested || !strings.EqualFold(filepath.Ext(requested), ".csv") {
		return "", errors.New("turnRestrictionsFile must name one uploaded .csv file")
	}
	found := false
	for _, name := range uploaded {
		if name == requested {
			found = true
			break
		}
	}
	if !found {
		return "", errors.New("turnRestrictionsFile was not part of the inspected upload")
	}
	path := filepath.Join(uploadDir, requested)
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return "", errors.New("uploaded turn restrictions file was not found")
	}
	return path, nil
}

func supportedVectorSourceExtension(extension string) bool {
	switch strings.ToLower(extension) {
	case ".shp", ".geojson", ".json":
		return true
	default:
		return false
	}
}

func selectTurnRestrictionsFile(files []string) (string, error) {
	selected := ""
	for _, name := range files {
		if !strings.EqualFold(filepath.Ext(name), ".csv") {
			continue
		}
		if selected != "" {
			return "", errors.New("upload at most one turn restrictions .csv file")
		}
		selected = name
	}
	return selected, nil
}

func selectVectorSource(files []string) (string, error) {
	sources := make([]string, 0, 1)
	for _, name := range files {
		if supportedVectorSourceExtension(filepath.Ext(name)) {
			sources = append(sources, name)
		}
	}
	if len(sources) != 1 {
		return "", errors.New(
			"upload exactly one source: a .geojson/.json file or one .shp with its sidecars")
	}
	source := sources[0]
	if !strings.EqualFold(filepath.Ext(source), ".shp") {
		return source, nil
	}

	sourceStem := strings.ToLower(strings.TrimSuffix(source, filepath.Ext(source)))
	present := make(map[string]bool, len(files))
	for _, name := range files {
		stem := strings.ToLower(strings.TrimSuffix(name, filepath.Ext(name)))
		if stem == sourceStem {
			present[strings.ToLower(filepath.Ext(name))] = true
		}
	}
	for _, required := range []string{".shx", ".dbf", ".prj"} {
		if !present[required] {
			return "", fmt.Errorf("incomplete Shapefile bundle: missing %s sidecar", required)
		}
	}
	return source, nil
}

func saveMultipartFile(header *multipart.FileHeader, destination string) error {
	source, err := header.Open()
	if err != nil {
		return err
	}
	defer source.Close()
	target, err := os.OpenFile(destination, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return err
	}
	defer target.Close()
	_, err = io.Copy(target, source)
	return err
}

func newID(prefix string) string {
	bytes := make([]byte, 8)
	if _, err := rand.Read(bytes); err != nil {
		panic(err)
	}
	return prefix + "_" + hex.EncodeToString(bytes)
}

func saveRecord(path string, record MapRecord) error {
	return saveJSONFile(path, record)
}

func saveJSONFile(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	temporary, err := os.CreateTemp(filepath.Dir(path), ".record-*.json")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err := temporary.Chmod(0o644); err != nil {
		_ = temporary.Close()
		return err
	}
	if _, err := temporary.Write(data); err != nil {
		_ = temporary.Close()
		return err
	}
	if err := temporary.Sync(); err != nil {
		_ = temporary.Close()
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	return os.Rename(temporaryPath, path)
}

func loadInspectResult(path string) (InspectResult, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return InspectResult{}, err
	}
	var result InspectResult
	err = json.Unmarshal(data, &result)
	return result, err
}

func loadOSMCleaningSummary(path string) (OSMCleaningSummary, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return OSMCleaningSummary{}, err
	}
	var report struct {
		Profile string `json:"profile"`
		Options struct {
			IncludeService bool    `json:"include_service"`
			IncludeTrack   bool    `json:"include_track"`
			IncludePrivate bool    `json:"include_private"`
			MinLength      float64 `json:"min_length_m"`
		} `json:"options"`
		InputFeatures    int64 `json:"input_features"`
		OutputFeatures   int64 `json:"output_features"`
		FilteredFeatures int64 `json:"filtered_features"`
		Normalization    struct {
			GeometryCollectionsConverted int64 `json:"geometry_collections_converted"`
			DefaultSpeedApplied          int64 `json:"default_speed_applied"`
			MPHSpeedConverted            int64 `json:"mph_speed_converted"`
			ImpliedOnewayApplied         int64 `json:"implied_oneway_applied"`
			ReverseOnewayNormalized      int64 `json:"reverse_oneway_normalized"`
			DuplicateGeometriesRemoved   int64 `json:"duplicate_geometries_removed"`
		} `json:"normalization"`
		ExcludedByReason map[string]int64 `json:"excluded_by_reason"`
		OutputByClass    map[string]int64 `json:"output_by_class"`
	}
	if err := json.Unmarshal(data, &report); err != nil {
		return OSMCleaningSummary{}, err
	}
	if report.Profile == "" {
		return OSMCleaningSummary{}, errors.New("cleaning report is missing profile")
	}
	return OSMCleaningSummary{
		Profile: report.Profile,
		Options: OSMPreprocessOptions{
			Enabled: true, IncludeService: report.Options.IncludeService,
			IncludeTrack: report.Options.IncludeTrack, IncludePrivate: report.Options.IncludePrivate,
			MinLengthMeters: report.Options.MinLength,
		},
		InputFeatures: report.InputFeatures, OutputFeatures: report.OutputFeatures,
		FilteredFeatures: report.FilteredFeatures,
		Normalization: OSMCleaningNormalization{
			GeometryCollectionsConverted: report.Normalization.GeometryCollectionsConverted,
			DefaultSpeedApplied:          report.Normalization.DefaultSpeedApplied,
			MPHSpeedConverted:            report.Normalization.MPHSpeedConverted,
			ImpliedOnewayApplied:         report.Normalization.ImpliedOnewayApplied,
			ReverseOnewayNormalized:      report.Normalization.ReverseOnewayNormalized,
			DuplicateGeometriesRemoved:   report.Normalization.DuplicateGeometriesRemoved,
		},
		ExcludedByReason: report.ExcludedByReason,
		OutputByClass:    report.OutputByClass,
	}, nil
}

func loadRecord(path string) (MapRecord, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return MapRecord{}, err
	}
	var record MapRecord
	err = json.Unmarshal(data, &record)
	return record, err
}

func loadReferenceLayerRecord(path string) (ReferenceLayerRecord, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return ReferenceLayerRecord{}, err
	}
	var record ReferenceLayerRecord
	err = json.Unmarshal(data, &record)
	return record, err
}

func decodeJSON(r *http.Request, value any) error {
	decoder := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, APIError{Error: message})
}

func writeJobEvent(w io.Writer, job ImportJob) error {
	data, err := json.Marshal(job)
	if err != nil {
		return err
	}
	_, err = fmt.Fprintf(w, "event: progress\ndata: %s\n\n", data)
	return err
}

func formatFloat(value float64) string {
	return strconv.FormatFloat(value, 'f', -1, 64)
}

func requestLogger(logger *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		next.ServeHTTP(w, r)
		logger.Info("http request", "method", r.Method, "path", r.URL.Path, "duration", time.Since(started))
	})
}

func spaHandler(webDir string) http.Handler {
	fileServer := http.FileServer(http.Dir(webDir))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/") {
			http.NotFound(w, r)
			return
		}
		requested := filepath.Join(webDir, strings.TrimPrefix(filepath.Clean(r.URL.Path), string(filepath.Separator)))
		if info, err := os.Stat(requested); err == nil && !info.IsDir() {
			fileServer.ServeHTTP(w, r)
			return
		}
		http.ServeFile(w, r, filepath.Join(webDir, "index.html"))
	})
}
