#include <functional>
#include <set>
#include <string>
#include <vector>

#include "relay/proxy.hpp"
#include "testing.hpp"

using namespace relay;

// Router tests use a fake transport, which is the seam that makes them
// possible. A router that called Connect() directly could only be tested
// against a real upstream, and "fail the third request but not the first two"
// is not something a real upstream does on request.

namespace {

TimePoint T0() { return TimePoint{} + Millis(1000000); }

struct FakeUpstream {
    // One scripted outcome per attempt; the last entry repeats.
    std::vector<Status> script;
    int response_status = 200;
    bool report_request_sent = true;

    std::vector<std::string> saw_endpoints;
    int calls = 0;

    UpstreamTransport::Fn Fn() {
        return [this](const Endpoint& ep, const Request&, Millis, Response* out,
                      bool* request_was_sent, ClusterStats*) -> Status {
            saw_endpoints.push_back(ep.Address());
            Status s = script.empty() ? Status::Ok()
                                      : script[std::min<size_t>(calls, script.size() - 1)];
            ++calls;
            *request_was_sent = report_request_sent;
            if (s.ok()) {
                *out = Response::Simple(response_status, "OK", "body\n");
            }
            return s;
        };
    }
};

ClusterSnapshotPtr Cluster(int count) {
    std::vector<Endpoint> eps;
    for (int i = 0; i < count; ++i) {
        Endpoint ep;
        ep.host = "10.0.0." + std::to_string(i + 1);
        ep.port = 8080;
        ep.weight = 1;
        ep.health = Health::kHealthy;
        eps.push_back(ep);
    }
    return std::make_shared<ClusterSnapshot>("api", std::move(eps));
}

RouteConfig BasicRoute() {
    RouteConfig r;
    r.cluster = "api";
    r.path_prefix = "";
    r.balancer = BalancerKind::kRoundRobin;
    r.retry.max_attempts = 3;
    r.retry.jitter = 0.0;
    r.retry.initial_backoff = Millis(1);
    r.breaker.minimum_requests = 0;
    r.breaker.failure_threshold = 2;
    r.retry_budget.burst = 100;
    r.retry_budget.min_per_second = 100;
    return r;
}

RouteTablePtr Table(RouteConfig route, ClusterSnapshotPtr cluster) {
    auto t = std::make_shared<RouteTable>();
    const_cast<RouteTable*>(t.get())->AddRoute(std::move(route), std::move(cluster));
    const_cast<RouteTable*>(t.get())->Finalize();
    return t;
}

Request Get(const char* target = "/x") {
    Request r;
    r.method = "GET";
    r.target = target;
    r.headers = {{"host", "example.com"}};
    return r;
}

Request Post(const char* target = "/x") {
    Request r;
    r.method = "POST";
    r.target = target;
    r.headers = {{"host", "example.com"}};
    r.body = "data";
    return r;
}

auto NoRoll() { return [] { return 0.0; }; }

}  // namespace

RELAY_TEST(happy_path) {
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    Router r(Table(BasicRoute(), Cluster(2)), t);

    auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
    CHECK(outcome.status.ok());
    CHECK_EQ(outcome.response.status, 200);
    CHECK_EQ(outcome.attempts, uint32_t(1));
    CHECK_EQ(r.stats("api").requests, uint64_t(1));
    CHECK_EQ(r.stats("api").responses_2xx, uint64_t(1));
}

RELAY_TEST(no_matching_route_is_404) {
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.path_prefix = "/api";
    Router r(Table(route, Cluster(1)), t);

    auto outcome = r.Route(Get("/other"), "1.2.3.4", T0(), NoRoll());
    CHECK_EQ(outcome.response.status, 404);
    CHECK_EQ(up.calls, 0);
}

