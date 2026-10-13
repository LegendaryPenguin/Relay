#pragma once

// The router: where every policy mechanism composes into one request path.
//
// The order of operations is the design, and it took rearranging to get right.
// Each step is placed where it is because of what it costs to be wrong:
//
//   1. **Rate limit** first. It is the cheapest check and the only one that
//      bounds a single abusive client, so it runs before any resource is
//      committed.
//   2. **Load shed** second, on queue depth. Before endpoint selection,
//      because selecting an endpoint for a request about to be rejected wastes
//      the selection and, worse, increments its load counters.
//   3. **Select an endpoint.** No endpoint, no request — 503 without retry,
//      since a retry finds the same empty set.
//   4. **Check the breaker** for that endpoint, not for the cluster. A
//      per-cluster breaker takes out healthy endpoints along with the sick
//      one, which turns one bad host into a total outage. This was a real bug.
//   5. **Attempt**, with a per-attempt timeout *and* an overall deadline.
//      Both, because N attempts at T each is N*T of client latency, and a
//      client with a 1s budget does not care that each of our three attempts
//      honoured its 400ms.
//   6. **On failure, consult the retry budget** before retrying — not the
//      attempt count alone. The budget is what bounds aggregate amplification.
//
// The coupling that matters most: a request rejected at step 1 or 2 must not
// charge the breaker at step 4. Shedding is the proxy protecting the upstream;
// letting it trip the breaker converts self-protection into a self-inflicted
// outage. `Status::charges_breaker()` is where that lives.

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "relay/http.hpp"
#include "relay/metrics.hpp"
#include "relay/policy.hpp"
#include "relay/pool.hpp"
#include "relay/upstream.hpp"

namespace relay {

// How a request chooses its hash key for consistent hashing.
enum class HashKeySource {
    kNone,
    kPath,
    kHeader,
    kRemoteAddress,
};

struct RouteConfig {
    std::string cluster;
    // Prefix the request target must start with. Longest prefix wins, which
    // is resolved at config-build time by sorting rather than at request time.
    std::string path_prefix;

    BalancerKind balancer = BalancerKind::kP2C;
    HashKeySource hash_source = HashKeySource::kNone;
    std::string hash_header;

    Millis per_attempt_timeout{1000};
    // Overall budget across all attempts. Separate from the per-attempt
    // timeout because three 1s attempts is 3s of client latency, and a client
    // with a 1s budget does not care that each attempt honoured its own.
    Millis overall_deadline{3000};

    RetryPolicy::Config retry;
    CircuitBreaker::Config breaker;
    RetryBudget::Config retry_budget;
    RateLimiter::Config rate_limit;
    LoadShedder::Config shedder;
    bool rate_limit_enabled = false;
};

// The whole data-plane configuration, published atomically.
//
// Immutable once built. The control plane constructs a new one and the router
// swaps a shared_ptr, so a request that started under the old configuration
// finishes under it and no reader ever takes a lock.
class RouteTable {
  public:
    RouteTable() = default;

    void AddRoute(RouteConfig route, ClusterSnapshotPtr cluster);
    // Sorts routes so longest-prefix-first matching is a linear scan.
    void Finalize();

    struct Match {
        const RouteConfig* route = nullptr;
        ClusterSnapshotPtr cluster;
    };
    Match Lookup(std::string_view target) const;

    uint64_t version() const { return version_; }
    void set_version(uint64_t v) { version_ = v; }
    size_t size() const { return routes_.size(); }

  private:
    struct Entry {
        RouteConfig route;
        ClusterSnapshotPtr cluster;
    };
    std::vector<Entry> routes_;
    uint64_t version_ = 0;
};

using RouteTablePtr = std::shared_ptr<const RouteTable>;

// What the router needs from the world, injected so it can be tested without
// sockets.
//
// This is the seam that makes the failure-handling tests possible at all. A
// router that called Connect() directly could only be tested against a real
// upstream, and "make the upstream time out on the third request but not the
// first two" is not something a real upstream does on request.
struct UpstreamTransport {
    // Sends one request to one endpoint and reads the response.
    //
    // `stats` is the *calling cluster's* counters, passed per call rather than
    // bound once at construction. That is not a style preference: the pool
    // records hits, misses and connection churn into it, and binding a single
    // ClusterStats at construction meant those counters went into an object
    // the router never rendered — so an operator saw pool_hits=0 and
    // pool_misses=0 on a pool that was working fine. Per-call also attributes
    // connection churn to the right cluster when there is more than one.
    using Fn = std::function<Status(const Endpoint&, const Request&, Millis timeout,
                                    Response* out, bool* request_was_sent,
                                    ClusterStats* stats)>;
    Fn send;
};

// Per-endpoint policy state, which must persist across requests.
//
// Keyed by address rather than by index, because the index into a snapshot's
// endpoint list changes when the control plane publishes a new one, and a
// breaker that forgot its state on every config push would never open.
struct EndpointState {
    CircuitBreaker breaker;
    explicit EndpointState(CircuitBreaker::Config cfg) : breaker(cfg) {}
};

class Router {
  public:
    Router(RouteTablePtr table, UpstreamTransport transport);

