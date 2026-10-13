#pragma once

// The data plane's configuration client: polls the control plane and swaps
// the router's route table.
//
// This is the piece that makes the two-process architecture real rather than
// nominal. Without it the control plane resolves and health-checks endpoints
// that nothing consumes.
//
// Three decisions worth stating:
//
//   * **Pull, not push.** The proxy polls; the control plane holds no
//     connection to it and tracks no delivery state. Staleness is bounded by
//     the poll interval and self-corrects, where a push scheme has to retry
//     and reconcile — and the most common way a control plane goes wrong is
//     believing it delivered a config it did not.
//
//   * **Conditional requests.** The ETag carries the snapshot version, so an
//     unchanged poll transfers a few bytes. With a short interval and a large
//     endpoint list, the difference is a trivial poll against a steady stream
//     of identical payloads.
//
//   * **A bad config is refused, and the previous one keeps serving.** This
//     is the one place the data plane prefers stale-but-valid over
//     fresh-but-broken. The alternative is a proxy that stops routing because
//     its control plane shipped a typo.
//
// The applied table is built fresh each time and published by swapping a
// shared_ptr, so a request in flight finishes against the table it started
// on. Router::SetTable deliberately keeps per-endpoint breaker state across
// the swap — otherwise a control plane polling every second would reset every
// circuit breaker every second and they would never open.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "relay/json.hpp"
#include "relay/policy.hpp"
#include "relay/proxy.hpp"
#include "relay/status.hpp"

namespace relay {

// Builds a RouteTable from a parsed control-plane snapshot.
//
// Separated from the polling so it can be tested on a literal JSON string with
// no server involved — which is how the mapping tests work.
Status BuildRouteTableFromJson(const JsonValue& snapshot, RouteTablePtr* out,
                               std::string* warning);

// Fetches one URL with an optional If-None-Match. Exposed because the config
// client's HTTP needs are tiny and a general client would be more code than
// this.
struct HttpFetchResult {
    int status = 0;
    std::string body;
    std::string etag;
};
Status HttpGet(const std::string& host, uint16_t port, const std::string& path,
               const std::string& if_none_match, Millis timeout, HttpFetchResult* out);

class ConfigClient {
  public:
    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 9901;
        std::string path = "/config";
        Millis poll_interval{1000};
        Millis timeout{2000};
    };

    struct Stats {
        uint64_t polls = 0;
        uint64_t not_modified = 0;
        uint64_t applied = 0;
        uint64_t rejected = 0;
        uint64_t errors = 0;
        uint64_t version = 0;
        std::string last_error;
    };

    ConfigClient(Config config, Router* router);
    ~ConfigClient();

    // Fetches once and applies if changed. Returns the fetch's status;
    // a rejected config is reported as an error but leaves the router alone.
    Status PollOnce();

    void Start();
    void Stop();

    Stats stats() const;

  private:
    void Loop();

    Config config_;
    Router* router_;
    std::string etag_;

    mutable std::mutex mu_;
    Stats stats_;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace relay
