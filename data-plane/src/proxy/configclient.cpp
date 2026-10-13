#include "relay/configclient.hpp"

#include <algorithm>
#include <unordered_map>

#include "relay/http.hpp"
#include "relay/socket.hpp"

namespace relay {

namespace {

Health ParseHealth(const std::string& s) {
    if (s == "healthy") return Health::kHealthy;
    if (s == "unhealthy") return Health::kUnhealthy;
    // Anything unrecognised is Unknown, which is *not* eligible for traffic.
    // Defaulting an unparseable health to healthy would send traffic on the
    // strength of a typo.
    return Health::kUnknown;
}

BalancerKind ParseBalancer(const std::string& s, bool* ok) {
    *ok = true;
    if (s == "round_robin") return BalancerKind::kRoundRobin;
    if (s == "least_request") return BalancerKind::kLeastRequest;
    if (s == "p2c") return BalancerKind::kP2C;
    if (s == "random") return BalancerKind::kRandom;
    if (s == "ring_hash") return BalancerKind::kRingHash;
    *ok = false;
    return BalancerKind::kP2C;
}

HashKeySource ParseHashSource(const std::string& s) {
    if (s == "path") return HashKeySource::kPath;
    if (s == "header") return HashKeySource::kHeader;
    if (s == "remote_address") return HashKeySource::kRemoteAddress;
    return HashKeySource::kNone;
}

}  // namespace

Status BuildRouteTableFromJson(const JsonValue& snapshot, RouteTablePtr* out,
                               std::string* warning) {
    if (!snapshot.is_object()) {
        return Status::InvalidRequest("snapshot is not a JSON object");
    }

    int64_t version = snapshot.IntField("version", 0);
    if (version <= 0) {
        // Version zero means the control plane has not published anything
        // yet. Applying it would replace a working table with an empty one.
        return Status::InvalidRequest("snapshot version is missing or zero");
    }

    const JsonValue* clusters_node = snapshot.Find("clusters");
    const JsonValue* routes_node = snapshot.Find("routes");
    if (clusters_node == nullptr || !clusters_node->is_array()) {
        return Status::InvalidRequest("snapshot has no clusters array");
    }
    if (routes_node == nullptr || !routes_node->is_array()) {
        return Status::InvalidRequest("snapshot has no routes array");
    }

    // Build cluster snapshots first, keyed by name, so routes can reference
    // them. One ClusterSnapshot per cluster, shared by every route that names
    // it — two routes over the same cluster must share the endpoint list, or
    // they would each get their own balancer state and the cluster's load
    // accounting would be split.
    std::unordered_map<std::string, ClusterSnapshotPtr> by_name;
    for (const auto& cluster_ptr : clusters_node->AsArray()) {
        if (!cluster_ptr || !cluster_ptr->is_object()) {
            return Status::InvalidRequest("a cluster entry is not an object");
        }
        const JsonValue& c = *cluster_ptr;
        std::string name = c.StringField("name");
        if (name.empty()) return Status::InvalidRequest("a cluster has no name");

        std::vector<Endpoint> endpoints;
        const JsonValue* eps = c.Find("endpoints");
        if (eps != nullptr && eps->is_array()) {
            for (const auto& ep_ptr : eps->AsArray()) {
                if (!ep_ptr || !ep_ptr->is_object()) {
                    return Status::InvalidRequest("an endpoint entry is not an object");
                }
                const JsonValue& e = *ep_ptr;
                Endpoint endpoint;
                endpoint.host = e.StringField("host");
                int64_t port = e.IntField("port", 0);
                if (endpoint.host.empty() || port <= 0 || port > 65535) {
                    return Status::InvalidRequest("endpoint " + endpoint.host +
                                                  " has no usable host:port");
                }
                endpoint.port = static_cast<uint16_t>(port);
                endpoint.weight = static_cast<uint32_t>(e.IntField("weight", 1));
                endpoint.health = ParseHealth(e.StringField("health", "unknown"));
                endpoint.zone = e.StringField("zone");
                endpoints.push_back(std::move(endpoint));
            }
        }

        auto cs = std::make_shared<ClusterSnapshot>(name, std::move(endpoints));
        const_cast<ClusterSnapshot*>(cs.get())->set_version(static_cast<uint64_t>(version));
        by_name[name] = cs;
    }

    auto table = std::make_shared<RouteTable>();
    auto* mutable_table = const_cast<RouteTable*>(table.get());
    size_t route_count = 0;

    for (const auto& route_ptr : routes_node->AsArray()) {
        if (!route_ptr || !route_ptr->is_object()) {
            return Status::InvalidRequest("a route entry is not an object");
        }
        const JsonValue& r = *route_ptr;

        RouteConfig route;
        route.cluster = r.StringField("cluster");
        if (route.cluster.empty()) return Status::InvalidRequest("a route has no cluster");
        auto it = by_name.find(route.cluster);
        if (it == by_name.end()) {
            return Status::InvalidRequest("route names unknown cluster " + route.cluster);
        }
        route.path_prefix = r.StringField("path_prefix");

        bool balancer_ok = true;
        route.balancer = ParseBalancer(r.StringField("balancer", "p2c"), &balancer_ok);
        if (!balancer_ok) {
            return Status::InvalidRequest("route " + route.path_prefix +
                                          " names an unknown balancer");
        }
        route.hash_source = ParseHashSource(r.StringField("hash_source", "none"));
        route.hash_header = ToLowerAscii(r.StringField("hash_header"));
        if (route.balancer == BalancerKind::kRingHash &&
            route.hash_source == HashKeySource::kNone) {
            // Ring hashing with no key hashes everything to zero and sends
            // the whole cluster's traffic to one endpoint.
            return Status::InvalidRequest("route " + route.path_prefix +
                                          " uses ring_hash with no hash source");
        }
        if (route.hash_source == HashKeySource::kHeader && route.hash_header.empty()) {
            return Status::InvalidRequest("route " + route.path_prefix +
                                          " hashes on a header but names none");
        }

        route.per_attempt_timeout = Millis(r.IntField("per_attempt_timeout_ms", 1000));
        route.overall_deadline = Millis(r.IntField("overall_deadline_ms", 3000));
        if (route.per_attempt_timeout.count() <= 0 || route.overall_deadline.count() <= 0) {
            return Status::InvalidRequest("route " + route.path_prefix +
                                          " has a non-positive timeout");
        }

        if (const JsonValue* retry = r.Find("retry"); retry != nullptr) {
            route.retry.max_attempts =
                static_cast<uint32_t>(retry->IntField("max_attempts", 3));
            route.retry.initial_backoff = Millis(retry->IntField("initial_backoff_ms", 10));
            route.retry.max_backoff = Millis(retry->IntField("max_backoff_ms", 500));
            route.retry.multiplier = retry->DoubleField("multiplier", 2.0);
            route.retry.jitter = retry->DoubleField("jitter", 0.5);
            route.retry_budget.ratio = retry->DoubleField("budget_ratio", 0.2);
            route.retry_budget.min_per_second = retry->DoubleField("budget_min_per_second", 3.0);
            route.retry_budget.burst = retry->DoubleField("budget_burst", 20.0);
            if (route.retry.max_attempts == 0) {
                return Status::InvalidRequest("route " + route.path_prefix +
                                              " permits zero attempts");
            }
        }

        if (const JsonValue* b = r.Find("breaker"); b != nullptr) {
            route.breaker.failure_threshold =
                static_cast<uint32_t>(b->IntField("failure_threshold", 5));
            route.breaker.minimum_requests =
                static_cast<uint32_t>(b->IntField("minimum_requests", 10));
            route.breaker.open_duration = Millis(b->IntField("open_duration_ms", 5000));
            route.breaker.half_open_probes =
                static_cast<uint32_t>(b->IntField("half_open_probes", 3));
            route.breaker.success_threshold =
                static_cast<uint32_t>(b->IntField("success_threshold", 2));
        }

        if (const JsonValue* s = r.Find("shedder"); s != nullptr) {
            route.shedder.max_concurrent =
                static_cast<uint32_t>(s->IntField("max_concurrent", 1024));
            route.shedder.max_queue_depth =
                static_cast<uint32_t>(s->IntField("max_queue_depth", 256));
            route.shedder.soft_threshold = s->DoubleField("soft_threshold", 0.5);
            if (route.shedder.soft_threshold < 0 || route.shedder.soft_threshold > 1) {
                return Status::InvalidRequest("route " + route.path_prefix +
                                              " soft_threshold is outside [0,1]");
            }
        }

        if (const JsonValue* rl = r.Find("rate_limit"); rl != nullptr) {
            route.rate_limit_enabled = rl->BoolField("enabled", false);
            route.rate_limit.per_second = rl->DoubleField("per_second", 1000.0);
            route.rate_limit.burst = rl->DoubleField("burst", 2000.0);
            int64_t max_keys = rl->IntField("max_keys", 65536);
            if (max_keys > 0) route.rate_limit.max_keys = static_cast<size_t>(max_keys);
        }

        mutable_table->AddRoute(route, it->second);
        ++route_count;
    }

    if (route_count == 0) {
        // An empty route table 404s every request. Refusing is better than
        // applying it, since the previous table at least routes something.
        return Status::InvalidRequest("snapshot has no routes");
    }

    mutable_table->Finalize();
    mutable_table->set_version(static_cast<uint64_t>(version));

    if (warning != nullptr) {
        size_t ineligible = 0;
        for (const auto& [name, cs] : by_name) {
            if (cs->eligible_count() == 0) ++ineligible;
        }
        if (ineligible > 0) {
            *warning = std::to_string(ineligible) +
                       " cluster(s) have no endpoints eligible for traffic";
        }
    }

    *out = table;
    return Status::Ok();
}

Status HttpGet(const std::string& host, uint16_t port, const std::string& path,
               const std::string& if_none_match, Millis timeout, HttpFetchResult* out) {
    Socket sock;
    Status s = Connect(host, port, timeout, &sock);
    if (!s.ok()) return s;

    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                          std::to_string(port) + "\r\n";
    if (!if_none_match.empty()) {
        request += "If-None-Match: " + if_none_match + "\r\n";
    }
    // Close rather than keep-alive. The config client makes one request per
    // poll interval, so pooling would buy nothing and the connection would
    // almost certainly be closed by the control plane between polls anyway.
    request += "Connection: close\r\n\r\n";

    s = sock.WriteAll(request, timeout);
    if (!s.ok()) return s;

    ResponseParser parser;
    std::string buffer;
    while (!parser.complete()) {
        buffer.clear();
        bool eof = false;
        s = sock.Read(&buffer, 64 * 1024, timeout, &eof);
        if (!s.ok()) return s;
        if (eof) {
            if (parser.MarkEof()) break;
            return Status::UpstreamFailure("control plane closed mid-response");
        }
        size_t consumed = 0;
        s = parser.Feed(buffer, &consumed);
        if (!s.ok()) return s;
    }

    const Response& response = parser.response();
    out->status = response.status;
    out->body = response.body;
    if (const std::string* etag = response.Find("etag"); etag != nullptr) {
        out->etag = *etag;
    }
    return Status::Ok();
}

ConfigClient::ConfigClient(Config config, Router* router)
    : config_(config), router_(router) {}

ConfigClient::~ConfigClient() { Stop(); }

Status ConfigClient::PollOnce() {
    HttpFetchResult result;
    Status s = HttpGet(config_.host, config_.port, config_.path, etag_, config_.timeout,
                       &result);

    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.polls;

    if (!s.ok()) {
        ++stats_.errors;
        stats_.last_error = s.ToString();
        return s;
    }

    if (result.status == 304) {
        // Unchanged. The whole point of the ETag.
        ++stats_.not_modified;
        return Status::Ok();
    }
    if (result.status != 200) {
        ++stats_.errors;
        stats_.last_error = "control plane returned " + std::to_string(result.status);
        return Status::UpstreamFailure(stats_.last_error);
    }

    JsonPtr parsed;
    Status ps = JsonParse(result.body, &parsed);
    if (!ps.ok() || !parsed) {
        ++stats_.rejected;
        stats_.last_error = "unparseable snapshot: " + ps.ToString();
        return Status::InvalidRequest(stats_.last_error);
    }

    RouteTablePtr table;
    std::string warning;
    Status bs = BuildRouteTableFromJson(*parsed, &table, &warning);
    if (!bs.ok()) {
        // Refused, and the previous table keeps serving. This is the one
        // place the data plane prefers stale-but-valid over fresh-but-broken:
        // the alternative is a proxy that stops routing because its control
        // plane shipped a typo.
        ++stats_.rejected;
        stats_.last_error = "refused snapshot: " + bs.ToString();
        return bs;
    }

    router_->SetTable(table);
    etag_ = result.etag;
    ++stats_.applied;
    stats_.version = table->version();
    stats_.last_error = warning;
    return Status::Ok();
}

void ConfigClient::Start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { Loop(); });
}

void ConfigClient::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void ConfigClient::Loop() {
    while (running_.load()) {
        PollOnce();
        // Sleep in small slices so Stop() is responsive without needing a
        // condition variable. A single sleep for the whole interval would
        // make shutdown wait out a poll.
        auto remaining = config_.poll_interval;
        while (running_.load() && remaining.count() > 0) {
            auto slice = std::min(remaining, Millis(100));
            std::this_thread::sleep_for(slice);
            remaining -= slice;
        }
    }
}

ConfigClient::Stats ConfigClient::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

}  // namespace relay
