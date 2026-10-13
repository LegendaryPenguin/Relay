#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "relay/pool.hpp"
#include "relay/socket.hpp"
#include "testing.hpp"

using namespace relay;

// Pool tests need a real listener, because the behaviour under test is about
// socket state — whether a peer has closed, whether a descriptor is still
// usable. A fake socket would test the bookkeeping and miss the point.

namespace {

// A trivial accept-and-hold server. Holds connections open so the pool can
// reuse them, and can be told to close them to simulate an upstream's idle
// timeout.
class TinyServer {
  public:
    Status Start() {
        Status s = Listen("127.0.0.1", 0, 64, &listener_);
        if (!s.ok()) return s;
        s = LocalPort(listener_, &port_);
        if (!s.ok()) return s;
        running_ = true;
        thread_ = std::thread([this] { Loop(); });
        return Status::Ok();
    }

    void Stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        listener_.Close();
        CloseAll();
    }

    // Closes every accepted connection, as an upstream's idle timeout would.
    void CloseAll() {
        std::lock_guard<std::mutex> lock(mu_);
        accepted_.clear();
    }

    uint16_t port() const { return port_; }
    int accept_count() const { return accepts_.load(); }

    // Waits for the accept loop to catch up.
    //
    // Connecting and the server accepting are separate events, and the loop
    // polls with a 50ms timeout — so reading accept_count() straight after a
    // Connect reads it before the server has run. Three tests here were
    // flaky-by-construction until this existed. Returns false on timeout so a
    // genuine failure to accept is still a test failure rather than a hang.
    bool WaitForAccepts(int n, Millis timeout = Millis(2000)) {
        auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (accepts_.load() >= n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return accepts_.load() >= n;
    }

    ~TinyServer() { Stop(); }

  private:
    void Loop() {
        while (running_) {
            Socket client;
            std::string peer;
            if (!Accept(listener_, Millis(50), &client, &peer).ok()) continue;
            accepts_.fetch_add(1);
            std::lock_guard<std::mutex> lock(mu_);
            accepted_.push_back(std::move(client));
        }
    }

    Socket listener_;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> accepts_{0};
    std::thread thread_;
    std::mutex mu_;
    std::vector<Socket> accepted_;
};

}  // namespace

RELAY_TEST(acquire_opens_a_connection) {
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool pool;
    ClusterStats stats;
    ConnectionPool::Lease lease;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &lease, &stats));
    CHECK(lease.valid());
    CHECK(!lease.reused());
    CHECK_EQ(stats.pool_misses, uint64_t(1));
    CHECK_EQ(stats.connections_opened, uint64_t(1));
    server.Stop();
}

RELAY_TEST(release_then_acquire_reuses) {
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool pool;
    ClusterStats stats;
    ConnectionPool::Lease a;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &a, &stats));
    pool.Release(std::move(a), Clock::now(), &stats);
    CHECK_EQ(pool.idle_count(), size_t(1));

    ConnectionPool::Lease b;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &b, &stats));
    CHECK(b.reused());
    CHECK_EQ(stats.pool_hits, uint64_t(1));
    // Only one TCP connection was ever made.
    CHECK(server.WaitForAccepts(1));
    CHECK_EQ(server.accept_count(), 1);
    server.Stop();
}

RELAY_TEST(discard_does_not_pool) {
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool pool;
    ClusterStats stats;
    ConnectionPool::Lease a;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &a, &stats));
    pool.Discard(std::move(a), &stats);
    CHECK_EQ(pool.idle_count(), size_t(0));
    CHECK_EQ(stats.connections_closed, uint64_t(1));
    server.Stop();
}

RELAY_TEST(stale_connections_are_not_handed_out) {
    // The defence that actually works against the spurious-502 race, because
    // checking for a close cannot be made reliable — the FIN can arrive
    // between the check and the write.
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool::Config cfg;
    cfg.max_idle = Millis(10);
    ConnectionPool pool(cfg);
    ClusterStats stats;

    ConnectionPool::Lease a;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &a, &stats));
    CHECK(server.WaitForAccepts(1));
    TimePoint pooled_at = Clock::now();
    pool.Release(std::move(a), pooled_at, &stats);
    CHECK_EQ(pool.idle_count(), size_t(1));

    // Acquire well past the idle limit.
    ConnectionPool::Lease b;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), pooled_at + Millis(1000), &b, &stats));
    CHECK(!b.reused());  // The stale one was discarded and a new one dialled.
    CHECK(server.WaitForAccepts(2));
    CHECK_EQ(server.accept_count(), 2);
    server.Stop();
}

RELAY_TEST(closed_peer_is_detected_before_handing_out) {
    // The cheap first defence: a zero-timeout poll plus MSG_PEEK
    // distinguishes FIN from pending data.
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool pool;
    ClusterStats stats;
    ConnectionPool::Lease a;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &a, &stats));
    // The server must have accepted it before CloseAll can close it; without
    // this the connection sits in the listen backlog, never gets closed, and
    // the pool correctly reuses a live connection — making the test fail for
    // a reason that has nothing to do with close detection.
    CHECK(server.WaitForAccepts(1));
    pool.Release(std::move(a), Clock::now(), &stats);

    // The upstream closes it, as an idle timeout or a deploy would.
    server.CloseAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ConnectionPool::Lease b;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &b, &stats));
    CHECK(!b.reused());
    CHECK(b.valid());
    server.Stop();
}

