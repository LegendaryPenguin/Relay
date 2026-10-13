#include "relay/proxy.hpp"

#include <algorithm>
#include <chrono>

namespace relay {

void RouteTable::AddRoute(RouteConfig route, ClusterSnapshotPtr cluster) {
    routes_.push_back(Entry{std::move(route), std::move(cluster)});
}

void RouteTable::Finalize() {
    // Longest prefix first, so Lookup's linear scan returns the most specific
    // match. Sorting at build time rather than scoring at request time keeps
    // the hot path a comparison per route and nothing else.
    std::sort(routes_.begin(), routes_.end(), [](const Entry& a, const Entry& b) {
        if (a.route.path_prefix.size() != b.route.path_prefix.size()) {
            return a.route.path_prefix.size() > b.route.path_prefix.size();
        }
        return a.route.path_prefix < b.route.path_prefix;
    });
}

RouteTable::Match RouteTable::Lookup(std::string_view target) const {
    for (const auto& entry : routes_) {
        const std::string& prefix = entry.route.path_prefix;
        if (prefix.empty() || target.rfind(prefix, 0) == 0) {
            return Match{&entry.route, entry.cluster};
        }
    }
    return Match{};
}

Router::Router(RouteTablePtr table, UpstreamTransport transport)
    : table_(std::move(table)), transport_(std::move(transport)) {
    if (table_) table_version_seen_ = table_->version();
}

void Router::SetTable(RouteTablePtr table) {
    {
        std::lock_guard<std::mutex> lock(table_mu_);
        table_ = std::move(table);
        if (!table_) return;
        table_version_seen_ = table_->version();
    }
    // Cluster runtimes are deliberately NOT rebuilt here. Each one's balancer
    // is re-pointed at the new snapshot lazily, on its next use, in
    // RuntimeFor.
    //
    // Rebuilding them eagerly would reset every circuit breaker and empty
    // every retry budget on each config push, so a cluster in a degraded
    // state would forget it was degraded every time the control plane said
    // anything — and a control plane that pushes every few seconds would make
    // the breakers permanently useless.
}

Router::ClusterRuntime& Router::RuntimeFor(const RouteConfig& route,
                                           const ClusterSnapshotPtr& cluster) {
    auto it = clusters_.find(route.cluster);
    if (it == clusters_.end()) {
        auto runtime = std::make_unique<ClusterRuntime>(route);
        runtime->balancer.SetSnapshot(cluster);
        it = clusters_.emplace(route.cluster, std::move(runtime)).first;
        return *it->second;
    }
    // Refresh the snapshot if the control plane published a newer one. Compared
    // by pointer identity first because that is the common case and is free.
    const auto& current = it->second->balancer.snapshot();
    if (current.get() != cluster.get()) {
        it->second->balancer.SetSnapshot(cluster);
    }
    return *it->second;
}

EndpointState& Router::StateFor(const std::string& address,
                                const CircuitBreaker::Config& cfg) {
    auto it = endpoints_.find(address);
    if (it == endpoints_.end()) {
        it = endpoints_.emplace(address, std::make_unique<EndpointState>(cfg)).first;
    }
    return *it->second;
}

uint64_t Router::HashFor(const RouteConfig& route, const Request& request,
                         const std::string& remote_address) const {
    switch (route.hash_source) {
        case HashKeySource::kNone:
            return 0;
        case HashKeySource::kPath: {
            // Hash the path only, excluding the query string. A cache key that
            // varies with an irrelevant query parameter defeats the affinity
            // the ring exists to provide — which is the whole reason anyone
            // turns ring hashing on.
            std::string_view target = request.target;
            size_t q = target.find('?');
            if (q != std::string_view::npos) target = target.substr(0, q);
            return Fnv1a64(target);
        }
        case HashKeySource::kHeader: {
            const std::string* v = request.Find(route.hash_header);
            // A request missing the hash header falls back to the path rather
            // than to a constant. Hashing a constant would send every such
            // request to one endpoint, which under partial client rollout is
            // a hot spot that looks like a load-balancer bug.
            if (v == nullptr) return Fnv1a64(request.target);
            return Fnv1a64(*v);
        }
        case HashKeySource::kRemoteAddress:
            return Fnv1a64(remote_address);
    }
    return 0;
}

void Router::SetInFlight(uint32_t in_flight, uint32_t queue_depth) {
    in_flight_ = in_flight;
    queue_depth_ = queue_depth;
}

Router::Outcome Router::Route(const Request& request, const std::string& remote_address,
                              TimePoint now, const std::function<double()>& roll_source) {
    Outcome outcome;
    const TimePoint started = now;

    // One copy of the table pointer for the whole request. Re-reading it
    // mid-request would let a config push change the cluster between the
    // endpoint selection and the retry, so a retry could land in a different
    // cluster than the first attempt.
    RouteTablePtr table;
    {
        std::lock_guard<std::mutex> lock(table_mu_);
        table = table_;
    }

    if (!table) {
        outcome.status = Status::Internal("no route table");
        outcome.response = Response::Simple(500, "Internal Server Error", "no route table\n");
        return outcome;
    }

    RouteTable::Match match = table->Lookup(request.target);
    if (match.route == nullptr || !match.cluster) {
        outcome.status = Status::InvalidRequest("no route matches " + request.target);
        outcome.response = Response::Simple(404, "Not Found", "no route\n");
        return outcome;
    }

    const RouteConfig& route = *match.route;
    ClusterRuntime& rt = RuntimeFor(route, match.cluster);
    ++rt.stats.requests;

    // --- 1. Rate limit. Cheapest check, and the only one that bounds a single
    // abusive client, so it runs before any resource is committed.
    if (route.rate_limit_enabled && !rt.limiter.Allow(remote_address, now)) {
        ++rt.stats.rate_limited;
        outcome.rate_limited = true;
        outcome.status = Status::Overloaded("rate limit exceeded");
        outcome.response = Response::Simple(429, "Too Many Requests", "rate limited\n");
        outcome.response.headers.push_back({"retry-after", "1"});
        return outcome;
    }

    // --- 2. Load shed on queue depth. Before endpoint selection, because
    // selecting for a request about to be rejected wastes the selection and
    // wrongly increments the endpoint's load counters.
    if (!rt.shedder.Admit(in_flight_, queue_depth_, roll_source())) {
        ++rt.stats.load_shed;
        outcome.shed = true;
        // Overloaded, so it does not charge the breaker. Shedding is the proxy
        // protecting the upstream; letting it trip the breaker would convert
        // self-protection into a self-inflicted outage.
        outcome.status = Status::Overloaded("load shed at queue depth " +
                                            std::to_string(queue_depth_));
        outcome.response = Response::Simple(503, "Service Unavailable", "load shed\n");
        outcome.response.headers.push_back({"retry-after", "1"});
        return outcome;
    }

    uint64_t key_hash = HashFor(route, request, remote_address);
    Status last = Status::NoHealthyUpstream("no attempt made");

    for (uint32_t attempt = 1; attempt <= route.retry.max_attempts; ++attempt) {
        // --- 5b. Overall deadline, checked before every attempt. Without it
        // N attempts at T each is N*T of client latency, and a client with a
        // 1s budget does not care that each of our attempts honoured 400ms.
        auto elapsed = std::chrono::duration_cast<Millis>(now - started);
        if (elapsed >= route.overall_deadline) {
            last = Status::Timeout("overall deadline exceeded");
            break;
        }
        Millis remaining = route.overall_deadline - elapsed;
        Millis attempt_timeout = std::min(route.per_attempt_timeout, remaining);

        // --- 3. Select an endpoint.
        auto picked = rt.balancer.Pick(key_hash);
        if (!picked.has_value()) {
            ++rt.stats.no_healthy_upstream;
            last = Status::NoHealthyUpstream("cluster " + route.cluster + " has no eligible endpoints");
            break;  // A retry would find the same empty set.
        }
        size_t index = *picked;
        const Endpoint& endpoint = match.cluster->endpoints()[index];
        std::string address = endpoint.Address();

        // --- 4. Breaker, per endpoint rather than per cluster. A per-cluster
        // breaker takes healthy endpoints out along with the sick one, turning
        // one bad host into a total outage.
        EndpointState& state = StateFor(address, route.breaker);
        if (!state.breaker.Allow(now)) {
            ++rt.stats.breaker_rejections;
            outcome.breaker_rejected = true;
            last = Status::UpstreamFailure("circuit open for " + address);
            // Try a different endpoint on the next iteration rather than
            // giving up: one open breaker says nothing about the others.
            // Charged to the attempt count so a cluster of open breakers
            // cannot spin.
            ++outcome.attempts;
            continue;
        }

        rt.balancer.OnRequestStart(index);
        TimePoint attempt_started = now;
        Response response;
        bool request_was_sent = false;
        Status s = transport_.send(endpoint, request, attempt_timeout, &response,
                                   &request_was_sent, &rt.stats);
        auto attempt_micros = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - attempt_started)
                .count());
        rt.stats.upstream_latency.Record(attempt_micros);

