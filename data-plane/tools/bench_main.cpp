// Microbenchmarks for the data plane's hot paths.
//
// No framework, matching the rest of the project. Medians over fixed
// iteration counts, no outlier rejection, single run per cell — where a
// number is too noisy to conclude from, bench/results.md says so rather than
// quoting a figure that implies a precision the measurement does not have.
//
// What is measured here is the per-request CPU of the *decision* path:
// parsing, balancing, hashing, policy checks. End-to-end proxy throughput is
// measured by scripts/stress.sh against a real origin, because it is dominated
// by syscalls and scheduling rather than by any of this.

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "relay/http.hpp"
#include "relay/metrics.hpp"
#include "relay/policy.hpp"
#include "relay/upstream.hpp"

using namespace relay;
using Nanos = std::chrono::nanoseconds;

namespace {

template <typename F>
void Bench(const char* label, size_t iterations, F&& fn) {
    auto start = std::chrono::steady_clock::now();
    fn(iterations);
    auto elapsed = std::chrono::steady_clock::now() - start;
    double ns = static_cast<double>(std::chrono::duration_cast<Nanos>(elapsed).count());
    double per_op = ns / static_cast<double>(iterations);
    double per_sec = 1e9 / per_op;
    std::printf("%-52s %9.1f ns/op  %12.0f op/sec\n", label, per_op, per_sec);
}

void Section(const char* title) { std::printf("\n-- %s --\n", title); }

ClusterSnapshotPtr MakeCluster(int count) {
    std::vector<Endpoint> eps;
    for (int i = 0; i < count; ++i) {
        Endpoint ep;
        ep.host = "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        ep.port = 8080;
        ep.weight = 1;
        ep.health = Health::kHealthy;
        eps.push_back(ep);
    }
    return std::make_shared<ClusterSnapshot>("c", std::move(eps));
}

}  // namespace

int main() {
    std::printf("relay data-plane microbenchmarks\n");

    Section("HTTP parsing");
    {
        const std::string simple =
            "GET /api/v1/resource?id=42 HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "User-Agent: relay-bench/1.0\r\n"
            "Accept: */*\r\n"
            "X-Request-Id: 0123456789abcdef\r\n"
            "\r\n";
        Bench("parse request, 5 headers, no body", 500000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) {
                RequestParser p;
                size_t consumed = 0;
                p.Feed(simple, &consumed);
            }
        });

        std::string with_body = "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: 1024\r\n\r\n";
        with_body += std::string(1024, 'a');
        Bench("parse request, 1KiB body", 200000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) {
                RequestParser p;
                size_t consumed = 0;
                p.Feed(with_body, &consumed);
            }
        });

        Request req;
        req.method = "GET";
        req.target = "/api/v1/resource";
        req.headers = {{"host", "example.com"}, {"user-agent", "x"}, {"accept", "*/*"}};
        Bench("serialize request", 500000, [&](size_t n) {
            size_t sink = 0;
            for (size_t i = 0; i < n; ++i) sink += req.Serialize().size();
            if (sink == 0) std::printf("");
        });
    }

    Section("Hashing and the ring");
    {
        Bench("fnv1a64 over a 32-byte path", 5000000, [](size_t n) {
            uint64_t sink = 0;
            std::string path = "/api/v1/users/12345678/profile";
            for (size_t i = 0; i < n; ++i) sink ^= Fnv1a64(path);
            if (sink == 1) std::printf("");
        });

        for (int count : {10, 100, 1000}) {
            auto cluster = MakeCluster(count);
            char label[96];
            std::snprintf(label, sizeof(label), "build ring, %d endpoints x160 vnodes", count);
            Bench(label, count <= 100 ? 2000 : 200, [&](size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    HashRing ring(*cluster);
                    if (ring.size() == 0) std::printf("");
                }
            });
        }

        auto cluster = MakeCluster(100);
        HashRing ring(*cluster);
        Bench("ring lookup, 100 endpoints (16k points)", 5000000, [&](size_t n) {
            size_t sink = 0;
            for (size_t i = 0; i < n; ++i) sink += ring.Lookup(i * 0x9e3779b9ull).value_or(0);
            if (sink == 1) std::printf("");
        });
    }

    Section("Load balancing");
    {
        struct Case { BalancerKind kind; const char* name; };
        const std::vector<Case> kinds = {
            {BalancerKind::kRoundRobin, "round_robin"},
            {BalancerKind::kRandom, "random"},
            {BalancerKind::kP2C, "p2c"},
            {BalancerKind::kLeastRequest, "least_request"},
            {BalancerKind::kRingHash, "ring_hash"},
        };
        for (int count : {10, 100, 1000}) {
            auto cluster = MakeCluster(count);
            for (const auto& c : kinds) {
                Balancer b(c.kind);
                b.SetSnapshot(cluster);
                char label[96];
                std::snprintf(label, sizeof(label), "pick: %-14s %4d endpoints", c.name, count);
                Bench(label, 1000000, [&](size_t n) {
                    for (size_t i = 0; i < n; ++i) {
                        auto idx = b.Pick(i * 0x9e3779b9ull);
                        if (idx.has_value()) {
                            b.OnRequestStart(*idx);
                            b.OnRequestEnd(*idx, false);
                        }
                    }
                });
            }
        }
    }

    Section("Policy checks");
    {
        TimePoint now = Clock::now();
        CircuitBreaker breaker;
        Bench("circuit breaker allow+success", 5000000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) {
                breaker.Allow(now);
                breaker.OnSuccess(now);
            }
        });

        RetryBudget budget;
        Bench("retry budget on_request", 5000000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) budget.OnRequest(true, now);
        });

        RateLimiter::Config rl;
        rl.per_second = 1e9;
        rl.burst = 1e9;
        RateLimiter limiter(rl);
        Bench("rate limiter, 1 key", 3000000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) limiter.Allow("10.1.2.3", now);
        });
        Bench("rate limiter, 10k rotating keys", 1000000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) {
                limiter.Allow("10.1.2." + std::to_string(i % 10000), now);
            }
        });

        LoadShedder shedder;
        Bench("load shedder admit", 10000000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) shedder.Admit(10, 10, 0.5);
        });
    }

    Section("Metrics");
    {
        LatencyHistogram h;
        Bench("histogram record", 10000000, [&](size_t n) {
            for (size_t i = 0; i < n; ++i) h.Record(i % 1000000);
        });
        Bench("histogram percentile", 1000000, [&](size_t n) {
            uint64_t sink = 0;
            for (size_t i = 0; i < n; ++i) sink += h.Percentile(0.99);
            if (sink == 1) std::printf("");
        });
    }

    std::printf("\n");
    return 0;
}