    void SetTable(RouteTablePtr table);

    struct Outcome {
        Response response;
        Status status;
        uint32_t attempts = 0;
        std::string endpoint;
        bool shed = false;
        bool rate_limited = false;
        bool breaker_rejected = false;
    };

    // Routes one request. `now` is passed in so every policy decision is
    // testable on a virtual clock; nothing here reads the wall clock.
    //
    // `roll_source` supplies randomness for jitter and probabilistic shedding,
    // again so the tests can be exact rather than statistical.
    Outcome Route(const Request& request, const std::string& remote_address, TimePoint now,
                  const std::function<double()>& roll_source);

    const ClusterStats& stats(const std::string& cluster) const;
    std::unordered_map<std::string, ClusterStats> AllStats() const;

    void SetInFlight(uint32_t in_flight, uint32_t queue_depth);

    // Exposed for the metrics endpoint and for tests that assert on breaker
    // state transitions.
    CircuitBreaker::State BreakerState(const std::string& endpoint) const;
    double RetryBudgetTokens(const std::string& cluster) const;

  private:
    struct ClusterRuntime {
        explicit ClusterRuntime(const RouteConfig& cfg)
            : balancer(cfg.balancer),
              budget(cfg.retry_budget),
              limiter(cfg.rate_limit),
              shedder(cfg.shedder),
              retry(cfg.retry) {}
        Balancer balancer;
        RetryBudget budget;
        RateLimiter limiter;
        LoadShedder shedder;
        RetryPolicy retry;
        ClusterStats stats;
    };

    ClusterRuntime& RuntimeFor(const RouteConfig& route, const ClusterSnapshotPtr& cluster);
    EndpointState& StateFor(const std::string& address, const CircuitBreaker::Config& cfg);
    uint64_t HashFor(const RouteConfig& route, const Request& request,
                     const std::string& remote_address) const;

    RouteTablePtr table_;
    UpstreamTransport transport_;
    std::unordered_map<std::string, std::unique_ptr<ClusterRuntime>> clusters_;
    std::unordered_map<std::string, std::unique_ptr<EndpointState>> endpoints_;
    uint32_t in_flight_ = 0;
    uint32_t queue_depth_ = 0;
    uint64_t table_version_seen_ = 0;
};

// A real transport over the connection pool. Separated from Router so the
// router's tests never open a socket.
class PooledTransport {
  public:
    PooledTransport() : PooledTransport(ConnectionPool::Config{}) {}
    explicit PooledTransport(ConnectionPool::Config config) : pool_(config) {}

    UpstreamTransport::Fn Fn();
    ConnectionPool& pool() { return pool_; }

  private:
    ConnectionPool pool_;
};

// The listening server: accept loop plus a bounded worker pool.
class Server {
  public:
    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 8080;
        int backlog = 512;
        // Worker threads. Concurrency is bounded by this, not by file
        // descriptors — see the architecture note in socket.hpp.
        uint32_t workers = 8;
        // Queued-but-unstarted connections. This is the depth the load
        // shedder sheds on.
        uint32_t accept_queue = 256;
        Millis client_read_timeout{5000};
        Millis client_write_timeout{5000};
        Millis idle_timeout{30000};
        HttpLimits limits;
    };

    Server(Config config, Router* router);
    ~Server();

    Status Start();
    void Stop();
    uint16_t bound_port() const { return bound_port_; }

    // Serves until Stop(). Blocks.
    void Serve();

  private:
    void WorkerLoop();
    void HandleConnection(Socket client, const std::string& peer);

    Config config_;
    Router* router_;
    Socket listener_;
    uint16_t bound_port_ = 0;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace relay