RELAY_TEST(lifo_order_hands_out_the_freshest) {
    // FIFO would hand out the oldest connection, which is exactly the one
    // most likely to have been closed by the upstream's idle timeout.
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool pool;
    ClusterStats stats;
    std::vector<ConnectionPool::Lease> leases(3);
    for (auto& l : leases) {
        CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &l, &stats));
    }
    CHECK(server.WaitForAccepts(3));
    // Release in order, so index 2 is pooled last and is the freshest.
    TimePoint t = Clock::now();
    for (size_t i = 0; i < leases.size(); ++i) {
        pool.Release(std::move(leases[i]), t + Millis(static_cast<long>(i)), &stats);
    }
    CHECK_EQ(pool.idle_count(), size_t(3));

    ConnectionPool::Lease next;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), t + Millis(5), &next, &stats));
    CHECK(next.reused());
    CHECK_EQ(pool.idle_count(), size_t(2));
    server.Stop();
}

RELAY_TEST(idle_pool_is_bounded) {
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool::Config cfg;
    cfg.max_idle_per_endpoint = 2;
    ConnectionPool pool(cfg);
    ClusterStats stats;

    std::vector<ConnectionPool::Lease> leases(5);
    for (auto& l : leases) {
        CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &l, &stats));
    }
    for (auto& l : leases) pool.Release(std::move(l), Clock::now(), &stats);

    CHECK_EQ(pool.idle_count(), size_t(2));
    server.Stop();
}

RELAY_TEST(per_endpoint_connection_limit_reports_overloaded) {
    // The status matters: this is the proxy's own limit, not an upstream
    // failure, so reporting it as one would let the connection cap trip a
    // breaker against a perfectly healthy upstream.
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool::Config cfg;
    cfg.max_per_endpoint = 2;
    ConnectionPool pool(cfg);
    ClusterStats stats;

    ConnectionPool::Lease a, b, c;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &a, &stats));
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &b, &stats));
    Status s = pool.Acquire("127.0.0.1", server.port(), Clock::now(), &c, &stats);
    CHECK(!s.ok());
    CHECK_EQ(s.code(), Code::kOverloaded);
    CHECK(!s.charges_breaker());
    server.Stop();
}

RELAY_TEST(active_count_is_returned_on_release) {
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool pool;
    ClusterStats stats;
    std::string key = std::string("127.0.0.1:") + std::to_string(server.port());

    ConnectionPool::Lease a;
    CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &a, &stats));
    CHECK_EQ(pool.active_count(key), uint32_t(1));
    pool.Release(std::move(a), Clock::now(), &stats);
    CHECK_EQ(pool.active_count(key), uint32_t(0));
    server.Stop();
}

RELAY_TEST(active_count_is_returned_on_discard) {
    // A discard path that forgot to decrement would make the pool refuse new
    // connections after max_per_endpoint failures, which presents as an
    // upstream outage caused entirely by the proxy.
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool::Config cfg;
    cfg.max_per_endpoint = 3;
    ConnectionPool pool(cfg);
    ClusterStats stats;
    std::string key = std::string("127.0.0.1:") + std::to_string(server.port());

    for (int i = 0; i < 10; ++i) {
        ConnectionPool::Lease l;
        CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &l, &stats));
        pool.Discard(std::move(l), &stats);
    }
    CHECK_EQ(pool.active_count(key), uint32_t(0));
    server.Stop();
}

RELAY_TEST(prune_removes_stale_entries) {
    TinyServer server;
    CHECK_OK(server.Start());

    ConnectionPool::Config cfg;
    cfg.max_idle = Millis(10);
    ConnectionPool pool(cfg);
    ClusterStats stats;

    std::vector<ConnectionPool::Lease> leases(3);
    for (auto& l : leases) {
        CHECK_OK(pool.Acquire("127.0.0.1", server.port(), Clock::now(), &l, &stats));
    }
    TimePoint t = Clock::now();
    for (auto& l : leases) pool.Release(std::move(l), t, &stats);
    CHECK_EQ(pool.idle_count(), size_t(3));

    CHECK_EQ(pool.Prune(t + Millis(1000), &stats), size_t(3));
    CHECK_EQ(pool.idle_count(), size_t(0));
    server.Stop();
}

RELAY_TEST(connect_to_a_dead_port_fails_as_upstream_failure) {
    // Not an internal error: the proxy is fine, the upstream is not. Getting
    // this wrong means a refused connection shows up as a 500 and gets blamed
    // on the proxy.
    ConnectionPool pool;
    ClusterStats stats;
    ConnectionPool::Lease l;
    // Port 1 on loopback is reliably closed.
    Status s = pool.Acquire("127.0.0.1", 1, Clock::now(), &l, &stats);
    CHECK(!s.ok());
    CHECK(s.code() == Code::kUpstreamFailure || s.code() == Code::kTimeout);
    CHECK(s.charges_breaker());
}

RELAY_TEST(socket_move_does_not_double_close) {
    // A double close would eventually close an unrelated descriptor that had
    // been assigned the same number, which is a spectacularly confusing bug.
    TinyServer server;
    CHECK_OK(server.Start());
    Socket a;
    CHECK_OK(Connect("127.0.0.1", server.port(), Millis(1000), &a));
    int fd = a.fd();
    Socket b = std::move(a);
    CHECK(!a.valid());
    CHECK_EQ(b.fd(), fd);
    b.Close();
    b.Close();  // Idempotent.
    server.Stop();
}

RELAY_TEST_MAIN()