RELAY_TEST(longest_prefix_wins) {
    auto table = std::make_shared<RouteTable>();
    RouteConfig broad = BasicRoute();
    broad.cluster = "broad";
    broad.path_prefix = "/a";
    RouteConfig narrow = BasicRoute();
    narrow.cluster = "narrow";
    narrow.path_prefix = "/a/b";
    // Added broad-first on purpose: Finalize must reorder them.
    const_cast<RouteTable*>(table.get())->AddRoute(broad, Cluster(1));
    const_cast<RouteTable*>(table.get())->AddRoute(narrow, Cluster(1));
    const_cast<RouteTable*>(table.get())->Finalize();

    auto match = table->Lookup("/a/b/c");
    CHECK(match.route != nullptr);
    CHECK_EQ(match.route->cluster, std::string("narrow"));

    match = table->Lookup("/a/z");
    CHECK_EQ(match.route->cluster, std::string("broad"));
}

RELAY_TEST(empty_cluster_is_503_without_retry) {
    // A retry would find the same empty set, so spending attempts on it is
    // pure latency.
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    auto empty = std::make_shared<ClusterSnapshot>("api", std::vector<Endpoint>{});
    Router r(Table(BasicRoute(), empty), t);

    auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
    CHECK_EQ(outcome.response.status, 503);
    CHECK_EQ(up.calls, 0);
    CHECK_EQ(r.stats("api").no_healthy_upstream, uint64_t(1));
}

RELAY_TEST(retries_a_retryable_failure) {
    FakeUpstream up;
    up.script = {Status::UpstreamFailure("boom"), Status::Ok()};
    UpstreamTransport t{up.Fn()};
    Router r(Table(BasicRoute(), Cluster(3)), t);

    auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
    CHECK(outcome.status.ok());
    CHECK_EQ(outcome.attempts, uint32_t(2));
    CHECK_EQ(r.stats("api").retries, uint64_t(1));
    // Round robin means the retry went to a different endpoint, which is the
    // point of retrying at all.
    CHECK(up.saw_endpoints.size() == 2);
    CHECK(up.saw_endpoints[0] != up.saw_endpoints[1]);
}

RELAY_TEST(stops_at_max_attempts) {
    FakeUpstream up;
    up.script = {Status::UpstreamFailure("boom")};
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.retry.max_attempts = 3;
    Router r(Table(route, Cluster(5)), t);

    auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
    CHECK(!outcome.status.ok());
    CHECK_EQ(outcome.attempts, uint32_t(3));
    CHECK_EQ(outcome.response.status, 502);
}

RELAY_TEST(does_not_retry_a_post_after_a_timeout) {
    // A timeout cannot distinguish "never arrived" from "arrived and the reply
    // was lost". Applying a POST twice is a correctness failure.
    FakeUpstream up;
    up.script = {Status::Timeout("slow")};
    up.report_request_sent = true;
    UpstreamTransport t{up.Fn()};
    Router r(Table(BasicRoute(), Cluster(5)), t);

    auto outcome = r.Route(Post(), "1.2.3.4", T0(), NoRoll());
    CHECK_EQ(outcome.attempts, uint32_t(1));
    CHECK_EQ(outcome.response.status, 504);
}

RELAY_TEST(does_retry_a_post_when_the_request_was_never_sent) {
    // The pooled-connection case: a write failure on a reused connection means
    // the upstream had already closed it and nothing was processed, so even a
    // POST is safe to retry. This is the fix for spurious 502s from pooling.
    FakeUpstream up;
    up.script = {Status::UpstreamFailure("connection reset"), Status::Ok()};
    up.report_request_sent = false;
    UpstreamTransport t{up.Fn()};
    Router r(Table(BasicRoute(), Cluster(3)), t);

    auto outcome = r.Route(Post(), "1.2.3.4", T0(), NoRoll());
    CHECK(outcome.status.ok());
    CHECK_EQ(outcome.attempts, uint32_t(2));
}

