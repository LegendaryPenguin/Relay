#include <string>

#include "relay/configclient.hpp"
#include "relay/json.hpp"
#include "testing.hpp"

using namespace relay;

// Tests for the control-plane -> data-plane configuration mapping.
//
// Driven from literal JSON with no server involved, which is the point: the
// mapping is where a field name typo or a unit mismatch silently produces a
// proxy that routes differently from what the operator configured, and that
// is testable without any network.

namespace {

Status Build(const std::string& json, RouteTablePtr* out, std::string* warning = nullptr) {
    JsonPtr parsed;
    Status s = JsonParse(json, &parsed);
    if (!s.ok()) return s;
    std::string local_warning;
    return BuildRouteTableFromJson(*parsed, out, warning ? warning : &local_warning);
}

const char* kMinimal = R"({
  "version": 3,
  "clusters": [{"name":"api","endpoints":[
    {"host":"10.0.0.1","port":8080,"weight":1,"health":"healthy"}
  ]}],
  "routes": [{"cluster":"api","path_prefix":""}]
})";

}  // namespace

RELAY_TEST(minimal_snapshot_builds) {
    RouteTablePtr table;
    CHECK_OK(Build(kMinimal, &table));
    CHECK(table != nullptr);
    CHECK_EQ(table->version(), uint64_t(3));
    CHECK_EQ(table->size(), size_t(1));

    auto match = table->Lookup("/anything");
    CHECK(match.route != nullptr);
    CHECK_EQ(match.route->cluster, std::string("api"));
    CHECK(match.cluster != nullptr);
    CHECK_EQ(match.cluster->eligible_count(), size_t(1));
}

RELAY_TEST(defaults_are_applied_when_fields_are_absent) {
    RouteTablePtr table;
    CHECK_OK(Build(kMinimal, &table));
    const RouteConfig& r = *table->Lookup("/x").route;

    CHECK_EQ(r.balancer, BalancerKind::kP2C);
    CHECK_EQ(r.per_attempt_timeout.count(), 1000);
    CHECK_EQ(r.overall_deadline.count(), 3000);
    CHECK_EQ(r.retry.max_attempts, uint32_t(3));
    // Jitter must never end up zero: without it every client that failed
    // together retries together.
    CHECK(r.retry.jitter > 0);
    CHECK(r.retry_budget.ratio > 0);
    CHECK(r.shedder.max_queue_depth > 0);
    CHECK(!r.rate_limit_enabled);
}

RELAY_TEST(every_field_maps_through) {
    // The unit-mismatch guard. A _ms field read as seconds produces a proxy
    // that times out a thousand times too late, and nothing else catches it.
    const char* full = R"({
      "version": 11,
      "clusters": [{"name":"api","endpoints":[
        {"host":"10.0.0.1","port":9090,"weight":3,"health":"healthy","zone":"za"}
      ]}],
      "routes": [{
        "cluster":"api","path_prefix":"/v2","balancer":"ring_hash",
        "hash_source":"header","hash_header":"X-Session",
        "per_attempt_timeout_ms":250,"overall_deadline_ms":900,
        "retry":{"max_attempts":5,"initial_backoff_ms":20,"max_backoff_ms":200,
                 "multiplier":3.0,"jitter":0.25,"budget_ratio":0.4,
                 "budget_min_per_second":7.0,"budget_burst":33.0},
        "breaker":{"failure_threshold":9,"minimum_requests":4,"open_duration_ms":1234,
                   "half_open_probes":6,"success_threshold":5},
        "shedder":{"max_concurrent":77,"max_queue_depth":88,"soft_threshold":0.25},
        "rate_limit":{"enabled":true,"per_second":12.5,"burst":25.0,"max_keys":128}
      }]
    })";
    RouteTablePtr table;
    CHECK_OK(Build(full, &table));
    const RouteConfig& r = *table->Lookup("/v2/thing").route;

    CHECK_EQ(r.balancer, BalancerKind::kRingHash);
    CHECK_EQ(r.hash_source, HashKeySource::kHeader);
    // Header names are lowercased on the way in, matching the parser.
    CHECK_EQ(r.hash_header, std::string("x-session"));
    CHECK_EQ(r.per_attempt_timeout.count(), 250);
    CHECK_EQ(r.overall_deadline.count(), 900);
    CHECK_EQ(r.retry.max_attempts, uint32_t(5));
    CHECK_EQ(r.retry.initial_backoff.count(), 20);
    CHECK_EQ(r.retry.max_backoff.count(), 200);
    CHECK_EQ(r.retry.multiplier, 3.0);
    CHECK_EQ(r.retry.jitter, 0.25);
    CHECK_EQ(r.retry_budget.ratio, 0.4);
    CHECK_EQ(r.retry_budget.min_per_second, 7.0);
    CHECK_EQ(r.retry_budget.burst, 33.0);
    CHECK_EQ(r.breaker.failure_threshold, uint32_t(9));
    CHECK_EQ(r.breaker.minimum_requests, uint32_t(4));
    CHECK_EQ(r.breaker.open_duration.count(), 1234);
    CHECK_EQ(r.breaker.half_open_probes, uint32_t(6));
    CHECK_EQ(r.breaker.success_threshold, uint32_t(5));
    CHECK_EQ(r.shedder.max_concurrent, uint32_t(77));
    CHECK_EQ(r.shedder.max_queue_depth, uint32_t(88));
    CHECK_EQ(r.shedder.soft_threshold, 0.25);
    CHECK(r.rate_limit_enabled);
    CHECK_EQ(r.rate_limit.per_second, 12.5);
    CHECK_EQ(r.rate_limit.burst, 25.0);
    CHECK_EQ(r.rate_limit.max_keys, size_t(128));

    const Endpoint& e = table->Lookup("/v2/x").cluster->endpoints()[0];
    CHECK_EQ(e.host, std::string("10.0.0.1"));
    CHECK_EQ(e.port, uint16_t(9090));
    CHECK_EQ(e.weight, uint32_t(3));
    CHECK_EQ(e.health, Health::kHealthy);
    CHECK_EQ(e.zone, std::string("za"));
}

