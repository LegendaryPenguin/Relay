#pragma once

// Status, and the error taxonomy the proxy actually needs.
//
// The categories are not generic. Each one exists because the proxy has to do
// something *different* in response, and a status type whose values do not map
// onto distinct responses is just a string with extra steps:
//
//   kOk               forward it.
//   kInvalidRequest   the client sent something malformed. 400, do not retry,
//                     do not blame the upstream.
//   kUpstreamFailure  the upstream failed in a way a retry might fix.
//                     Charges the retry budget and the circuit breaker.
//   kTimeout          we gave up waiting. Retryable, but counted separately
//                     because a timeout may mean the request *did* land and
//                     retrying a non-idempotent request is a correctness
//                     problem rather than a latency one.
//   kOverloaded       we refused the work deliberately. 503, and crucially
//                     NOT charged to the circuit breaker — shedding load is
//                     the proxy protecting the upstream, and letting it trip
//                     the breaker would turn self-protection into an outage.
//   kNoHealthyUpstream  nothing to send it to. 503, no retry, since a retry
//                     would find the same empty set.
//   kInternal         our bug.
//
// That kOverloaded distinction is the one I got wrong first and it is in the
// README's list of bugs.

#include <ostream>
#include <string>
#include <utility>

namespace relay {

enum class Code {
    kOk = 0,
    kInvalidRequest,
    kUpstreamFailure,
    kTimeout,
    kOverloaded,
    kNoHealthyUpstream,
    kInternal,
};

const char* CodeName(Code code);

// Printable so assertions and log lines name the code rather than its integer
// value. Found by ADL on relay::Code.
inline std::ostream& operator<<(std::ostream& os, Code code) {
    return os << CodeName(code);
}

class Status {
  public:
    Status() = default;

    static Status Ok() { return Status(); }
    static Status InvalidRequest(std::string msg) {
        return Status(Code::kInvalidRequest, std::move(msg));
    }
    static Status UpstreamFailure(std::string msg) {
        return Status(Code::kUpstreamFailure, std::move(msg));
    }
    static Status Timeout(std::string msg) { return Status(Code::kTimeout, std::move(msg)); }
    static Status Overloaded(std::string msg) { return Status(Code::kOverloaded, std::move(msg)); }
    static Status NoHealthyUpstream(std::string msg) {
        return Status(Code::kNoHealthyUpstream, std::move(msg));
    }
    static Status Internal(std::string msg) { return Status(Code::kInternal, std::move(msg)); }

    bool ok() const { return code_ == Code::kOk; }
    Code code() const { return code_; }
    const std::string& message() const { return message_; }

    // Whether a retry could plausibly succeed.
    //
    // Deliberately excludes kOverloaded. If the proxy is shedding load,
    // retrying through the same proxy adds load to a component that has
    // already said it has none to spare — the retry storm that turns a
    // brownout into an outage.
    bool retryable() const {
        return code_ == Code::kUpstreamFailure || code_ == Code::kTimeout;
    }

    // Whether this failure should count against a circuit breaker.
    //
    // A timeout counts and an overload does not. The breaker is a statement
    // about the *upstream's* health, and our own load shedding says nothing
    // about the upstream.
    bool charges_breaker() const {
        return code_ == Code::kUpstreamFailure || code_ == Code::kTimeout;
    }

    // The HTTP status a client should see.
    int http_status() const;

    std::string ToString() const;

  private:
    Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

    Code code_ = Code::kOk;
    std::string message_;
};

}  // namespace relay
