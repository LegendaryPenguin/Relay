#pragma once

// Upstream connection pooling.
//
// Opening a TCP connection per request costs a round trip before any useful
// work happens, which on a 1ms-RTT network doubles the latency of a 1ms
// upstream. Pooling removes that, and introduces the one failure mode that
// makes naive pools worse than no pool: **handing out a connection the
// upstream has already closed.**
//
// An idle keep-alive connection can be closed by the upstream at any time —
// its own idle timeout, a deploy, a scale-down. The proxy does not find out
// until it writes a request and the write succeeds (into the socket buffer)
// and the read then returns EOF. The request has been consumed, the proxy has
// no response, and the natural reaction is to report 502. That is a spurious
// 502 caused entirely by the pool, and it is the single most common pooling
// bug.
//
// Three defences here, in order of how much they help:
//
//   1. **Check for a peer close before handing out.** A zero-timeout poll plus
//      MSG_PEEK distinguishes FIN from pending data. Cheap and catches most of
//      it.
//   2. **Bound idle lifetime below the upstream's.** A connection older than
//      `max_idle` is discarded unused. This is the defence that actually
//      works, because the race in (1) is unavoidable — the FIN can arrive
//      between the check and the write.
//   3. **Treat a failure on a *reused* connection as retryable even for
//      non-idempotent requests**, provided nothing was read back. If the
//      write failed on a pooled connection and no response byte arrived, the
//      request cannot have been processed, so retrying a POST is safe. This is
//      the subtle one and it is why `Lease::reused` exists.

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "relay/metrics.hpp"
#include "relay/policy.hpp"
#include "relay/socket.hpp"

namespace relay {

class ConnectionPool {
  public:
    struct Config {
        // Idle connections kept per endpoint.
        uint32_t max_idle_per_endpoint = 32;
        // Total connections, idle plus active, per endpoint.
        uint32_t max_per_endpoint = 256;
        // How long an idle connection may be kept.
        //
        // Must be shorter than the upstream's own idle timeout, and there is
        // no way for the proxy to discover that value — so this is a
        // configuration the operator has to get right, and the README says so.
        Millis max_idle{30000};
        Millis connect_timeout{1000};
    };

    ConnectionPool() : ConnectionPool(Config{}) {}
    explicit ConnectionPool(Config config) : config_(config) {}

    // A borrowed connection.
    //
    // Move-only, and the destructor does *not* return it to the pool. Return
    // is explicit via Release/Discard because the decision depends on how the
    // request went — a connection that saw a protocol error must not go back.
    // An implicit return in the destructor would put broken connections back
    // on every error path.
    class Lease {
      public:
        Lease() = default;
        Lease(Socket sock, std::string endpoint, bool reused)
            : sock_(std::move(sock)), endpoint_(std::move(endpoint)), reused_(reused) {}

        Lease(Lease&&) = default;
        Lease& operator=(Lease&&) = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Socket& socket() { return sock_; }
        const std::string& endpoint() const { return endpoint_; }

        // Whether this connection came from the pool rather than being freshly
        // dialled.
        //
        // Load-bearing for retry safety: a failure on a reused connection
        // where no response byte was read cannot have been processed by the
        // upstream, so it is safe to retry even a POST.
        bool reused() const { return reused_; }

        bool valid() const { return sock_.valid(); }

      private:
        Socket sock_;
        std::string endpoint_;
        bool reused_ = false;
    };

    // Borrows or opens a connection to host:port.
    Status Acquire(const std::string& host, uint16_t port, TimePoint now, Lease* out,
                   ClusterStats* stats);

    // Returns a healthy connection to the pool.
    void Release(Lease lease, TimePoint now, ClusterStats* stats);

    // Closes a connection rather than pooling it.
    void Discard(Lease lease, ClusterStats* stats);

    // Drops idle connections older than max_idle, and any whose peer has
    // closed. Called from the proxy's maintenance tick.
    size_t Prune(TimePoint now, ClusterStats* stats);

    size_t idle_count() const;
    size_t idle_count(const std::string& endpoint) const;
    uint32_t active_count(const std::string& endpoint) const;

  private:
    struct Idle {
        Socket sock;
        TimePoint pooled_at;
    };
    struct PerEndpoint {
        std::deque<Idle> idle;
        uint32_t active = 0;
    };

    Config config_;
    // Keyed by "host:port". A string key per acquire is a small cost against
    // a connect, and keying by a parsed struct would need a hash and an
    // equality that are both more code than the string.
    std::unordered_map<std::string, PerEndpoint> endpoints_;
};

}  // namespace relay