RELAY_TEST(retries_a_get_after_a_timeout) {
    FakeUpstream up;
    up.script = {Status::Timeout("slow"), Status::Ok()};
    UpstreamTransport t{up.Fn()};
    Router r(Table(BasicRoute(), Cluster(3)), t);

    auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
    CHECK(outcome.status.ok());
    CHECK_EQ(outcome.attempts, uint32_t(2));
}

RELAY_TEST(retry_budget_bounds_aggregate_amplification) {
    // The attempt count bounds amplification per request; only the budget
    // bounds it across requests, which is what matters when everything starts
    // failing at once.
    FakeUpstream up;
    up.script = {Status::UpstreamFailure("boom")};
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.retry.max_attempts = 3;
    route.retry_budget.burst = 4;
    route.retry_budget.ratio = 0.0;
    route.retry_budget.min_per_second = 0.0;  // No accrual.
    Router r(Table(route, Cluster(5)), t);

    TimePoint now = T0();
    for (int i = 0; i < 20; ++i) r.Route(Get(), "1.2.3.4", now, NoRoll());

    // Only 4 retries were ever affordable, after which requests fail on their
    // first attempt.
    CHECK_EQ(r.stats("api").retries, uint64_t(4));
    CHECK(r.stats("api").retries_denied_by_budget > 0);
}

RELAY_TEST(breaker_is_per_endpoint_not_per_cluster) {
    // A per-cluster breaker takes healthy endpoints out along with the sick
    // one, turning one bad host into a total outage. This was a real bug.
    //
    // The fake fails only for 10.0.0.1.
    int calls = 0;
    std::vector<std::string> seen;
    UpstreamTransport t;
    t.send = [&](const Endpoint& ep, const Request&, Millis, Response* out,
                 bool* sent, ClusterStats*) -> Status {
        ++calls;
        *sent = true;
        seen.push_back(ep.Address());
        if (ep.host == "10.0.0.1") return Status::UpstreamFailure("this host is sick");
        *out = Response::Simple(200, "OK", "ok\n");
        return Status::Ok();
    };

    RouteConfig route = BasicRoute();
    route.breaker.failure_threshold = 2;
    route.breaker.minimum_requests = 0;
    route.breaker.open_duration = Millis(10000);
    Router r(Table(route, Cluster(2)), t);

    TimePoint now = T0();
    // Drive enough traffic to open the breaker on the bad host.
    for (int i = 0; i < 10; ++i) r.Route(Get(), "1.2.3.4", now, NoRoll());

    CHECK_EQ(r.BreakerState("10.0.0.1:8080"), CircuitBreaker::State::kOpen);
    // The healthy host's breaker must still be closed.
    CHECK_EQ(r.BreakerState("10.0.0.2:8080"), CircuitBreaker::State::kClosed);

    // And requests still succeed, via the healthy endpoint.
    auto outcome = r.Route(Get(), "1.2.3.4", now, NoRoll());
    CHECK(outcome.status.ok());
}

RELAY_TEST(load_shed_does_not_charge_the_breaker) {
    // Shedding is the proxy protecting the upstream. Letting it trip the
    // breaker converts self-protection into a self-inflicted outage.
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.shedder.max_queue_depth = 10;
    route.shedder.soft_threshold = 0.0;  // Shed from depth 1.
    route.breaker.failure_threshold = 1;
    route.breaker.minimum_requests = 0;
    Router r(Table(route, Cluster(1)), t);

    r.SetInFlight(0, 10);  // At the hard limit: shed everything.
    for (int i = 0; i < 20; ++i) {
        auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
        CHECK(outcome.shed);
        CHECK_EQ(outcome.response.status, 503);
    }
    CHECK_EQ(up.calls, 0);
    CHECK_EQ(r.stats("api").load_shed, uint64_t(20));
    // The breaker never saw a failure, because none of this was the
    // upstream's fault.
    CHECK_EQ(r.BreakerState("10.0.0.1:8080"), CircuitBreaker::State::kClosed);
}

