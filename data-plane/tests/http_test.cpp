#include <string>
#include <vector>

#include "relay/http.hpp"
#include "testing.hpp"

using namespace relay;

namespace {

// Feeds a whole buffer and returns the parse status.
Status FeedAll(RequestParser& p, std::string_view data) {
    size_t total = 0;
    while (total < data.size()) {
        size_t consumed = 0;
        Status s = p.Feed(data.substr(total), &consumed);
        if (!s.ok()) return s;
        if (consumed == 0) break;  // Needs more input than we have.
        total += consumed;
        if (p.complete()) break;
    }
    return Status::Ok();
}

const char* kSimple =
    "GET /hello HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "User-Agent: test\r\n"
    "\r\n";

}  // namespace

RELAY_TEST(parses_a_simple_request) {
    RequestParser p;
    CHECK_OK(FeedAll(p, kSimple));
    CHECK(p.complete());
    CHECK_EQ(p.request().method, std::string("GET"));
    CHECK_EQ(p.request().target, std::string("/hello"));
    CHECK_EQ(p.request().minor_version, 1);
    CHECK_EQ(p.request().headers.size(), size_t(2));
    // Names are lowercased; values keep their case.
    CHECK(p.request().Find("host") != nullptr);
    CHECK_EQ(*p.request().Find("host"), std::string("example.com"));
    CHECK(p.request().Find("Host") == nullptr);
    CHECK(p.request().keep_alive);
}

RELAY_TEST(parses_at_every_byte_boundary) {
    // The property that matters for a network parser: a request arrives in
    // whatever chunks the network decides. A parser that works on whole
    // buffers and fails on a split at byte 23 passes every naive test.
    std::string req(kSimple);
    for (size_t split = 0; split <= req.size(); ++split) {
        RequestParser p;
        size_t consumed = 0;
        Status s1 = p.Feed(std::string_view(req).substr(0, split), &consumed);
        if (!s1.ok()) {
            CHECK(false);
            continue;
        }
        size_t consumed2 = 0;
        Status s2 = p.Feed(std::string_view(req).substr(split), &consumed2);
        if (!s2.ok()) {
            CHECK(false);
            continue;
        }
        if (!p.complete()) {
            std::fprintf(stderr, "        split at %zu did not complete\n", split);
            CHECK(false);
            continue;
        }
        CHECK_EQ(p.request().target, std::string("/hello"));
    }
}

RELAY_TEST(parses_one_byte_at_a_time) {
    RequestParser p;
    std::string req(kSimple);
    for (char c : req) {
        size_t consumed = 0;
        CHECK_OK(p.Feed(std::string_view(&c, 1), &consumed));
    }
    CHECK(p.complete());
    CHECK_EQ(p.request().target, std::string("/hello"));
}

RELAY_TEST(content_length_body) {
    RequestParser p;
    CHECK_OK(FeedAll(p,
                     "POST /submit HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Content-Length: 11\r\n"
                     "\r\n"
                     "hello world"));
    CHECK(p.complete());
    CHECK_EQ(p.request().body, std::string("hello world"));
}

RELAY_TEST(zero_length_body_completes) {
    RequestParser p;
    CHECK_OK(FeedAll(p, "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"));
    CHECK(p.complete());
    CHECK(p.request().body.empty());
}

RELAY_TEST(chunked_body) {
    RequestParser p;
    CHECK_OK(FeedAll(p,
                     "POST /submit HTTP/1.1\r\n"
                     "Host: x\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"
                     "5\r\nhello\r\n"
                     "6\r\n world\r\n"
                     "0\r\n\r\n"));
    CHECK(p.complete());
    CHECK_EQ(p.request().body, std::string("hello world"));
}

RELAY_TEST(chunked_body_split_across_feeds) {
    // Chunk framing split mid-size-line and mid-data is where a chunked
    // parser usually breaks.
    std::string req =
        "POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nabcd\r\n3\r\nefg\r\n0\r\n\r\n";
    for (size_t split = 1; split < req.size(); ++split) {
        RequestParser p;
        size_t c1 = 0, c2 = 0;
        CHECK_OK(p.Feed(std::string_view(req).substr(0, split), &c1));
        CHECK_OK(p.Feed(std::string_view(req).substr(split), &c2));
        if (!p.complete() || p.request().body != "abcdefg") {
            std::fprintf(stderr, "        split at %zu gave body %s\n", split,
                         p.request().body.c_str());
            CHECK(false);
        }
    }
}

RELAY_TEST(chunk_extensions_are_ignored_not_fatal) {
    RequestParser p;
    CHECK_OK(FeedAll(p,
                     "POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5;foo=bar\r\nhello\r\n0\r\n\r\n"));
    CHECK(p.complete());
    CHECK_EQ(p.request().body, std::string("hello"));
}

