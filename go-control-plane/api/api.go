// Package api is the control plane's HTTP surface.
//
// Two audiences again, and the split is the same one as in Dovetail: the data
// plane polls for configuration, and operators read status and push changes.
//
// The data plane *pulls*. That is the significant choice. Pushing would be
// lower latency and requires the control plane to hold a connection to every
// proxy, track which pushes landed, and retry the ones that did not — which
// is a delivery problem with partial failure, and the most common way a
// control plane ends up with proxies running configurations it believes it
// replaced. Polling makes staleness bounded and self-correcting: a proxy that
// misses a poll is at most one interval behind, and nobody has to track
// anything.
//
// The cost is up to one poll interval of propagation delay, which for
// configuration that changes a few times an hour is not a cost worth
// engineering around.
package api

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/LegendaryPenguin/relay/config"
	"github.com/LegendaryPenguin/relay/discovery"
)

type Server struct {
	mu       sync.RWMutex
	snapshot config.Snapshot
	routes   []config.Route

	registry *discovery.Registry
	health   *discovery.HealthState
	log      *slog.Logger

	// Counters for the operator endpoint.
	polls            uint64
	pollsNotModified uint64
	publishes        uint64
	rejected         uint64
}

func NewServer(registry *discovery.Registry, health *discovery.HealthState,
	routes []config.Route, logger *slog.Logger) *Server {
	if logger == nil {
		logger = slog.Default()
	}
	return &Server{registry: registry, health: health, routes: routes, log: logger}
}

func (s *Server) Routes() *http.ServeMux {
	mux := http.NewServeMux()
	// Data-plane facing.
	mux.HandleFunc("GET /config", s.handleConfig)
	// Operator facing.
	mux.HandleFunc("PUT /routes", s.handlePutRoutes)
	mux.HandleFunc("GET /status", s.handleStatus)
	mux.HandleFunc("GET /health", s.handleHealth)
	mux.HandleFunc("GET /placement", s.handlePlacement)
	return mux
}

// handleConfig serves the current snapshot.
//
// Honours If-None-Match against the snapshot version, so a proxy polling
// every second on an unchanged configuration transfers a few bytes rather
// than the whole config. With a hundred proxies and a large endpoint list
// that is the difference between a trivial poll and a steady megabit of
// pointless traffic.
func (s *Server) handleConfig(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	s.polls++
	s.mu.Unlock()

	snap := s.Snapshot()
	etag := fmt.Sprintf(`"%d"`, snap.Version)
	if match := r.Header.Get("If-None-Match"); match == etag {
		s.mu.Lock()
		s.pollsNotModified++
		s.mu.Unlock()
		w.Header().Set("ETag", etag)
		w.WriteHeader(http.StatusNotModified)
		return
	}
	w.Header().Set("ETag", etag)
	s.respond(w, http.StatusOK, snap)
}

// handlePutRoutes replaces the route configuration.
//
// Validated before it is accepted. Rejecting here means the operator gets an
// error at the moment they made the mistake, rather than the data plane
// having to choose between running something stale they believe they replaced
// and breaking traffic.
func (s *Server) handlePutRoutes(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	var incoming struct {
		Routes []config.Route `json:"routes"`
	}
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(&incoming); err != nil {
		s.fail(w, http.StatusBadRequest, fmt.Errorf("malformed body: %w", err))
		return
	}
	if len(incoming.Routes) == 0 {
		s.fail(w, http.StatusBadRequest, errors.New("no routes supplied"))
		return
	}

	candidate := config.Snapshot{Clusters: s.registry.Clusters(), Routes: incoming.Routes}
	candidate.Normalize()
	if err := candidate.Validate(); err != nil {
		s.mu.Lock()
		s.rejected++
		s.mu.Unlock()
		s.log.Warn("rejected a route configuration", "error", err)
		s.fail(w, http.StatusBadRequest, err)
		return
	}

	s.mu.Lock()
	s.routes = candidate.Routes
	s.mu.Unlock()
	s.Rebuild()

	s.respond(w, http.StatusOK, map[string]any{
		"routes":   len(candidate.Routes),
		"version":  s.Snapshot().Version,
		"warnings": candidate.Warnings(),
	})
}