RELAY_TEST(health_strings_map_and_unknown_is_not_eligible) {
    const char* json = R"({
      "version": 1,
      "clusters": [{"name":"api","endpoints":[
        {"host":"a","port":1,"weight":1,"health":"healthy"},
        {"host":"b","port":1,"weight":1,"health":"unhealthy"},
        {"host":"c","port":1,"weight":1,"health":"unknown"},
        {"host":"d","port":1,"weight":1,"health":"gibberish"}
      ]}],
      "routes": [{"cluster":"api","path_prefix":""}]
    })";
    RouteTablePtr table;
    CHECK_OK(Build(json, &table));
    const auto& eps = table->Lookup("/").cluster->endpoints();
    CHECK_EQ(eps[0].health, Health::kHealthy);
    CHECK_EQ(eps[1].health, Health::kUnhealthy);
    CHECK_EQ(eps[2].health, Health::kUnknown);
    // An unrecognised health is Unknown, not healthy. Defaulting it the other
    // way would send traffic on the strength of a typo.
    CHECK_EQ(eps[3].health, Health::kUnknown);
    CHECK_EQ(table->Lookup("/").cluster->eligible_count(), size_t(1));
}

RELAY_TEST(version_zero_is_refused) {
    // Version zero means the control plane has published nothing yet.
    // Applying it would replace a working table with an empty one.
    const char* json = R"({"version":0,"clusters":[{"name":"a","endpoints":[]}],
                           "routes":[{"cluster":"a","path_prefix":""}]})";
    RouteTablePtr table;
    CHECK_NOT_OK(Build(json, &table));
}

RELAY_TEST(route_to_unknown_cluster_is_refused) {
    const char* json = R"({"version":1,
      "clusters":[{"name":"api","endpoints":[]}],
      "routes":[{"cluster":"nope","path_prefix":""}]})";
    RouteTablePtr table;
    CHECK_NOT_OK(Build(json, &table));
}

RELAY_TEST(empty_route_list_is_refused) {
    // An empty table 404s every request; the previous table at least routes
    // something.
    const char* json = R"({"version":1,"clusters":[{"name":"a","endpoints":[]}],"routes":[]})";
    RouteTablePtr table;
    CHECK_NOT_OK(Build(json, &table));
}