        ++outcome.attempts;
        outcome.endpoint = address;
        rt.balancer.OnRequestEnd(index, !s.ok());
        rt.budget.OnRequest(s.ok(), now);

        if (s.ok()) {
            state.breaker.OnSuccess(now);
            if (response.status >= 500) {
                ++rt.stats.responses_5xx;
                // A 5xx is a response, not a transport failure, and it is
                // deliberately NOT charged to the breaker. An upstream
                // returning 500 for one bad route would otherwise open the
                // breaker for all of its traffic. Whether 5xx should count is
                // a real policy question and the honest answer is that it
                // depends on the upstream — this picks the conservative side
                // and the README says so.
            } else if (response.status >= 400) {
                ++rt.stats.responses_4xx;
            } else {
                ++rt.stats.responses_2xx;
            }
            outcome.response = std::move(response);
            outcome.status = Status::Ok();
            rt.stats.total_latency.Record(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started)
                    .count()));
            return outcome;
        }

        last = s;
        if (s.code() == Code::kTimeout) ++rt.stats.timeouts;
        if (s.code() == Code::kUpstreamFailure) ++rt.stats.upstream_failures;
        if (s.charges_breaker()) state.breaker.OnFailure(now);

        // Retry safety. A failure where the request was never written cannot
        // have been processed, so even a POST is safe to retry — this is why
        // the transport reports `request_was_sent` rather than the router
        // guessing from the error.
        bool safe_to_retry = request.idempotent() || !request_was_sent;
        if (!rt.retry.ShouldRetry(outcome.attempts, s, safe_to_retry)) break;

        // --- 6. The retry budget, consulted *in addition to* the attempt
        // count. The count bounds amplification per request at max_attempts;
        // only the budget bounds it in aggregate, which is what matters when
        // every request starts failing at once.
        if (!rt.budget.TryConsume(now)) {
            ++rt.stats.retries_denied_by_budget;
            break;
        }
        ++rt.stats.retries;

        Millis backoff = rt.retry.Backoff(outcome.attempts, roll_source());
        // The clock is advanced rather than slept. The caller owns real
        // sleeping; this keeps Route a pure function of its inputs, which is
        // what makes the retry tests exact instead of timing-dependent.
        now += backoff;
    }

    outcome.status = last;
    int http = last.http_status();
    if (http >= 500) ++rt.stats.responses_5xx;
    outcome.response = Response::Simple(http, http == 504 ? "Gateway Timeout" : "Bad Gateway",
                                        last.ToString() + "\n");
    rt.stats.total_latency.Record(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count()));
    return outcome;
}

