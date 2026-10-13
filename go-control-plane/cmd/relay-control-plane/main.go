// Command relay-control-plane resolves endpoints, health-checks them, and
// serves configuration to the data plane.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/LegendaryPenguin/relay/api"
	"github.com/LegendaryPenguin/relay/config"
	"github.com/LegendaryPenguin/relay/discovery"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:9901", "listen address")
	clusterSpec := flag.String("cluster", "api=127.0.0.1:9000,127.0.0.1:9001",
		"cluster definition: name=host:port[:weight],... (repeatable via commas within one name)")
	dnsSpec := flag.String("dns-cluster", "", "cluster from DNS: name=host:port")
	probePath := flag.String("probe-path", "/", "health check path")
	probeInterval := flag.Duration("probe-interval", 2*time.Second, "health check interval")
	probeTimeout := flag.Duration("probe-timeout", 500*time.Millisecond, "health check timeout")
	resolveInterval := flag.Duration("resolve-interval", 10*time.Second, "endpoint resolution interval")
	unhealthyAfter := flag.Int("unhealthy-after", 3, "consecutive failures before marking unhealthy")
	healthyAfter := flag.Int("healthy-after", 2, "consecutive successes before marking healthy")
	routesFile := flag.String("routes", "", "JSON file of routes; omitted means one catch-all route")
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))

	health := discovery.NewHealthState(*unhealthyAfter, *healthyAfter, logger)
	registry := discovery.NewRegistry(health, logger)

	clusterNames, err := addStaticClusters(registry, *clusterSpec)
	if err != nil {
		logger.Error("bad -cluster", "error", err)
		os.Exit(2)
	}
	if *dnsSpec != "" {
		name, err := addDNSCluster(registry, *dnsSpec)
		if err != nil {
			logger.Error("bad -dns-cluster", "error", err)
			os.Exit(2)
		}
		clusterNames = append(clusterNames, name)
	}
	if len(clusterNames) == 0 {
		logger.Error("no clusters configured")
		os.Exit(2)
	}

	routes, err := loadRoutes(*routesFile, clusterNames)
	if err != nil {
		// A bad routes file is a refusal to start. Starting with a default
		// catch-all when the operator supplied a file would be silently
		// running something other than what they asked for.
		logger.Error("cannot load routes", "error", err)
		os.Exit(2)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Resolve and probe once before serving, so the first config a proxy
	// polls has real health rather than Unknown everywhere — which would make
	// every cluster ineligible and every request a 503 for the first probe
	// interval.
	registry.ResolveAll(ctx)
	prober := discovery.HTTPProber(*probePath, *probeTimeout)
	registry.ProbeAll(ctx, prober, 16)

	server := api.NewServer(registry, health, routes, logger)
	server.Rebuild()

	go func() {
		resolveTicker := time.NewTicker(*resolveInterval)
		probeTicker := time.NewTicker(*probeInterval)
		defer resolveTicker.Stop()
		defer probeTicker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-resolveTicker.C:
				registry.ResolveAll(ctx)
				server.Rebuild()
			case <-probeTicker.C:
				registry.ProbeAll(ctx, prober, 16)
				server.Rebuild()
			}
		}
	}()

	httpSrv := &http.Server{
		Addr:              *addr,
		Handler:           server.Routes(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      30 * time.Second,
	}

	go func() {
		sig := make(chan os.Signal, 1)
		signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
		<-sig
		logger.Info("shutting down")
		cancel()
		_ = httpSrv.Close()
	}()

	snap := server.Snapshot()
	logger.Info("control plane listening", "addr", *addr,
		"version", snap.Version, "clusters", len(snap.Clusters), "routes", len(snap.Routes))
	for _, w := range snap.Warnings() {
		logger.Warn("snapshot warning", "detail", w)
	}

	if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		logger.Error("serve", "error", err)
		os.Exit(1)
	}
}

// addStaticClusters parses "name=host:port[:weight],host:port[:weight]".
func addStaticClusters(registry *discovery.Registry, spec string) ([]string, error) {
	if strings.TrimSpace(spec) == "" {
		return nil, nil
	}
	eq := strings.Index(spec, "=")
	if eq < 0 {
		return nil, errors.New(`expected name=host:port,...`)
	}
	name := strings.TrimSpace(spec[:eq])
	if name == "" {
		return nil, errors.New("cluster name is empty")
	}

	var endpoints []config.Endpoint
	for _, item := range strings.Split(spec[eq+1:], ",") {
		item = strings.TrimSpace(item)
		if item == "" {
			continue
		}
		host, port, weight, err := parseEndpoint(item)
		if err != nil {
			return nil, err
		}
		endpoints = append(endpoints, config.Endpoint{
			Host: host, Port: port, Weight: weight, Health: config.HealthUnknown,
		})
	}
	if len(endpoints) == 0 {
		return nil, errors.New("cluster has no endpoints")
	}
	registry.AddResolver(&discovery.StaticResolver{ClusterName: name, Endpoints: endpoints})
	return []string{name}, nil
}

func addDNSCluster(registry *discovery.Registry, spec string) (string, error) {
	eq := strings.Index(spec, "=")
	if eq < 0 {
		return "", errors.New("expected name=host:port")
	}
	name := strings.TrimSpace(spec[:eq])
	host, port, _, err := parseEndpoint(strings.TrimSpace(spec[eq+1:]))
	if err != nil {
		return "", err
	}
	registry.AddResolver(&discovery.DNSResolver{
		ClusterName: name, Host: host, Port: port, Weight: 1,
	})
	return name, nil
}

func parseEndpoint(item string) (host string, port uint16, weight uint32, err error) {
	// Split from the right so an IPv6 literal's colons do not confuse it,
	// at least for the bracketed form.
	parts := strings.Split(item, ":")
	if len(parts) < 2 {
		return "", 0, 0, errors.New("expected host:port[:weight] in " + item)
	}
	weight = 1
	if len(parts) == 3 {
		w, perr := strconv.ParseUint(parts[2], 10, 32)
		if perr != nil {
			return "", 0, 0, errors.New("bad weight in " + item)
		}
		weight = uint32(w)
	}
	p, perr := strconv.ParseUint(parts[1], 10, 16)
	if perr != nil {
		return "", 0, 0, errors.New("bad port in " + item)
	}
	host = parts[0]
	if host == "" {
		return "", 0, 0, errors.New("empty host in " + item)
	}
	// Reject something that is neither an IP nor a plausible hostname early,
	// so a typo fails at startup rather than as a resolution error every ten
	// seconds.
	if net.ParseIP(host) == nil && !strings.Contains(host, ".") && host != "localhost" {
		return "", 0, 0, errors.New("host " + host + " is neither an IP nor a qualified name")
	}
	return host, uint16(p), weight, nil
}

func loadRoutes(path string, clusters []string) ([]config.Route, error) {
	if path == "" {
		out := make([]config.Route, 0, len(clusters))
		for i, name := range clusters {
			prefix := ""
			if i > 0 {
				// Only one route can be the catch-all; later clusters get a
				// prefix derived from their name so the default config is
				// valid rather than rejected for duplicate prefixes.
				prefix = "/" + name
			}
			out = append(out, config.Route{Cluster: name, PathPrefix: prefix})
		}
		return out, nil
	}

	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var parsed struct {
		Routes []config.Route `json:"routes"`
	}
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&parsed); err != nil {
		return nil, err
	}
	if len(parsed.Routes) == 0 {
		return nil, errors.New("routes file contains no routes")
	}
	return parsed.Routes, nil
}
