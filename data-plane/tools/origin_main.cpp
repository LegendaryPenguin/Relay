// A deliberately misbehaving origin server.
//
// Every failure-handling test needs an upstream that fails on command.
// Breaking a real server on request is not a thing you can do, so this one
// takes its misbehaviour as flags: fixed latency, an error rate, a hard
// failure window, and the ability to close connections without answering.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "relay/http.hpp"
#include "relay/socket.hpp"

using namespace relay;

namespace {

struct Options {
    uint16_t port = 9000;
    int latency_ms = 0;
    double error_rate = 0.0;
    int fail_after = -1;     // Start failing after N requests. -1 disables.
    bool hang = false;       // Accept and never answer.
    bool drop = false;       // Close without answering.
    std::string body = "origin\n";
};

std::atomic<uint64_t> g_served{0};

void Handle(Socket client, const Options& opt) {
    RequestParser parser;
    while (true) {
        std::string buf;
        bool eof = false;
        if (!client.Read(&buf, 64 * 1024, Millis(5000), &eof).ok() || (eof && buf.empty())) return;
        size_t consumed = 0;
        if (!parser.Feed(buf, &consumed).ok()) return;
        if (!parser.complete()) continue;

        uint64_t n = g_served.fetch_add(1) + 1;

        if (opt.hang) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return;
        }
        if (opt.drop) return;
        if (opt.latency_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.latency_ms));
        }

        bool fail = false;
        if (opt.fail_after >= 0 && n > static_cast<uint64_t>(opt.fail_after)) fail = true;
        if (opt.error_rate > 0.0) {
            // Deterministic rather than random, so a test asserting "the
            // breaker opened" does not depend on a seed.
            fail = fail || (n % 100) < static_cast<uint64_t>(opt.error_rate * 100);
        }

        Response r = fail ? Response::Simple(503, "Service Unavailable", "origin failing\n")
                          : Response::Simple(200, "OK", opt.body);
        r.headers.push_back({"x-origin-port", std::to_string(opt.port)});
        r.headers.push_back({"x-origin-served", std::to_string(n)});
        if (!client.WriteAll(r.Serialize(), Millis(5000)).ok()) return;
        if (!parser.request().keep_alive) return;
        parser.Reset();
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-port") opt.port = static_cast<uint16_t>(std::atoi(next().c_str()));
        else if (a == "-latency-ms") opt.latency_ms = std::atoi(next().c_str());
        else if (a == "-error-rate") opt.error_rate = std::atof(next().c_str());
        else if (a == "-fail-after") opt.fail_after = std::atoi(next().c_str());
        else if (a == "-hang") opt.hang = true;
        else if (a == "-drop") opt.drop = true;
        else if (a == "-body") opt.body = next() + "\n";
        else {
            std::fprintf(stderr, "usage: relay-origin [-port N] [-latency-ms N] "
                                 "[-error-rate F] [-fail-after N] [-hang] [-drop] [-body S]\n");
            return 2;
        }
    }

    Socket listener;
    Status s = Listen("127.0.0.1", opt.port, 128, &listener);
    if (!s.ok()) {
        std::fprintf(stderr, "listen: %s\n", s.ToString().c_str());
        return 1;
    }
    uint16_t bound = 0;
    LocalPort(listener, &bound);
    opt.port = bound;
    std::printf("origin listening on 127.0.0.1:%u\n", bound);
    std::fflush(stdout);

    std::vector<std::thread> threads;
    while (true) {
        Socket client;
        std::string peer;
        if (!Accept(listener, Millis(200), &client, &peer).ok()) continue;
        threads.emplace_back([c = std::move(client), &opt]() mutable { Handle(std::move(c), opt); });
        threads.erase(std::remove_if(threads.begin(), threads.end(),
                                     [](std::thread& t) { return !t.joinable(); }),
                      threads.end());
        if (threads.size() > 256) {
            for (auto& t : threads) if (t.joinable()) t.detach();
            threads.clear();
        }
    }
}