RELAY_TEST(rate_limit_rejects_with_429_and_no_upstream_call) {
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.rate_limit_enabled = true;
    route.rate_limit.per_second = 0.001;
    route.rate_limit.burst = 2;
    Router r(Table(route, Cluster(1)), t);

    TimePoint now = T0();
    CHECK(r.Route(Get(), "9.9.9.9", now, NoRoll()).status.ok());
    CHECK(r.Route(Get(), "9.9.9.9", now, NoRoll()).status.ok());
    auto limited = r.Route(Get(), "9.9.9.9", now, NoRoll());
    CHECK(limited.rate_limited);
    CHECK_EQ(limited.response.status, 429);
    CHECK_EQ(up.calls, 2);
    // A Retry-After tells the client when to come back rather than leaving it
    // to guess.
    CHECK(limited.response.Find("retry-after") != nullptr);
}

RELAY_TEST(rate_limit_is_per_client) {
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.rate_limit_enabled = true;
    route.rate_limit.per_second = 0.001;
    route.rate_limit.burst = 1;
    Router r(Table(route, Cluster(1)), t);

    TimePoint now = T0();
    CHECK(r.Route(Get(), "1.1.1.1", now, NoRoll()).status.ok());
    CHECK(r.Route(Get(), "1.1.1.1", now, NoRoll()).rate_limited);
    // A different client is unaffected.
    CHECK(r.Route(Get(), "2.2.2.2", now, NoRoll()).status.ok());
}

RELAY_TEST(overall_deadline_caps_total_latency) {
    // Three 1s attempts is 3s of client latency, and a client with a 1s
    // budget does not care that each attempt honoured its own.
    FakeUpstream up;
    up.script = {Status::Timeout("slow")};
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.retry.max_attempts = 10;
    route.retry.initial_backoff = Millis(400);
    route.retry.max_backoff = Millis(400);
    route.retry.jitter = 0.0;
    route.overall_deadline = Millis(1000);
    Router r(Table(route, Cluster(10)), t);

    auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
    // 400ms of backoff per retry against a 1s budget allows far fewer than 10
    // attempts.
    CHECK(outcome.attempts < 5);
    CHECK_EQ(outcome.response.status, 504);
}

RELAY_TEST(upstream_5xx_is_a_response_not_a_transport_failure) {
    // An upstream returning 500 for one bad route should not open the breaker
    // for all of its traffic. Whether 5xx should count is a real policy
    // question; this picks the conservative side.
    FakeUpstream up;
    up.response_status = 500;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.breaker.failure_threshold = 1;
    route.breaker.minimum_requests = 0;
    Router r(Table(route, Cluster(1)), t);

    for (int i = 0; i < 10; ++i) {
        auto outcome = r.Route(Get(), "1.2.3.4", T0(), NoRoll());
        CHECK(outcome.status.ok());
        CHECK_EQ(outcome.response.status, 500);
        CHECK_EQ(outcome.attempts, uint32_t(1));  // Not retried.
    }
    CHECK_EQ(r.BreakerState("10.0.0.1:8080"), CircuitBreaker::State::kClosed);
    CHECK_EQ(r.stats("api").responses_5xx, uint64_t(10));
}

RELAY_TEST(ring_hash_routing_is_sticky_per_path) {
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.balancer = BalancerKind::kRingHash;
    route.hash_source = HashKeySource::kPath;
    Router r(Table(route, Cluster(5)), t);

    auto first = r.Route(Get("/user/42"), "1.2.3.4", T0(), NoRoll());
    for (int i = 0; i < 10; ++i) {
        auto again = r.Route(Get("/user/42"), "9.9.9.9", T0(), NoRoll());
        CHECK_EQ(again.endpoint, first.endpoint);
    }
}