func (s *Server) handleStatus(w http.ResponseWriter, _ *http.Request) {
	snap := s.Snapshot()
	s.mu.RLock()
	stats := map[string]uint64{
		"polls":              s.polls,
		"polls_not_modified": s.pollsNotModified,
		"publishes":          s.publishes,
		"rejected":           s.rejected,
	}
	s.mu.RUnlock()

	clusters := make([]map[string]any, 0, len(snap.Clusters))
	for _, c := range snap.Clusters {
		clusters = append(clusters, map[string]any{
			"name":      c.Name,
			"endpoints": len(c.Endpoints),
			"eligible":  c.EligibleCount(),
		})
	}

	s.respond(w, http.StatusOK, map[string]any{
		"version":        snap.Version,
		"clusters":       clusters,
		"routes":         len(snap.Routes),
		"stats":          stats,
		"warnings":       snap.Warnings(),
		"resolve_errors": s.registry.ResolveErrors(),
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	s.respond(w, http.StatusOK, map[string]any{
		"endpoints": s.health.Stats(time.Now()),
	})
}

// handlePlacement answers "where would this key go".
//
// Exists because the control plane and the data plane compute the same hash,
// which makes consistent-hash placement predictable from outside. Being able
// to ask the question without tailing proxy logs is the difference between
// debugging affinity in minutes and in an afternoon.
func (s *Server) handlePlacement(w http.ResponseWriter, r *http.Request) {
	key := r.URL.Query().Get("key")
	cluster := r.URL.Query().Get("cluster")
	if key == "" || cluster == "" {
		s.fail(w, http.StatusBadRequest, errors.New("key and cluster are both required"))
		return
	}

	snap := s.Snapshot()
	var target *config.Cluster
	for i := range snap.Clusters {
		if snap.Clusters[i].Name == cluster {
			target = &snap.Clusters[i]
			break
		}
	}
	if target == nil {
		s.fail(w, http.StatusNotFound, fmt.Errorf("unknown cluster %s", cluster))
		return
	}

	ring := BuildRing(*target, 160)
	idx, ok := ring.Lookup(config.Fnv1a64(key))
	if !ok {
		s.fail(w, http.StatusServiceUnavailable,
			fmt.Errorf("cluster %s has no eligible endpoints", cluster))
		return
	}
	s.respond(w, http.StatusOK, map[string]any{
		"key":       key,
		"cluster":   cluster,
		"key_hash":  config.Fnv1a64(key),
		"endpoint":  target.Endpoints[idx].Address(),
		"ring_size": ring.Size(),
	})
}

// Snapshot returns the current configuration.
func (s *Server) Snapshot() config.Snapshot {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.snapshot
}

// Rebuild recomputes the snapshot from current discovery state.
//
// The version increments only when the content actually changed. Bumping it
// on every rebuild would defeat the ETag and make every proxy re-download an
// identical config on every poll — which is the whole saving.
func (s *Server) Rebuild() {
	clusters := s.registry.Clusters()

	s.mu.Lock()
	routes := append([]config.Route(nil), s.routes...)
	previous := s.snapshot
	s.mu.Unlock()

	candidate := config.Snapshot{Clusters: clusters, Routes: routes}
	candidate.Normalize()

	if sameContent(previous, candidate) {
		return
	}
	candidate.Version = previous.Version + 1

	if err := candidate.Validate(); err != nil {
		// A snapshot that fails validation is not published, and the previous
		// one keeps serving. This is the one place the control plane prefers
		// stale-but-valid over fresh-but-broken, because the data plane has
		// no better option on receiving a bad config.
		s.log.Error("refusing to publish an invalid snapshot; keeping the previous one",
			"error", err, "serving_version", previous.Version)
		return
	}

	for _, warning := range candidate.Warnings() {
		s.log.Warn("snapshot warning", "version", candidate.Version, "detail", warning)
	}

	s.mu.Lock()
	s.snapshot = candidate
	s.publishes++
	s.mu.Unlock()
	s.log.Info("published snapshot", "version", candidate.Version,
		"clusters", len(candidate.Clusters), "routes", len(candidate.Routes))
}

// sameContent compares everything except the version.
func sameContent(a, b config.Snapshot) bool {
	a.Version = 0
	b.Version = 0
	ja, err1 := json.Marshal(a)
	jb, err2 := json.Marshal(b)
	if err1 != nil || err2 != nil {
		// On a marshalling failure, report "different" so the snapshot is
		// republished. Treating an error as "same" would wedge the control
		// plane at whatever version it was on with no indication why.
		return false
	}
	return string(ja) == string(jb)
}

func (s *Server) respond(w http.ResponseWriter, code int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	if err := json.NewEncoder(w).Encode(body); err != nil {
		s.log.Error("writing response", "error", err)
	}
}

func (s *Server) fail(w http.ResponseWriter, code int, err error) {
	s.respond(w, code, map[string]string{"error": err.Error()})
}
