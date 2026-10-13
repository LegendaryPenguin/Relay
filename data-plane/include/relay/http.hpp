#pragma once

// HTTP/1.1 request and response parsing.
//
// Written rather than imported, which for a parser is a defensible choice only
// if the security-relevant cases are handled deliberately. The ones that matter
// for a proxy, as opposed to a client or a server:
//
//   * **Request smuggling.** A proxy that disagrees with its upstream about
//     where one request ends and the next begins lets an attacker inject a
//     request the upstream attributes to someone else. Both
//     Content-Length-with-Transfer-Encoding and duplicate-Content-Length are
//     rejected outright rather than resolved by precedence, because any
//     precedence rule is a disagreement waiting to happen.
//
//   * **Header injection.** A header value containing CR or LF would let a
//     client write its own headers into the upstream request. Rejected at
//     parse time.
//
//   * **Unbounded growth.** Every limit is explicit and enforced during
//     parsing, not after, so a hostile request cannot make the proxy allocate
//     before it is rejected.
//
// Incremental by design: Feed() accepts arbitrary byte boundaries, because a
// request arrives in whatever chunks the network decides and a parser that
// needs whole requests needs an unbounded buffer first.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "relay/status.hpp"

namespace relay {

// Limits, all enforced during parsing.
struct HttpLimits {
    size_t max_request_line = 8 * 1024;
    size_t max_header_count = 100;
    size_t max_header_bytes = 32 * 1024;
    size_t max_body_bytes = 8 * 1024 * 1024;
};

struct Header {
    std::string name;   // Stored lowercase: HTTP header names are case-insensitive.
    std::string value;
};

struct Request {
    std::string method;
    std::string target;
    int minor_version = 1;  // The 1 in HTTP/1.1.
    std::vector<Header> headers;
    std::string body;

    // Whether the connection may be reused after this request.
    //
    // HTTP/1.1 defaults to keep-alive and 1.0 to close, and `Connection:` can
    // override either. Getting this wrong in the permissive direction means
    // the proxy holds a connection the client has abandoned; in the strict
    // direction it discards a pooled connection per request and the pool
    // becomes decoration.
    bool keep_alive = true;

    const std::string* Find(std::string_view lowercase_name) const;
    bool Has(std::string_view lowercase_name) const { return Find(lowercase_name) != nullptr; }

    // Whether this method is safe to retry without a correctness argument.
    //
    // Used by the retry logic, and deliberately conservative: POST and PATCH
    // are never retried on a timeout, because a timeout cannot distinguish
    // "never arrived" from "arrived and the response was lost".
    bool idempotent() const;

    std::string Serialize() const;
};

struct Response {
    int status = 200;
    std::string reason;
    std::vector<Header> headers;
    std::string body;
    bool keep_alive = true;

    const std::string* Find(std::string_view lowercase_name) const;
    std::string Serialize() const;

    static Response Simple(int status, std::string_view reason, std::string body);
};

// Incremental request parser.
//
// One instance per connection, reused across requests on a keep-alive
// connection via Reset(). Holding the limits per-parser rather than globally
// is what lets a test exercise a 100-byte limit without a 100-byte global.
class RequestParser {
  public:
    explicit RequestParser(HttpLimits limits = {}) : limits_(limits) {}

    enum class State {
        kRequestLine,
        kHeaders,
        kBody,
        kChunkSize,
        kChunkData,
        kChunkTrailer,
        kComplete,
        kError,
    };

    // Feeds bytes. Returns the number consumed, which may be less than
    // supplied when a request completes mid-buffer — the remainder is the next
    // pipelined request and belongs to the caller.
    //
    // Returning the consumed count rather than swallowing the buffer is what
    // makes pipelining work at all, and it is the part a naive parser gets
    // wrong by assuming one read is one request.
    Status Feed(std::string_view data, size_t* consumed);

    bool complete() const { return state_ == State::kComplete; }
    State state() const { return state_; }
    const Request& request() const { return request_; }
    Request& mutable_request() { return request_; }

    void Reset();

  private:
    Status ParseRequestLine(std::string_view line);
    Status ParseHeaderLine(std::string_view line);
    Status FinishHeaders();

    HttpLimits limits_;
    State state_ = State::kRequestLine;
    Request request_;

    std::string pending_;        // Bytes of an incomplete line.
    size_t header_bytes_ = 0;
    size_t body_remaining_ = 0;
    size_t chunk_remaining_ = 0;
    bool chunked_ = false;
};

// Incremental response parser, for reading what an upstream sends back.
class ResponseParser {
  public:
    explicit ResponseParser(HttpLimits limits = {}) : limits_(limits) {}

    Status Feed(std::string_view data, size_t* consumed);
    bool complete() const { return state_ == State::kComplete; }
    const Response& response() const { return response_; }
    void Reset();

    // Signals that the upstream closed the connection.
    //
    // Required for an HTTP/1.0-style response with neither Content-Length nor
    // Transfer-Encoding, whose body is terminated by the close itself. Without
    // it such a response sits unfinished forever and the proxy reports a
    // timeout on a response it has fully received. Returns whether the
    // response is now complete — a close mid-headers is not.
    bool MarkEof();

  private:
    enum class State {
        kStatusLine,
        kHeaders,
        kBody,
        kChunkSize,
        kChunkData,
        kChunkTrailer,
        kComplete,
        kError,
    };

    Status ParseStatusLine(std::string_view line);
    Status ParseHeaderLine(std::string_view line);
    Status FinishHeaders();

    HttpLimits limits_;
    State state_ = State::kStatusLine;
    Response response_;
    std::string pending_;
    size_t header_bytes_ = 0;
    size_t body_remaining_ = 0;
    size_t chunk_remaining_ = 0;
    bool chunked_ = false;
    bool body_until_close_ = false;
};

// Lowercases ASCII in place. Header names only; values keep their case.
std::string ToLowerAscii(std::string_view s);

// Whether a header value is safe to forward.
//
// CR and LF in a value would let a client write its own headers into the
// upstream request, which is header injection. NUL is rejected too, since a
// value containing one truncates differently in C and C++ string handling and
// the difference is exactly the kind of parser disagreement that smuggling
// exploits.
bool HeaderValueIsSafe(std::string_view value);

}  // namespace relay