const ClusterStats& Router::stats(const std::string& cluster) const {
    static const ClusterStats kEmpty{};
    auto it = clusters_.find(cluster);
    return it == clusters_.end() ? kEmpty : it->second->stats;
}

std::unordered_map<std::string, ClusterStats> Router::AllStats() const {
    std::unordered_map<std::string, ClusterStats> out;
    for (const auto& [name, rt] : clusters_) out[name] = rt->stats;
    return out;
}

CircuitBreaker::State Router::BreakerState(const std::string& endpoint) const {
    auto it = endpoints_.find(endpoint);
    if (it == endpoints_.end()) return CircuitBreaker::State::kClosed;
    return it->second->breaker.state();
}

double Router::RetryBudgetTokens(const std::string& cluster) const {
    auto it = clusters_.find(cluster);
    return it == clusters_.end() ? 0.0 : it->second->budget.tokens();
}

// ---------------------------------------------------------------------------
// PooledTransport
// ---------------------------------------------------------------------------

UpstreamTransport::Fn PooledTransport::Fn() {
    return [this](const Endpoint& endpoint, const Request& request, Millis timeout,
                  Response* out, bool* request_was_sent, ClusterStats* stats) -> Status {
        *request_was_sent = false;

        ConnectionPool::Lease lease;
        Status s = pool_.Acquire(endpoint.host, endpoint.port, Clock::now(), &lease, stats);
        if (!s.ok()) return s;

        std::string wire = request.Serialize();
        s = lease.socket().WriteAll(wire, timeout);
        if (!s.ok()) {
            pool_.Discard(std::move(lease), stats);
            // A write failure on a *reused* connection almost certainly means
            // the upstream had already closed it, and nothing was processed.
            // Leaving request_was_sent false lets the router retry even a
            // POST, which is the fix for the spurious-502-from-pooling
            // problem.
            return s;
        }
        // From here the upstream may have seen the request.
        *request_was_sent = true;

        ResponseParser parser;
        std::string buffer;
        while (!parser.complete()) {
            buffer.clear();
            bool eof = false;
            s = lease.socket().Read(&buffer, 64 * 1024, timeout, &eof);
            if (!s.ok()) {
                pool_.Discard(std::move(lease), stats);
                return s;
            }
            if (eof) {
                if (parser.MarkEof()) break;
                pool_.Discard(std::move(lease), stats);
                return Status::UpstreamFailure("upstream closed mid-response");
            }
            size_t consumed = 0;
            s = parser.Feed(buffer, &consumed);
            if (!s.ok()) {
                pool_.Discard(std::move(lease), stats);
                return s;
            }
        }

        *out = parser.response();
        if (out->keep_alive) {
            pool_.Release(std::move(lease), Clock::now(), stats);
        } else {
            pool_.Discard(std::move(lease), stats);
        }
        return Status::Ok();
    };
}

}  // namespace relay
