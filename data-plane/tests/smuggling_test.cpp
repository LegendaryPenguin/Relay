#include <string>
#include <vector>

#include "relay/http.hpp"
#include "testing.hpp"

using namespace relay;

// Request smuggling defences, in their own file because they are the reason
// writing the parser by hand is defensible at all.
//
// The attack: a proxy and its upstream disagree about where one request ends
// and the next begins. An attacker sends bytes that the proxy reads as one
// request and the upstream reads as two, and the second one gets attributed to
// whoever's connection is reused next.
//
// Every case here is rejected rather than resolved. That is the whole
// strategy: *any* precedence rule the proxy applies is a rule the upstream may
// apply differently, so the only safe answer to an ambiguous framing is to
// refuse the request.

namespace {

Status Parse(std::string_view wire, RequestParser* p) {
    size_t consumed = 0;
    return p->Feed(wire, &consumed);
}

bool Rejected(std::string_view wire) {
    RequestParser p;
    Status s = Parse(wire, &p);
    return !s.ok();
}

}  // namespace

RELAY_TEST(cl_te_conflict_rejected) {
    // The classic CL.TE / TE.CL attack. Both headers present means the two
    // ends may frame differently. There is no safe precedence, so refuse.
    CHECK(Rejected(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 6\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n"));

    // Order reversed makes no difference.
    CHECK(Rejected(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 6\r\n"
        "\r\n"
        "0\r\n\r\n"));
}

RELAY_TEST(duplicate_content_length_rejected) {
    // RFC 9110 permits combining identical values. This refuses anyway: a
    // proxy that resolves the ambiguity at all can resolve it differently from
    // its upstream.
    CHECK(Rejected(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello"));
    CHECK(Rejected(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello"));
}

RELAY_TEST(obfuscated_transfer_encoding_rejected) {
    // Every one of these is a real bypass attempt against proxies that
    // pattern-match for "chunked" rather than requiring the value to *be*
    // chunked.
    const std::vector<std::string> values = {
        "chunked, identity",
        "identity, chunked",
        "chunked\tx",
        "xchunked",
        "chunkedx",
        "gzip, chunked",
        "chunked;q=1",
        "identity",
        "gzip",
    };
    for (const auto& v : values) {
        std::string wire = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: " + v +
                           "\r\n\r\n0\r\n\r\n";
        if (!Rejected(wire)) {
            std::fprintf(stderr, "        accepted Transfer-Encoding: %s\n", v.c_str());
            CHECK(false);
        }
    }
}

RELAY_TEST(transfer_encoding_chunked_case_insensitive_accepted) {
    // The one spelling that must work, in any case. Rejecting this would break
    // legitimate clients.
    for (const char* v : {"chunked", "Chunked", "CHUNKED", "ChUnKeD"}) {
        std::string wire = std::string("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ") + v +
                           "\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
        RequestParser p;
        Status s = Parse(wire, &p);
        if (!s.ok() || !p.complete() || p.request().body != "hello") {
            std::fprintf(stderr, "        rejected valid Transfer-Encoding: %s\n", v);
            CHECK(false);
        }
    }
}

RELAY_TEST(obsolete_line_folding_rejected) {
    // Deprecated by RFC 9110. Implementations disagree on how to unfold, and a
    // disagreement about header boundaries is a smuggling vector.
    CHECK(Rejected(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "    6\r\n"
        "\r\n"
        "hello"));
}

RELAY_TEST(header_injection_via_crlf_rejected) {
    // A CR or LF inside a header value would let a client write its own
    // headers into the upstream request. The parser cannot see an embedded
    // CRLF as part of a value — it terminates the line — so the realistic
    // vector is a lone CR, which is what this checks.
    CHECK(Rejected("GET / HTTP/1.1\r\nHost: x\rSmuggled: yes\r\n\r\n"));
    CHECK(Rejected(std::string("GET / HTTP/1.1\r\nX-Bad: a\0b\r\n\r\n", 31)));
}

RELAY_TEST(non_numeric_content_length_rejected) {
    // Note "5 " is absent: trailing optional whitespace is legal per RFC 9110
    // and is stripped before parsing, so rejecting it would be strictness
    // with no security argument. " 5x" is here because stripping leaves "5x".
    const std::vector<std::string> values = {
        "+5", "-5", "5.0", "0x5", " 5x", "five", "", "5,5",
    };
    for (const auto& v : values) {
        std::string wire = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " + v + "\r\n\r\nhello";
        if (!Rejected(wire)) {
            std::fprintf(stderr, "        accepted Content-Length: '%s'\n", v.c_str());
            CHECK(false);
        }
    }
}

RELAY_TEST(leading_zero_content_length_accepted) {
    // "007" is unambiguously 7 under any decimal reading, so refusing it would
    // be strictness with no security argument behind it. The line between
    // "reject ambiguity" and "reject anything unusual" is worth holding.
    RequestParser p;
    Status s = Parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 007\r\n\r\n1234567", &p);
    CHECK_OK(s);
    CHECK(p.complete());
    CHECK_EQ(p.request().body, std::string("1234567"));
}

RELAY_TEST(non_hex_chunk_size_rejected) {
    for (const char* v : {"-1", "+1", "0x5", "zz", "5.0", ""}) {
        std::string wire = std::string("POST / HTTP/1.1\r\nHost: x\r\n"
                                       "Transfer-Encoding: chunked\r\n\r\n") +
                           v + "\r\nhello\r\n0\r\n\r\n";
        if (!Rejected(wire)) {
            std::fprintf(stderr, "        accepted chunk size '%s'\n", v);
            CHECK(false);
        }
    }
}

RELAY_TEST(chunk_not_terminated_by_crlf_rejected) {
    // A chunk whose data is not followed by CRLF desynchronises the stream,
    // and a parser that resynchronises by scanning for the next plausible
    // size line is exactly how a smuggled chunk gets through.
    CHECK(Rejected(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhelloGARBAGE0\r\n\r\n"));
}

RELAY_TEST(oversized_chunk_rejected_before_allocating) {
    HttpLimits limits;
    limits.max_body_bytes = 1024;
    RequestParser p(limits);
    size_t consumed = 0;
    // 0xFFFFFFFF bytes declared. Must be refused on the size line, not after
    // trying to buffer it.
    Status s = p.Feed("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "ffffffff\r\n",
                      &consumed);
    CHECK_NOT_OK(s);
}

RELAY_TEST(accumulated_chunks_cannot_exceed_body_limit) {
    // Each chunk is individually small; together they exceed the limit. A
    // check that only looks at one chunk at a time misses this.
    HttpLimits limits;
    limits.max_body_bytes = 16;
    RequestParser p(limits);
    std::string wire = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (int i = 0; i < 10; ++i) wire += "8\r\n01234567\r\n";
    wire += "0\r\n\r\n";
    size_t consumed = 0;
    CHECK_NOT_OK(p.Feed(wire, &consumed));
}

RELAY_TEST(response_side_framing_conflict_rejected) {
    // An upstream that sends both is as dangerous as a client that does: the
    // proxy and the client could then disagree about the response boundary.
    ResponseParser p;
    size_t consumed = 0;
    Status s = p.Feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                      &consumed);
    CHECK_NOT_OK(s);
}

RELAY_TEST(response_duplicate_content_length_rejected) {
    ResponseParser p;
    size_t consumed = 0;
    CHECK_NOT_OK(p.Feed("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc",
                        &consumed));
}

RELAY_TEST(a_valid_request_is_still_accepted) {
    // Guards against the whole file passing because the parser rejects
    // everything, which is a real risk when a test file is all negatives.
    RequestParser p;
    Status s = Parse("POST /api HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", &p);
    CHECK_OK(s);
    CHECK(p.complete());
    CHECK_EQ(p.request().body, std::string("hello"));
}

RELAY_TEST_MAIN()
