#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include "relay/proxy.hpp"

namespace relay {

struct Server::Impl {
    std::vector<std::thread> workers;
    std::atomic<bool> running{false};

    // Accepted-but-unstarted connections.
    //
    // This queue *is* the load shedder's signal. With a bounded worker pool,
    // excess concurrency has to go somewhere, and making it visible as a
    // depth rather than letting it pile up invisibly in the kernel's accept
    // backlog is what lets the proxy shed deliberately instead of just
    // getting slower.
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<Socket, std::string>> queue;

    std::atomic<uint32_t> in_flight{0};

    // Router access is serialised. The Router holds per-endpoint breakers and
    // per-cluster budgets as plain (non-atomic) state, so concurrent Route
    // calls would race on them.
    //
    // This is the honest limitation of the design and it is the throughput
    // ceiling measured in bench/results.md: routing decisions are
    // single-threaded even though I/O is not. The fix is per-worker policy
    // state with periodic merging, which trades exactness in the breakers for
    // parallelism — a real tradeoff I did not make, rather than an oversight.
    std::mutex router_mu;

    std::mt19937_64 rng{0x5eed};
    std::mutex rng_mu;
};

Server::Server(Config config, Router* router)
    : config_(config), router_(router), impl_(std::make_unique<Impl>()) {}

Server::~Server() { Stop(); }

Status Server::Start() {
    Status s = Listen(config_.host, config_.port, config_.backlog, &listener_);
    if (!s.ok()) return s;
    s = LocalPort(listener_, &bound_port_);
    if (!s.ok()) return s;

    impl_->running = true;
    for (uint32_t i = 0; i < config_.workers; ++i) {
        impl_->workers.emplace_back([this] { WorkerLoop(); });
    }
    return Status::Ok();
}

void Server::Stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    impl_->cv.notify_all();
    for (auto& t : impl_->workers) {
        if (t.joinable()) t.join();
    }
    impl_->workers.clear();
    listener_.Close();
}

void Server::Serve() {
    while (impl_->running.load()) {
        Socket client;
        std::string peer;
        // A bounded accept wait rather than a blocking accept, so the loop
        // gets a regular chance to notice shutdown without needing a signal
        // or a self-pipe.
        Status s = Accept(listener_, Millis(100), &client, &peer);
        if (!s.ok()) continue;

        {
            std::lock_guard<std::mutex> lock(impl_->mu);
            if (impl_->queue.size() >= config_.accept_queue) {
                // The queue is full, so the connection is refused at the
                // transport level rather than being accepted and then
                // rejected at the HTTP level.
                //
                // Dropping it here means the client sees a connection reset
                // rather than a 503, which is worse for diagnosis but is the
                // only option left: writing a 503 requires a worker, and the
                // reason we are here is that there are none. The load
                // shedder's probabilistic ramp exists so this hard limit is
                // rarely reached.
                continue;
            }
            impl_->queue.emplace_back(std::move(client), std::move(peer));
        }
        impl_->cv.notify_one();
    }
}

void Server::WorkerLoop() {
    while (true) {
        std::pair<Socket, std::string> work;
        {
            std::unique_lock<std::mutex> lock(impl_->mu);
            impl_->cv.wait(lock, [this] {
                return !impl_->queue.empty() || !impl_->running.load();
            });
            if (!impl_->running.load() && impl_->queue.empty()) return;
            if (impl_->queue.empty()) continue;
            work = std::move(impl_->queue.front());
            impl_->queue.pop_front();
        }
        impl_->in_flight.fetch_add(1);
        HandleConnection(std::move(work.first), work.second);
        impl_->in_flight.fetch_sub(1);
    }
}

void Server::HandleConnection(Socket client, const std::string& peer) {
    RequestParser parser(config_.limits);
    std::string buffer;

    while (impl_->running.load()) {
        // One parser reused across requests on a keep-alive connection. Reset
        // deliberately preserves any buffered bytes, which is what makes
        // pipelining work — the next request may already be in the buffer.
        bool eof = false;
        if (!parser.complete()) {
            buffer.clear();
            Status s = client.Read(&buffer, 64 * 1024, config_.client_read_timeout, &eof);
            if (!s.ok() || (eof && buffer.empty())) return;
            size_t consumed = 0;
            s = parser.Feed(buffer, &consumed);
            if (!s.ok()) {
                // A malformed request is answered and the connection closed.
                // Continuing on a connection whose framing we have lost
                // confidence in is how a smuggled request gets through.
                Response r = Response::Simple(s.http_status(), "Bad Request", s.ToString() + "\n");
                r.keep_alive = false;
                client.WriteAll(r.Serialize(), config_.client_write_timeout);
                return;
            }
            if (!parser.complete()) continue;
        }

        const Request& request = parser.request();
        uint32_t queue_depth = 0;
        {
            std::lock_guard<std::mutex> lock(impl_->mu);
            queue_depth = static_cast<uint32_t>(impl_->queue.size());
        }

        double roll;
        {
            std::lock_guard<std::mutex> lock(impl_->rng_mu);
            roll = std::uniform_real_distribution<double>(0.0, 1.0)(impl_->rng);
        }

        Router::Outcome outcome;
        {
            std::lock_guard<std::mutex> lock(impl_->router_mu);
            router_->SetInFlight(impl_->in_flight.load(), queue_depth);
            outcome = router_->Route(request, peer, Clock::now(), [roll] { return roll; });
        }

        bool keep_alive = request.keep_alive && outcome.response.keep_alive;
        outcome.response.keep_alive = keep_alive;
        // Surface which endpoint served the request. Invaluable when one host
        // in a cluster is misbehaving and the symptom is intermittent.
        if (!outcome.endpoint.empty()) {
            outcome.response.headers.push_back({"x-relay-upstream", outcome.endpoint});
        }
        outcome.response.headers.push_back(
            {"x-relay-attempts", std::to_string(outcome.attempts)});

        Status w = client.WriteAll(outcome.response.Serialize(), config_.client_write_timeout);
        if (!w.ok() || !keep_alive) return;

        parser.Reset();
    }
}

}  // namespace relay
