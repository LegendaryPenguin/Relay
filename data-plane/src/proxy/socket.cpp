#include "relay/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace relay {

namespace {

std::string Errno(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

int PollOne(int fd, short events, Millis timeout) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    int timeout_ms = static_cast<int>(timeout.count());
    while (true) {
        int rc = ::poll(&pfd, 1, timeout_ms);
        // EINTR is not a failure. Returning it as one makes every blocking
        // operation in the proxy intermittently fail under any signal,
        // including the one used for shutdown.
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return rc;
        return pfd.revents;
    }
}

}  // namespace

Socket::~Socket() { Close(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

int Socket::Release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
}

void Socket::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Status SetNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return Status::Internal(Errno("fcntl F_GETFL"));
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Status::Internal(Errno("fcntl F_SETFL"));
    }
    return Status::Ok();
}

Status SetNoDelay(int fd) {
    int on = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
        return Status::Internal(Errno("setsockopt TCP_NODELAY"));
    }
    return Status::Ok();
}

Status Socket::Read(std::string* out, size_t max, Millis timeout, bool* eof) {
    *eof = false;
    if (!valid()) return Status::Internal("read on a closed socket");

    int revents = PollOne(fd_, POLLIN, timeout);
    if (revents == 0) return Status::Timeout("read timed out");
    if (revents < 0) return Status::Internal(Errno("poll"));

    char buf[16 * 1024];
    size_t want = max < sizeof(buf) ? max : sizeof(buf);
    while (true) {
        ssize_t n = ::read(fd_, buf, want);
        if (n > 0) {
            out->append(buf, static_cast<size_t>(n));
            return Status::Ok();
        }
        if (n == 0) {
            *eof = true;
            return Status::Ok();
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Poll said readable and the read says otherwise. Happens with a
            // spurious wakeup; treated as a timeout so the caller's retry or
            // deadline logic handles it rather than seeing a hard error.
            return Status::Timeout("read would block after poll indicated readable");
        }
        // ECONNRESET on read is an upstream failure, not an internal one.
        // Getting this wrong means a reset connection shows up as a 500 and
        // gets blamed on the proxy.
        return Status::UpstreamFailure(Errno("read"));
    }
}

Status Socket::WriteAll(std::string_view data, Millis timeout) {
    if (!valid()) return Status::Internal("write on a closed socket");
    size_t written = 0;
    while (written < data.size()) {
        int revents = PollOne(fd_, POLLOUT, timeout);
        if (revents == 0) return Status::Timeout("write timed out");
        if (revents < 0) return Status::Internal(Errno("poll"));

        ssize_t n = ::write(fd_, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        // EPIPE means the peer went away mid-write.
        return Status::UpstreamFailure(Errno("write"));
    }
    return Status::Ok();
}

bool Socket::PeerClosed() const {
    if (!valid()) return true;
    // Zero-timeout poll. A readable idle connection almost always means the
    // peer sent FIN, since a pooled connection should have nothing pending —
    // the exception is an upstream that sends unsolicited data, which would be
    // a protocol violation we would want to discard the connection over
    // anyway.
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    int rc = ::poll(&pfd, 1, 0);
    if (rc <= 0) return false;
    if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) return true;
    if ((pfd.revents & POLLIN) != 0) {
        // Distinguish FIN from pending data without consuming it.
        char probe;
        ssize_t n = ::recv(fd_, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) return true;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return true;
        // Unsolicited data on an idle pooled connection: treat as unusable.
        return n > 0;
    }
    return false;
}

Status Connect(const std::string& host, uint16_t port, Millis timeout, Socket* out) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        // Resolution failure is an upstream failure, so it charges the circuit
        // breaker. A cluster whose DNS has gone is as unusable as one whose
        // hosts have, and the breaker should open for it.
        return Status::UpstreamFailure(std::string("getaddrinfo ") + host + ": " +
                                       ::gai_strerror(rc));
    }

    Status last = Status::UpstreamFailure("no addresses for " + host);
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last = Status::Internal(Errno("socket"));
            continue;
        }
        Socket sock(fd);
        Status s = SetNonBlocking(fd);
        if (!s.ok()) {
            last = s;
            continue;
        }
        SetNoDelay(fd);

        int cr = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (cr == 0) {
            *out = std::move(sock);
            ::freeaddrinfo(result);
            return Status::Ok();
        }
        if (errno != EINPROGRESS) {
            last = Status::UpstreamFailure(Errno("connect"));
            continue;
        }

        int revents = PollOne(fd, POLLOUT, timeout);
        if (revents == 0) {
            // The whole reason for the non-blocking dance: a blocking connect
            // would wait out the kernel's SYN retry schedule, tens of
            // seconds, and a proxy that slow to notice a dead upstream has no
            // useful timeout.
            last = Status::Timeout("connect timed out to " + host + ":" + port_str);
            continue;
        }
        if (revents < 0) {
            last = Status::Internal(Errno("poll"));
            continue;
        }

        // POLLOUT alone does not mean success; the error must be collected.
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            last = Status::Internal(Errno("getsockopt SO_ERROR"));
            continue;
        }
        if (err != 0) {
            errno = err;
            last = Status::UpstreamFailure(Errno("connect"));
            continue;
        }
        *out = std::move(sock);
        ::freeaddrinfo(result);
        return Status::Ok();
    }

    ::freeaddrinfo(result);
    return last;
}

Status Listen(const std::string& host, uint16_t port, int backlog, Socket* out) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_str.c_str(), &hints,
                           &result);
    if (rc != 0) {
        return Status::Internal(std::string("getaddrinfo: ") + ::gai_strerror(rc));
    }

    int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(result);
        return Status::Internal(Errno("socket"));
    }
    Socket sock(fd);

    int on = 1;
    // SO_REUSEADDR so a restart does not have to wait out TIME_WAIT on the
    // listening port, which otherwise makes a redeploy fail for a minute.
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (::bind(fd, result->ai_addr, result->ai_addrlen) < 0) {
        ::freeaddrinfo(result);
        return Status::Internal(Errno("bind"));
    }
    ::freeaddrinfo(result);

    if (::listen(fd, backlog) < 0) return Status::Internal(Errno("listen"));
    *out = std::move(sock);
    return Status::Ok();
}

Status Accept(const Socket& listener, Millis timeout, Socket* out, std::string* peer) {
    int revents = PollOne(listener.fd(), POLLIN, timeout);
    if (revents == 0) return Status::Timeout("accept timed out");
    if (revents < 0) return Status::Internal(Errno("poll"));

    struct sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    int fd = ::accept(listener.fd(), reinterpret_cast<struct sockaddr*>(&addr), &len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return Status::Timeout("accept would block");
        }
        return Status::Internal(Errno("accept"));
    }
    SetNoDelay(fd);
    *out = Socket(fd);

    if (peer != nullptr) {
        char host[INET6_ADDRSTRLEN] = {0};
        if (addr.ss_family == AF_INET) {
            auto* in4 = reinterpret_cast<struct sockaddr_in*>(&addr);
            ::inet_ntop(AF_INET, &in4->sin_addr, host, sizeof(host));
        } else if (addr.ss_family == AF_INET6) {
            auto* in6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
            ::inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        }
        *peer = host;
    }
    return Status::Ok();
}

Status LocalPort(const Socket& sock, uint16_t* port) {
    struct sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(sock.fd(), reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        return Status::Internal(Errno("getsockname"));
    }
    if (addr.ss_family == AF_INET) {
        *port = ntohs(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        *port = ntohs(reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port);
    } else {
        return Status::Internal("unknown address family");
    }
    return Status::Ok();
}

}  // namespace relay
