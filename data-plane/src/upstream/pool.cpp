#include "relay/pool.hpp"

namespace relay {

Status ConnectionPool::Acquire(const std::string& host, uint16_t port, TimePoint now,
                               Lease* out, ClusterStats* stats) {
    std::string key = host + ":" + std::to_string(port);
    PerEndpoint& ep = endpoints_[key];

    // Take from the back: most recently pooled, so least likely to have been
    // closed by the upstream's idle timeout. LIFO rather than FIFO is a
    // deliberate inversion of the usual queue instinct — FIFO hands out the
    // oldest connection, which is exactly the one most likely to be dead.
    while (!ep.idle.empty()) {
        Idle entry = std::move(ep.idle.back());
        ep.idle.pop_back();

        if (now - entry.pooled_at > config_.max_idle) {
            // Too old to trust. This is the defence that actually works
            // against the spurious-502 race, because checking for a close
            // cannot be made reliable — the FIN can arrive between the check
            // and the write.
            if (stats != nullptr) ++stats->connections_closed;
            continue;
        }
        if (entry.sock.PeerClosed()) {
            if (stats != nullptr) ++stats->connections_closed;
            continue;
        }

        ++ep.active;
        if (stats != nullptr) ++stats->pool_hits;
        *out = Lease(std::move(entry.sock), key, /*reused=*/true);
        return Status::Ok();
    }

    if (ep.active >= config_.max_per_endpoint) {
        // Refusing is correct and the status matters: this is the proxy's own
        // limit, not an upstream failure, so it must not charge the circuit
        // breaker. Reporting it as an upstream failure would let the proxy's
        // connection cap trip a breaker against a perfectly healthy upstream.
        return Status::Overloaded("connection limit reached for " + key);
    }

    Socket sock;
    Status s = Connect(host, port, config_.connect_timeout, &sock);
    if (!s.ok()) return s;

    ++ep.active;
    if (stats != nullptr) {
        ++stats->pool_misses;
        ++stats->connections_opened;
    }
    *out = Lease(std::move(sock), key, /*reused=*/false);
    return Status::Ok();
}

void ConnectionPool::Release(Lease lease, TimePoint now, ClusterStats* stats) {
    if (!lease.valid()) return;
    auto it = endpoints_.find(lease.endpoint());
    if (it == endpoints_.end()) return;
    PerEndpoint& ep = it->second;
    if (ep.active > 0) --ep.active;

    if (ep.idle.size() >= config_.max_idle_per_endpoint) {
        // Pool full. Dropping the newly returned connection rather than
        // evicting an older pooled one keeps the pool's contents the
        // freshest available set.
        if (stats != nullptr) ++stats->connections_closed;
        return;
    }
    ep.idle.push_back(Idle{std::move(lease.socket()), now});
}

void ConnectionPool::Discard(Lease lease, ClusterStats* stats) {
    auto it = endpoints_.find(lease.endpoint());
    if (it != endpoints_.end() && it->second.active > 0) --it->second.active;
    if (lease.valid() && stats != nullptr) ++stats->connections_closed;
    // The Lease destructor closes the socket.
}

size_t ConnectionPool::Prune(TimePoint now, ClusterStats* stats) {
    size_t removed = 0;
    for (auto& [key, ep] : endpoints_) {
        auto it = ep.idle.begin();
        while (it != ep.idle.end()) {
            if (now - it->pooled_at > config_.max_idle || it->sock.PeerClosed()) {
                it = ep.idle.erase(it);
                ++removed;
                if (stats != nullptr) ++stats->connections_closed;
            } else {
                ++it;
            }
        }
    }
    return removed;
}

size_t ConnectionPool::idle_count() const {
    size_t total = 0;
    for (const auto& [key, ep] : endpoints_) total += ep.idle.size();
    return total;
}

size_t ConnectionPool::idle_count(const std::string& endpoint) const {
    auto it = endpoints_.find(endpoint);
    return it == endpoints_.end() ? 0 : it->second.idle.size();
}

uint32_t ConnectionPool::active_count(const std::string& endpoint) const {
    auto it = endpoints_.find(endpoint);
    return it == endpoints_.end() ? 0 : it->second.active;
}

}  // namespace relay