RELAY_TEST(ring_hash_without_a_key_is_refused) {
    // Hashing everything to zero sends the whole cluster's traffic to one
    // endpoint.
    const char* json = R"({"version":1,
      "clusters":[{"name":"api","endpoints":[{"host":"a","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"api","path_prefix":"","balancer":"ring_hash","hash_source":"none"}]})";
    RouteTablePtr table;
    CHECK_NOT_OK(Build(json, &table));
}

RELAY_TEST(header_hash_without_a_header_name_is_refused) {
    const char* json = R"({"version":1,
      "clusters":[{"name":"api","endpoints":[{"host":"a","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"api","path_prefix":"","balancer":"ring_hash","hash_source":"header"}]})";
    RouteTablePtr table;
    CHECK_NOT_OK(Build(json, &table));
}

RELAY_TEST(unknown_balancer_is_refused) {
    // Silently falling back to a default would mean the proxy balances
    // differently from what the operator wrote.
    const char* json = R"({"version":1,
      "clusters":[{"name":"api","endpoints":[{"host":"a","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"api","path_prefix":"","balancer":"magic"}]})";
    RouteTablePtr table;
    CHECK_NOT_OK(Build(json, &table));
}

RELAY_TEST(bad_endpoints_are_refused) {
    const char* cases[] = {
        R"({"version":1,"clusters":[{"name":"a","endpoints":[{"port":1}]}],
            "routes":[{"cluster":"a","path_prefix":""}]})",
        R"({"version":1,"clusters":[{"name":"a","endpoints":[{"host":"h"}]}],
            "routes":[{"cluster":"a","path_prefix":""}]})",
        R"({"version":1,"clusters":[{"name":"a","endpoints":[{"host":"h","port":0}]}],
            "routes":[{"cluster":"a","path_prefix":""}]})",
        R"({"version":1,"clusters":[{"name":"a","endpoints":[{"host":"h","port":99999}]}],
            "routes":[{"cluster":"a","path_prefix":""}]})",
    };
    for (const char* json : cases) {
        RouteTablePtr table;
        if (Build(json, &table).ok()) {
            std::fprintf(stderr, "        accepted: %s\n", json);
            CHECK(false);
        }
    }
}

RELAY_TEST(nonsense_timeouts_and_thresholds_are_refused) {
    const char* zero_attempts = R"({"version":1,
      "clusters":[{"name":"a","endpoints":[{"host":"h","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"a","path_prefix":"","retry":{"max_attempts":0}}]})";
    RouteTablePtr table;
    CHECK_NOT_OK(Build(zero_attempts, &table));

    const char* bad_soft = R"({"version":1,
      "clusters":[{"name":"a","endpoints":[{"host":"h","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"a","path_prefix":"","shedder":{"soft_threshold":1.5}}]})";
    CHECK_NOT_OK(Build(bad_soft, &table));

    const char* zero_timeout = R"({"version":1,
      "clusters":[{"name":"a","endpoints":[{"host":"h","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"a","path_prefix":"","per_attempt_timeout_ms":0}]})";
    CHECK_NOT_OK(Build(zero_timeout, &table));
}

RELAY_TEST(routes_sharing_a_cluster_share_one_snapshot) {
    // Two routes over the same cluster must share the endpoint list, or each
    // gets its own balancer state and the cluster's load accounting splits.
    const char* json = R"({
      "version": 2,
      "clusters":[{"name":"api","endpoints":[{"host":"h","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"api","path_prefix":"/a"},{"cluster":"api","path_prefix":"/b"}]
    })";
    RouteTablePtr table;
    CHECK_OK(Build(json, &table));
    auto a = table->Lookup("/a/x");
    auto b = table->Lookup("/b/y");
    CHECK(a.cluster.get() == b.cluster.get());
}

RELAY_TEST(longest_prefix_wins_after_build) {
    const char* json = R"({
      "version": 2,
      "clusters":[{"name":"api","endpoints":[{"host":"h","port":1,"health":"healthy"}]}],
      "routes":[{"cluster":"api","path_prefix":"/a"},
                {"cluster":"api","path_prefix":"/a/b/c"},
                {"cluster":"api","path_prefix":""}]
    })";
    RouteTablePtr table;
    CHECK_OK(Build(json, &table));
    CHECK_EQ(table->Lookup("/a/b/c/d").route->path_prefix, std::string("/a/b/c"));
    CHECK_EQ(table->Lookup("/a/z").route->path_prefix, std::string("/a"));
    CHECK_EQ(table->Lookup("/other").route->path_prefix, std::string(""));
}

RELAY_TEST(a_cluster_with_nothing_eligible_warns_but_builds) {
    // Almost certainly a mistake, and also exactly what a snapshot looks like
    // mid-redeploy. Refusing would pin the proxy to the pre-deploy table,
    // which is worse.
    const char* json = R"({
      "version": 4,
      "clusters":[{"name":"api","endpoints":[{"host":"h","port":1,"health":"unhealthy"}]}],
      "routes":[{"cluster":"api","path_prefix":""}]
    })";
    RouteTablePtr table;
    std::string warning;
    CHECK_OK(Build(json, &table, &warning));
    CHECK(table != nullptr);
    CHECK(!warning.empty());
}

RELAY_TEST(a_non_object_snapshot_is_refused) {
    RouteTablePtr table;
    CHECK_NOT_OK(Build("[]", &table));
    CHECK_NOT_OK(Build("42", &table));
    CHECK_NOT_OK(Build("null", &table));
}

RELAY_TEST(router_can_be_retargeted_at_a_built_table) {
    // The end of the chain: a table built from JSON is usable by the router.
    RouteTablePtr table;
    CHECK_OK(Build(kMinimal, &table));

    UpstreamTransport transport;
    transport.send = [](const Endpoint& ep, const Request&, Millis, Response* out, bool* sent,
                        ClusterStats*) -> Status {
        *sent = true;
        *out = Response::Simple(200, "OK", ep.Address() + "\n");
        return Status::Ok();
    };

    Router router(table, transport);
    Request request;
    request.method = "GET";
    request.target = "/x";
    auto outcome = router.Route(request, "1.2.3.4", Clock::now(), [] { return 0.0; });
    CHECK(outcome.status.ok());
    CHECK_EQ(outcome.endpoint, std::string("10.0.0.1:8080"));

    // And a swap takes effect.
    const char* second = R"({
      "version": 9,
      "clusters":[{"name":"api","endpoints":[{"host":"10.0.0.9","port":80,"health":"healthy"}]}],
      "routes":[{"cluster":"api","path_prefix":""}]
    })";
    RouteTablePtr next;
    CHECK_OK(Build(second, &next));
    router.SetTable(next);
    outcome = router.Route(request, "1.2.3.4", Clock::now(), [] { return 0.0; });
    CHECK(outcome.status.ok());
    CHECK_EQ(outcome.endpoint, std::string("10.0.0.9:80"));
}

RELAY_TEST_MAIN()
