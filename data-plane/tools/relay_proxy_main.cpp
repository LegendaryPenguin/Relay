// Command relay-proxy runs the data plane.
//
// Configuration comes from the Go control plane over HTTP in the full system;
// this binary accepts a static cluster list on the command line so the data
// plane can be run and benchmarked on its own. That separation is deliberate:
// a data plane that cannot start without its control plane is one you cannot
// debug in isolation.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include "relay/proxy.hpp"

using namespace relay;

namespace {

Server* g_server = nullptr;

void OnSignal(int) {
    if (g_server != nullptr) g_server->Stop();
}

std::vector<Endpoint> ParseEndpoints(const std::string& spec) {
    // "host:port[:weight],host:port[:weight]"
    std::vector<Endpoint> out;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos
                                                                         : comma - start);
        if (!item.empty()) {
            Endpoint ep;
            size_t c1 = item.find(':');
            if (c1 == std::string::npos) {
                std::fprintf(stderr, "bad endpoint %s\n", item.c_str());
                std::exit(2);
            }
            ep.host = item.substr(0, c1);
            size_t c2 = item.find(':', c1 + 1);
            if (c2 == std::string::npos) {
                ep.port = static_cast<uint16_t>(std::atoi(item.c_str() + c1 + 1));
            } else {
                ep.port = static_cast<uint16_t>(std::atoi(item.substr(c1 + 1, c2 - c1 - 1).c_str()));
                ep.weight = static_cast<uint32_t>(std::atoi(item.c_str() + c2 + 1));
            }
            // Endpoints given on the command line are trusted as healthy.
            // With no health checker running there is nothing to discover
            // otherwise, and leaving them Unknown would make the proxy refuse
            // all traffic.
            ep.health = Health::kHealthy;
            if (ep.weight == 0) ep.weight = 1;
            out.push_back(ep);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

BalancerKind ParseBalancer(const std::string& s) {
    if (s == "round_robin") return BalancerKind::kRoundRobin;
    if (s == "least_request") return BalancerKind::kLeastRequest;
    if (s == "p2c") return BalancerKind::kP2C;
    if (s == "random") return BalancerKind::kRandom;
    if (s == "ring_hash") return BalancerKind::kRingHash;
    std::fprintf(stderr, "unknown balancer %s\n", s.c_str());
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    Server::Config server_cfg;
    RouteConfig route;
    route.cluster = "default";
    route.path_prefix = "";
    std::string endpoints_spec = "127.0.0.1:9000";
    std::string balancer = "p2c";
    bool accept_queue_set = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-port") server_cfg.port = static_cast<uint16_t>(std::atoi(next().c_str()));
        else if (a == "-workers") server_cfg.workers = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "-upstreams") endpoints_spec = next();
        else if (a == "-balancer") balancer = next();
        else if (a == "-attempt-timeout-ms") route.per_attempt_timeout = Millis(std::atoi(next().c_str()));
        else if (a == "-deadline-ms") route.overall_deadline = Millis(std::atoi(next().c_str()));
        else if (a == "-max-attempts") route.retry.max_attempts = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "-retry-ratio") route.retry_budget.ratio = std::atof(next().c_str());
        else if (a == "-rate-limit") {
            route.rate_limit_enabled = true;
            route.rate_limit.per_second = std::atof(next().c_str());
        }
        else if (a == "-max-concurrent") route.shedder.max_concurrent = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "-queue-depth") {
            route.shedder.max_queue_depth = static_cast<uint32_t>(std::atoi(next().c_str()));
        }
        else if (a == "-accept-queue") {
            server_cfg.accept_queue = static_cast<uint32_t>(std::atoi(next().c_str()));
            accept_queue_set = true;
        }
        else {
            std::fprintf(stderr,
                "usage: relay-proxy [-port N] [-workers N] [-upstreams host:port[:w],...]\n"
                "                   [-balancer round_robin|least_request|p2c|random|ring_hash]\n"
                "                   [-attempt-timeout-ms N] [-deadline-ms N] [-max-attempts N]\n"
                "                   [-retry-ratio F] [-rate-limit F] [-max-concurrent N]\n"
                "                   [-queue-depth N] [-accept-queue N]\n");
            return 2;
        }
    }

    // The accept queue must be larger than the shed threshold, or load
    // shedding never runs.
    //
    // These were one flag at first, and the result was that every request
    // past the threshold got a TCP reset instead of a 503: the accept loop
    // dropped the connection before any worker could consult the shedder. A
    // client then cannot tell deliberate shedding from the proxy being dead,
    // and the whole point of shedding is to answer rather than vanish.
    //
    // 4x is slack enough that the shedder's probabilistic ramp has room to
    // operate across its whole range before the hard transport limit bites.
    if (!accept_queue_set) {
        server_cfg.accept_queue = route.shedder.max_queue_depth * 4;
    } else if (server_cfg.accept_queue <= route.shedder.max_queue_depth) {
        std::fprintf(stderr,
                     "warning: -accept-queue %u is not greater than -queue-depth %u; "
                     "excess load will be dropped as connection resets rather than 503s\n",
                     server_cfg.accept_queue, route.shedder.max_queue_depth);
    }

    route.balancer = ParseBalancer(balancer);
    if (route.balancer == BalancerKind::kRingHash) route.hash_source = HashKeySource::kPath;

    auto cluster = std::make_shared<ClusterSnapshot>(route.cluster, ParseEndpoints(endpoints_spec));
    auto table = std::make_shared<RouteTable>();
    {
        auto mutable_table = const_cast<RouteTable*>(table.get());
        mutable_table->AddRoute(route, cluster);
        mutable_table->Finalize();
    }

    PooledTransport transport;
    UpstreamTransport up;
    up.send = transport.Fn();

    Router router(table, up);
    Server server(server_cfg, &router);
    g_server = &server;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    Status s = server.Start();
    if (!s.ok()) {
        std::fprintf(stderr, "start: %s\n", s.ToString().c_str());
        return 1;
    }
    std::printf("relay-proxy listening on %s:%u -> %s (%s, %u workers)\n",
                server_cfg.host.c_str(), server.bound_port(), endpoints_spec.c_str(),
                BalancerKindName(route.balancer), server_cfg.workers);
    std::fflush(stdout);

    server.Serve();
    server.Stop();

    for (const auto& [name, stats] : router.AllStats()) {
        std::printf("\n%s", stats.Render(name).c_str());
    }
    return 0;
}