RELAY_TEST(chunked_trailers_are_consumed_and_dropped) {
    // The proxy re-frames with Content-Length, so trailers cannot be
    // forwarded. They must still be consumed or the connection desynchronises.
    RequestParser p;
    CHECK_OK(FeedAll(p,
                     "POST /x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n0\r\nX-Checksum: abc\r\n\r\n"));
    CHECK(p.complete());
    CHECK_EQ(p.request().body, std::string("hello"));
    CHECK(p.request().Find("x-checksum") == nullptr);
}

RELAY_TEST(keep_alive_defaults_by_version) {
    {
        RequestParser p;
        CHECK_OK(FeedAll(p, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
        CHECK(p.request().keep_alive);
    }
    {
        RequestParser p;
        CHECK_OK(FeedAll(p, "GET / HTTP/1.0\r\nHost: x\r\n\r\n"));
        CHECK(!p.request().keep_alive);
    }
    {
        RequestParser p;
        CHECK_OK(FeedAll(p, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CHECK(!p.request().keep_alive);
    }
    {
        RequestParser p;
        CHECK_OK(FeedAll(p, "GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"));
        CHECK(p.request().keep_alive);
    }
}

RELAY_TEST(pipelined_requests_report_consumed_bytes) {
    // The parser must stop at the end of the first request and report how much
    // it used, or the second request of a pipelined pair is swallowed. A parser
    // that assumes one read is one request gets this wrong.
    std::string two = std::string(kSimple) + "GET /second HTTP/1.1\r\nHost: x\r\n\r\n";
    RequestParser p;
    size_t consumed = 0;
    CHECK_OK(p.Feed(two, &consumed));
    CHECK(p.complete());
    CHECK_EQ(p.request().target, std::string("/hello"));
    CHECK(consumed < two.size());

    // Reset must preserve the buffered remainder.
    p.Reset();
    size_t consumed2 = 0;
    CHECK_OK(p.Feed("", &consumed2));
    CHECK(p.complete());
    CHECK_EQ(p.request().target, std::string("/second"));
}

RELAY_TEST(leading_blank_line_is_tolerated) {
    RequestParser p;
    CHECK_OK(FeedAll(p, std::string("\r\n") + kSimple));
    CHECK(p.complete());
    CHECK_EQ(p.request().target, std::string("/hello"));
}

RELAY_TEST(bare_lf_line_endings_accepted) {
    // A deliberate leniency: bare LF is widespread and rejecting it breaks
    // more than it protects. The dangerous leniencies are about framing, and
    // those are rejected strictly in smuggling_test.
    RequestParser p;
    CHECK_OK(FeedAll(p, "GET /x HTTP/1.1\nHost: y\n\n"));
    CHECK(p.complete());
    CHECK_EQ(p.request().target, std::string("/x"));
}

RELAY_TEST(malformed_request_lines_rejected) {
    const std::vector<std::string> bad = {
        "GET\r\n\r\n",
        "GET /x\r\n\r\n",
        "GET /x HTTP/2.0\r\n\r\n",
        "GET /x HTTP/1.2\r\n\r\n",
        "GE T /x HTTP/1.1\r\n\r\n",
        " GET /x HTTP/1.1\r\n\r\n",
        "GET  HTTP/1.1\r\n\r\n",
    };
    for (const auto& req : bad) {
        RequestParser p;
        size_t consumed = 0;
        Status s = p.Feed(req, &consumed);
        if (s.ok() && p.complete()) {
            std::fprintf(stderr, "        accepted: %s\n", req.c_str());
            CHECK(false);
        }
    }
}

RELAY_TEST(control_characters_in_target_rejected) {
    RequestParser p;
    size_t consumed = 0;
    Status s = p.Feed(std::string("GET /a\x01b HTTP/1.1\r\nHost: x\r\n\r\n"), &consumed);
    CHECK_NOT_OK(s);
}

RELAY_TEST(header_without_colon_rejected) {
    RequestParser p;
    size_t consumed = 0;
    CHECK_NOT_OK(p.Feed("GET / HTTP/1.1\r\nBrokenHeader\r\n\r\n", &consumed));
}

RELAY_TEST(header_name_with_space_rejected) {
    // "Host : x" is invalid. Tolerating it means disagreeing with a stricter
    // upstream about the header's name.
    RequestParser p;
    size_t consumed = 0;
    CHECK_NOT_OK(p.Feed("GET / HTTP/1.1\r\nHost : x\r\n\r\n", &consumed));
}

RELAY_TEST(limits_are_enforced_during_parsing) {
    {
        HttpLimits limits;
        limits.max_request_line = 32;
        RequestParser p(limits);
        size_t consumed = 0;
        std::string long_line = "GET /" + std::string(100, 'a') + " HTTP/1.1\r\n";
        CHECK_NOT_OK(p.Feed(long_line, &consumed));
    }
    {
        HttpLimits limits;
        limits.max_header_count = 2;
        RequestParser p(limits);
        size_t consumed = 0;
        CHECK_NOT_OK(p.Feed("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", &consumed));
    }
    {
        HttpLimits limits;
        limits.max_body_bytes = 8;
        RequestParser p(limits);
        size_t consumed = 0;
        CHECK_NOT_OK(
            p.Feed("POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\n", &consumed));
    }
}

RELAY_TEST(idempotency_classification) {
    auto method_is_idempotent = [](const char* m) {
        Request r;
        r.method = m;
        return r.idempotent();
    };
    CHECK(method_is_idempotent("GET"));
    CHECK(method_is_idempotent("HEAD"));
    CHECK(method_is_idempotent("PUT"));
    CHECK(method_is_idempotent("DELETE"));
    CHECK(method_is_idempotent("OPTIONS"));
    // POST and PATCH are not, which is what stops the retry logic from
    // replaying them after a timeout.
    CHECK(!method_is_idempotent("POST"));
    CHECK(!method_is_idempotent("PATCH"));
}

RELAY_TEST(serialize_strips_hop_by_hop_headers) {
    // Rejecting ambiguous framing on the way in is pointless if the proxy
    // re-emits it verbatim.
    Request r;
    r.method = "POST";
    r.target = "/x";
    r.headers = {{"host", "a"},
                 {"connection", "upgrade"},
                 {"transfer-encoding", "chunked"},
                 {"keep-alive", "timeout=5"},
                 {"te", "trailers"},
                 {"x-keep", "yes"}};
    r.body = "abc";
    std::string wire = r.Serialize();

    CHECK(wire.find("x-keep: yes") != std::string::npos);
    CHECK(wire.find("host: a") != std::string::npos);
    CHECK(wire.find("transfer-encoding") == std::string::npos);
    CHECK(wire.find("upgrade") == std::string::npos);
    CHECK(wire.find("keep-alive: timeout") == std::string::npos);
    CHECK(wire.find("te: trailers") == std::string::npos);
    // The proxy always re-frames with a length it computed itself.
    CHECK(wire.find("content-length: 3") != std::string::npos);
}

RELAY_TEST(response_parsing) {
    ResponseParser p;
    size_t consumed = 0;
    CHECK_OK(p.Feed("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nabc", &consumed));
    CHECK(p.complete());
    CHECK_EQ(p.response().status, 404);
    CHECK_EQ(p.response().reason, std::string("Not Found"));
    CHECK_EQ(p.response().body, std::string("abc"));
}

RELAY_TEST(bodyless_response_codes_complete_immediately) {
    // A parser that waits for a body on a 304 hangs the connection until the
    // read timeout, which presents as upstream slowness and is a parser bug.
    for (int status : {204, 304, 100}) {
        ResponseParser p;
        size_t consumed = 0;
        std::string wire = "HTTP/1.1 " + std::to_string(status) +
                           " X\r\nContent-Length: 50\r\n\r\n";
        CHECK_OK(p.Feed(wire, &consumed));
        if (!p.complete()) {
            std::fprintf(stderr, "        status %d did not complete\n", status);
            CHECK(false);
        }
    }
}

RELAY_TEST(response_chunked) {
    ResponseParser p;
    size_t consumed = 0;
    CHECK_OK(p.Feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "3\r\nfoo\r\n3\r\nbar\r\n0\r\n\r\n",
                    &consumed));
    CHECK(p.complete());
    CHECK_EQ(p.response().body, std::string("foobar"));
}

RELAY_TEST(response_body_until_close_needs_eof) {
    // An HTTP/1.0-style response with no framing headers. Without MarkEof the
    // parser waits forever and the proxy reports a timeout on a response it
    // has fully received.
    ResponseParser p;
    size_t consumed = 0;
    CHECK_OK(p.Feed("HTTP/1.0 200 OK\r\n\r\nbody bytes", &consumed));
    CHECK(!p.complete());
    CHECK(p.MarkEof());
    CHECK(p.complete());
    CHECK_EQ(p.response().body, std::string("body bytes"));
    // And such a connection must not be pooled.
    CHECK(!p.response().keep_alive);
}

RELAY_TEST(eof_mid_headers_is_not_completion) {
    // Reporting a truncated response as complete would hand a client a
    // partial body as if it were whole.
    ResponseParser p;
    size_t consumed = 0;
    CHECK_OK(p.Feed("HTTP/1.1 200 OK\r\nContent-Len", &consumed));
    CHECK(!p.MarkEof());
    CHECK(!p.complete());
}

RELAY_TEST(header_value_safety) {
    CHECK(HeaderValueIsSafe("normal value"));
    CHECK(!HeaderValueIsSafe("has\rcarriage"));
    CHECK(!HeaderValueIsSafe("has\nnewline"));
    CHECK(!HeaderValueIsSafe(std::string("has\0nul", 7)));
}

RELAY_TEST_MAIN()
