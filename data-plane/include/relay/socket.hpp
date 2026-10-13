#pragma once

// Sockets, and a thin readiness-poll abstraction.
//
// **An honest statement about the architecture up front**, because the word
// "high-performance" invites an assumption this does not meet: the proxy is a
// bounded thread pool over blocking sockets with poll-based timeouts, not a
// single-threaded event loop with a kqueue/epoll reactor. Each worker thread
// handles one client connection at a time, start to finish.
//
// Why: an event-loop proxy requires every stage — parse, balance, connect,
// write, read, respond — to be resumable, which means an explicit state
// machine per request and a continuation for every I/O point. That is the
// right design for C10k and it is several times the code. The thread-pool
// model handles hundreds of concurrent connections comfortably, which is the
// range the benchmarks cover, and the cost is stated in bench/results.md with
// the measured ceiling rather than hidden.
//
// What that costs concretely: memory per connection is a thread stack rather
// than a few hundred bytes, and concurrency is bounded by the pool size rather
// than by file descriptors. The load shedder exists partly because of this —
// with a bounded worker pool, excess concurrency becomes queue depth, and
// queue depth is the signal it sheds on.

#include <cstdint>
#include <string>
#include <string_view>

#include "relay/policy.hpp"
#include "relay/status.hpp"

namespace relay {

// An owned file descriptor.
//
// A class rather than a raw int because every error path in the proxy is a
// place a descriptor leaks, and a leaked descriptor presents as the proxy
// refusing new connections after some hours of uptime — which is a miserable
// thing to debug from the symptom. Move-only, closes on destruction.
class Socket {
  public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    // Releases ownership without closing, for handing a descriptor to the
    // connection pool.
    int Release();
    void Close();

    // Reads up to `max` bytes, appending to `out`.
    //
    // `timeout` applies to waiting for readability, not to the whole read. A
    // read that returns some bytes returns immediately with them rather than
    // waiting for more, because the parser is incremental and a partial read
    // may complete a request.
    Status Read(std::string* out, size_t max, Millis timeout, bool* eof);

    // Writes all of `data`, looping over partial writes.
    Status WriteAll(std::string_view data, Millis timeout);

    // Whether the peer has closed. Used by the pool to avoid handing out a
    // connection the upstream has already dropped — the single most common
    // cause of a spurious 502 in a pooled proxy.
    bool PeerClosed() const;

  private:
    int fd_ = -1;
};

// Sets O_NONBLOCK. Used for the connect path, where a blocking connect cannot
// be given a timeout shorter than the kernel's.
Status SetNonBlocking(int fd);

// Disables Nagle.
//
// On a proxy this matters more than usual: a request is written as a header
// block followed by a body, and with Nagle enabled the header block waits for
// an ACK before the body goes out, adding a round trip to every request that
// has a body.
Status SetNoDelay(int fd);

// Connects to host:port with a timeout.
//
// Non-blocking connect plus poll, because a blocking connect's timeout is the
// kernel's SYN retry schedule — typically tens of seconds — and a proxy that
// takes 20 seconds to notice a dead upstream has no useful timeout at all.
Status Connect(const std::string& host, uint16_t port, Millis timeout, Socket* out);

// Binds and listens on host:port.
Status Listen(const std::string& host, uint16_t port, int backlog, Socket* out);

// Accepts a connection, waiting up to `timeout`.
//
// Returns kTimeout when nothing arrived, which the accept loop uses as its
// chance to check for shutdown. A blocking accept would make shutdown require
// either a signal or a self-pipe.
Status Accept(const Socket& listener, Millis timeout, Socket* out, std::string* peer);

// Resolved address of a listening socket, for reporting the port when 0 was
// requested — which is how the tests get a free port without racing.
Status LocalPort(const Socket& sock, uint16_t* port);

}  // namespace relay