RELAY_TEST(ring_hash_ignores_the_query_string) {
    // A cache key that varies with an irrelevant query parameter defeats the
    // affinity the ring exists to provide.
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.balancer = BalancerKind::kRingHash;
    route.hash_source = HashKeySource::kPath;
    Router r(Table(route, Cluster(5)), t);

    auto a = r.Route(Get("/user/42"), "1.2.3.4", T0(), NoRoll());
    auto b = r.Route(Get("/user/42?t=1"), "1.2.3.4", T0(), NoRoll());
    auto c = r.Route(Get("/user/42?t=99999"), "1.2.3.4", T0(), NoRoll());
    CHECK_EQ(a.endpoint, b.endpoint);
    CHECK_EQ(a.endpoint, c.endpoint);
}

RELAY_TEST(missing_hash_header_falls_back_to_path_not_a_constant) {
    // Hashing a constant would send every such request to one endpoint, which
    // under partial client rollout is a hot spot that looks like a
    // load-balancer bug.
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    RouteConfig route = BasicRoute();
    route.balancer = BalancerKind::kRingHash;
    route.hash_source = HashKeySource::kHeader;
    route.hash_header = "x-session";
    Router r(Table(route, Cluster(8)), t);

    std::set<std::string> endpoints;
    for (int i = 0; i < 50; ++i) {
        auto outcome = r.Route(Get(("/path-" + std::to_string(i)).c_str()), "1.2.3.4", T0(),
                               NoRoll());
        endpoints.insert(outcome.endpoint);
    }
    // Distinct paths must spread, not collapse onto one endpoint.
    CHECK(endpoints.size() > 1);
}

RELAY_TEST(table_swap_preserves_breaker_state) {
    // A control plane that pushes every few seconds would make the breakers
    // permanently useless if a push reset them.
    UpstreamTransport t;
    t.send = [](const Endpoint&, const Request&, Millis, Response*, bool* sent,
                ClusterStats*) -> Status {
        *sent = true;
        return Status::UpstreamFailure("always down");
    };
    RouteConfig route = BasicRoute();
    route.breaker.failure_threshold = 2;
    route.breaker.minimum_requests = 0;
    route.breaker.open_duration = Millis(60000);
    auto cluster = Cluster(1);
    Router r(Table(route, cluster), t);

    TimePoint now = T0();
    for (int i = 0; i < 5; ++i) r.Route(Get(), "1.2.3.4", now, NoRoll());
    CHECK_EQ(r.BreakerState("10.0.0.1:8080"), CircuitBreaker::State::kOpen);

    // A new table with the same membership.
    r.SetTable(Table(route, Cluster(1)));
    CHECK_EQ(r.BreakerState("10.0.0.1:8080"), CircuitBreaker::State::kOpen);
}

RELAY_TEST(stats_are_per_cluster) {
    FakeUpstream up;
    UpstreamTransport t{up.Fn()};
    auto table = std::make_shared<RouteTable>();
    RouteConfig a = BasicRoute();
    a.cluster = "alpha";
    a.path_prefix = "/a";
    RouteConfig b = BasicRoute();
    b.cluster = "beta";
    b.path_prefix = "/b";
    const_cast<RouteTable*>(table.get())->AddRoute(a, Cluster(1));
    const_cast<RouteTable*>(table.get())->AddRoute(b, Cluster(1));
    const_cast<RouteTable*>(table.get())->Finalize();
    Router r(table, t);

    r.Route(Get("/a/x"), "1.2.3.4", T0(), NoRoll());
    r.Route(Get("/b/y"), "1.2.3.4", T0(), NoRoll());
    r.Route(Get("/b/z"), "1.2.3.4", T0(), NoRoll());

    CHECK_EQ(r.stats("alpha").requests, uint64_t(1));
    CHECK_EQ(r.stats("beta").requests, uint64_t(2));
    CHECK_EQ(r.AllStats().size(), size_t(2));
}

RELAY_TEST_MAIN()
