#include "relay/http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace relay {

const char* CodeName(Code code) {
    switch (code) {
        case Code::kOk: return "ok";
        case Code::kInvalidRequest: return "invalid_request";
        case Code::kUpstreamFailure: return "upstream_failure";
        case Code::kTimeout: return "timeout";
        case Code::kOverloaded: return "overloaded";
        case Code::kNoHealthyUpstream: return "no_healthy_upstream";
        case Code::kInternal: return "internal";
    }
    return "unknown";
}

int Status::http_status() const {
    switch (code_) {
        case Code::kOk: return 200;
        case Code::kInvalidRequest: return 400;
        case Code::kUpstreamFailure: return 502;
        case Code::kTimeout: return 504;
        // 503 with a Retry-After is the honest answer to deliberate shedding:
        // the request was not attempted and trying later may work.
        case Code::kOverloaded: return 503;
        case Code::kNoHealthyUpstream: return 503;
        case Code::kInternal: return 500;
    }
    return 500;
}

std::string Status::ToString() const {
    std::string out = CodeName(code_);
    if (!message_.empty()) {
        out += ": ";
        out += message_;
    }
    return out;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool HeaderValueIsSafe(std::string_view value) {
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

namespace {

std::string_view TrimOws(std::string_view s) {
    // Optional whitespace per RFC 9110 is space and horizontal tab only. Not
    // using isspace(): that would also strip \v and \f, which are not OWS, and
    // a parser that is more permissive than the upstream is how smuggling
    // discrepancies arise.
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool IsTokenChar(unsigned char c) {
    // RFC 9110 tchar. A method or header name outside this set is rejected
    // rather than passed through, because a name containing a space or colon
    // reframes the rest of the request.
    if (std::isalnum(c)) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

bool IsToken(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!IsTokenChar(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Extracts one CRLF-terminated line from pending_, if present.
//
// Accepts a bare LF as a line terminator as well as CRLF. That is a
// deliberate leniency with a specific justification: bare-LF is widespread in
// real clients, and rejecting it breaks more than it protects. The dangerous
// leniencies are the ones about *framing* — Content-Length vs
// Transfer-Encoding — and those are rejected strictly.
bool TakeLine(std::string& buffer, std::string* line) {
    size_t nl = buffer.find('\n');
    if (nl == std::string::npos) return false;
    size_t end = nl;
    if (end > 0 && buffer[end - 1] == '\r') --end;
    *line = buffer.substr(0, end);
    buffer.erase(0, nl + 1);
    return true;
}

std::optional<size_t> ParseDecimal(std::string_view s, size_t limit) {
    if (s.empty()) return std::nullopt;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    size_t value = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), value, 10);
    if (result.ec != std::errc() || result.ptr != s.data() + s.size()) return std::nullopt;
    if (value > limit) return std::nullopt;
    return value;
}

std::optional<size_t> ParseHex(std::string_view s, size_t limit) {
    // A chunk-size line may carry extensions after a semicolon. They are
    // ignored, but their presence must not make the size unparseable.
    size_t semi = s.find(';');
    if (semi != std::string_view::npos) s = s.substr(0, semi);
    s = TrimOws(s);
    if (s.empty()) return std::nullopt;
    size_t value = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), value, 16);
    if (result.ec != std::errc() || result.ptr != s.data() + s.size()) return std::nullopt;
    if (value > limit) return std::nullopt;
    return value;
}

bool DecideKeepAlive(const std::vector<Header>& headers, int minor_version) {
    // HTTP/1.1 defaults to keep-alive, 1.0 to close; `Connection` overrides.
    bool keep = minor_version >= 1;
    for (const auto& h : headers) {
        if (h.name != "connection") continue;
        std::string v = ToLowerAscii(h.value);
        if (v.find("close") != std::string::npos) return false;
        if (v.find("keep-alive") != std::string::npos) keep = true;
    }
    return keep;
}

}  // namespace

const std::string* Request::Find(std::string_view lowercase_name) const {
    for (const auto& h : headers) {
        if (h.name == lowercase_name) return &h.value;
    }
    return nullptr;
}

bool Request::idempotent() const {
    // GET, HEAD, OPTIONS, TRACE and PUT/DELETE are idempotent per RFC 9110.
    // PUT and DELETE are included because repeating them lands the resource in
    // the same state; POST and PATCH are not, and are never retried after a
    // timeout because a timeout cannot distinguish "never arrived" from
    // "arrived and the response was lost".
    return method == "GET" || method == "HEAD" || method == "OPTIONS" ||
           method == "TRACE" || method == "PUT" || method == "DELETE";
}

std::string Request::Serialize() const {
    std::string out;
    out.reserve(256 + body.size());
    out += method;
    out += ' ';
    out += target;
    out += minor_version >= 1 ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n";
    for (const auto& h : headers) {
        // Hop-by-hop headers are not forwarded. Passing `connection` or
        // `transfer-encoding` through would let the client's framing decisions
        // reach the upstream, which is the other half of the smuggling
        // problem — rejecting ambiguous framing on the way in is pointless if
        // the proxy then re-emits it verbatim.
        if (h.name == "connection" || h.name == "keep-alive" ||
            h.name == "transfer-encoding" || h.name == "upgrade" ||
            h.name == "proxy-connection" || h.name == "te" || h.name == "trailer") {
            continue;
        }
        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
    }
    // The proxy always re-frames the body with an explicit length it computed
    // itself, regardless of how the request arrived. A chunked request becomes
    // a length-delimited one upstream, which removes any chance of the two
    // sides disagreeing about where the body ends.
    out += "content-length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    out += "connection: keep-alive\r\n\r\n";
    out += body;
    return out;
}

const std::string* Response::Find(std::string_view lowercase_name) const {
    for (const auto& h : headers) {
        if (h.name == lowercase_name) return &h.value;
    }
    return nullptr;
}

std::string Response::Serialize() const {
    std::string out;
    out.reserve(128 + body.size());
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason.empty() ? "OK" : reason;
    out += "\r\n";
    for (const auto& h : headers) {
        if (h.name == "connection" || h.name == "keep-alive" ||
            h.name == "transfer-encoding" || h.name == "content-length") {
            continue;
        }
        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
    }
    out += "content-length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    out += keep_alive ? "connection: keep-alive\r\n\r\n" : "connection: close\r\n\r\n";
    out += body;
    return out;
}

Response Response::Simple(int status, std::string_view reason, std::string body) {
    Response r;
    r.status = status;
    r.reason = std::string(reason);
    r.body = std::move(body);
    r.headers.push_back({"content-type", "text/plain; charset=utf-8"});
    return r;
}

// ---------------------------------------------------------------------------
// RequestParser
// ---------------------------------------------------------------------------

void RequestParser::Reset() {
    state_ = State::kRequestLine;
    request_ = Request{};
    // pending_ is deliberately NOT cleared: on a keep-alive connection it may
    // already hold the start of the next pipelined request. Clearing it here
    // was a bug that dropped the second request of every pipelined pair.
    header_bytes_ = 0;
    body_remaining_ = 0;
    chunk_remaining_ = 0;
    chunked_ = false;
}

Status RequestParser::ParseRequestLine(std::string_view line) {
    if (line.size() > limits_.max_request_line) {
        return Status::InvalidRequest("request line exceeds limit");
    }
    size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return Status::InvalidRequest("malformed request line");
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return Status::InvalidRequest("malformed request line");

    std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);

    if (!IsToken(method)) return Status::InvalidRequest("method is not a token");
    if (target.empty()) return Status::InvalidRequest("empty request target");
    // A target containing whitespace or control characters would reframe the
    // request line for anything downstream that splits on spaces.
    for (char c : target) {
        auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return Status::InvalidRequest("control character in target");
    }

    if (version == "HTTP/1.1") {
        request_.minor_version = 1;
    } else if (version == "HTTP/1.0") {
        request_.minor_version = 0;
    } else {
        return Status::InvalidRequest("unsupported HTTP version");
    }

    request_.method = std::string(method);
    request_.target = std::string(target);
    return Status::Ok();
}

Status RequestParser::ParseHeaderLine(std::string_view line) {
    header_bytes_ += line.size();
    if (header_bytes_ > limits_.max_header_bytes) {
        return Status::InvalidRequest("headers exceed byte limit");
    }
    if (request_.headers.size() >= limits_.max_header_count) {
        return Status::InvalidRequest("too many headers");
    }
    // Leading whitespace means an obs-fold continuation line. Deprecated by
    // RFC 9110 and rejected rather than unfolded: implementations disagree on
    // how to unfold, and a disagreement about header boundaries is a
    // smuggling vector.
    if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        return Status::InvalidRequest("obsolete line folding in headers");
    }

    size_t colon = line.find(':');
    if (colon == std::string_view::npos) return Status::InvalidRequest("header without colon");
    std::string_view name = line.substr(0, colon);
    // No OWS before the colon: "Host : x" is invalid, and tolerating it means
    // disagreeing with a stricter upstream about the header's name.
    if (!IsToken(name)) return Status::InvalidRequest("header name is not a token");

    std::string_view value = TrimOws(line.substr(colon + 1));
    if (!HeaderValueIsSafe(value)) return Status::InvalidRequest("control character in header value");

    std::string lower = ToLowerAscii(name);

    // Duplicate Content-Length is rejected outright. RFC 9110 permits
    // combining identical values, but a proxy that resolves the ambiguity at
    // all is a proxy that can resolve it differently from its upstream.
    if (lower == "content-length") {
        if (request_.Has("content-length")) {
            return Status::InvalidRequest("duplicate content-length");
        }
    }

    request_.headers.push_back({std::move(lower), std::string(value)});
    return Status::Ok();
}

Status RequestParser::FinishHeaders() {
    const std::string* te = request_.Find("transfer-encoding");
    const std::string* cl = request_.Find("content-length");

    // The central smuggling defence. Both present means the two ends may pick
    // different framing; there is no safe precedence rule, so refuse.
    if (te != nullptr && cl != nullptr) {
        return Status::InvalidRequest(
            "both transfer-encoding and content-length present (request smuggling risk)");
    }

    if (te != nullptr) {
        std::string lower = ToLowerAscii(*te);
        if (lower != "chunked") {
            // "identity", unknown codings, and chunked-with-anything-after are
            // all refused. Accepting "chunked, gzip" and treating it as
            // chunked is precisely the discrepancy attackers look for.
            return Status::InvalidRequest("unsupported transfer-encoding");
        }
        chunked_ = true;
        state_ = State::kChunkSize;
        return Status::Ok();
    }

    if (cl != nullptr) {
        auto parsed = ParseDecimal(*cl, limits_.max_body_bytes);
        if (!parsed.has_value()) return Status::InvalidRequest("invalid content-length");
        body_remaining_ = *parsed;
        state_ = body_remaining_ > 0 ? State::kBody : State::kComplete;
        return Status::Ok();
    }

    // No framing headers means no body for a request. (A response without
    // them reads until close, which is why ResponseParser differs here.)
    state_ = State::kComplete;
    return Status::Ok();
}

Status RequestParser::Feed(std::string_view data, size_t* consumed) {
    size_t start_size = pending_.size();
    pending_.append(data);
    size_t initial_pending = start_size;

    auto finish = [&](size_t leftover) {
        // Bytes consumed from *this* call: everything we appended minus
        // whatever is still buffered, accounting for what was already there.
        size_t total_available = initial_pending + data.size();
        size_t used_total = total_available - leftover;
        *consumed = used_total > initial_pending ? used_total - initial_pending : 0;
    };

    std::string line;
    while (true) {
        switch (state_) {
            case State::kRequestLine: {
                if (!TakeLine(pending_, &line)) {
                    if (pending_.size() > limits_.max_request_line) {
                        state_ = State::kError;
                        finish(pending_.size());
                        return Status::InvalidRequest("request line exceeds limit");
                    }
                    finish(pending_.size());
                    return Status::Ok();
                }
                // A leading empty line before a request is tolerated: RFC 9112
                // allows a server to skip at least one CRLF, and some clients
                // emit a stray one after a previous request's body.
                if (line.empty()) continue;
                Status s = ParseRequestLine(line);
                if (!s.ok()) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return s;
                }
                state_ = State::kHeaders;
                break;
            }
            case State::kHeaders: {
                if (!TakeLine(pending_, &line)) {
                    if (pending_.size() > limits_.max_header_bytes) {
                        state_ = State::kError;
                        finish(pending_.size());
                        return Status::InvalidRequest("headers exceed byte limit");
                    }
                    finish(pending_.size());
                    return Status::Ok();
                }
                if (line.empty()) {
                    Status s = FinishHeaders();
                    if (!s.ok()) {
                        state_ = State::kError;
                        finish(pending_.size());
                        return s;
                    }
                    request_.keep_alive = DecideKeepAlive(request_.headers, request_.minor_version);
                    if (state_ == State::kComplete) {
                        finish(pending_.size());
                        return Status::Ok();
                    }
                    break;
                }
                Status s = ParseHeaderLine(line);
                if (!s.ok()) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return s;
                }
                break;
            }
            case State::kBody: {
                size_t take = std::min(body_remaining_, pending_.size());
                request_.body.append(pending_, 0, take);
                pending_.erase(0, take);
                body_remaining_ -= take;
                if (body_remaining_ == 0) {
                    state_ = State::kComplete;
                    finish(pending_.size());
                    return Status::Ok();
                }
                finish(pending_.size());
                return Status::Ok();
            }
            case State::kChunkSize: {
                if (!TakeLine(pending_, &line)) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                auto size = ParseHex(line, limits_.max_body_bytes);
                if (!size.has_value()) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return Status::InvalidRequest("invalid chunk size");
                }
                chunk_remaining_ = *size;
                if (chunk_remaining_ == 0) {
                    state_ = State::kChunkTrailer;
                    break;
                }
                if (request_.body.size() + chunk_remaining_ > limits_.max_body_bytes) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return Status::InvalidRequest("chunked body exceeds limit");
                }
                state_ = State::kChunkData;
                break;
            }
            case State::kChunkData: {
                size_t take = std::min(chunk_remaining_, pending_.size());
                request_.body.append(pending_, 0, take);
                pending_.erase(0, take);
                chunk_remaining_ -= take;
                if (chunk_remaining_ > 0) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                // Consume the CRLF that terminates the chunk. If it has not
                // arrived yet, wait rather than treating the next chunk size
                // line as data.
                if (!TakeLine(pending_, &line)) {
                    // Put the parser back so the terminator is awaited.
                    state_ = State::kChunkData;
                    chunk_remaining_ = 0;
                    finish(pending_.size());
                    return Status::Ok();
                }
                if (!line.empty()) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return Status::InvalidRequest("chunk not terminated by CRLF");
                }
                state_ = State::kChunkSize;
                break;
            }
            case State::kChunkTrailer: {
                if (!TakeLine(pending_, &line)) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                if (line.empty()) {
                    // The proxy re-frames with Content-Length, so trailers are
                    // dropped rather than forwarded. Stated here because
                    // silently dropping data is the kind of thing that should
                    // be a deliberate note rather than an omission.
                    state_ = State::kComplete;
                    finish(pending_.size());
                    return Status::Ok();
                }
                // Trailer fields are validated but discarded.
                if (line.find(':') == std::string::npos) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return Status::InvalidRequest("malformed trailer");
                }
                break;
            }
            case State::kComplete:
            case State::kError:
                finish(pending_.size());
                return Status::Ok();
        }
    }
}

// ---------------------------------------------------------------------------
// ResponseParser
// ---------------------------------------------------------------------------

void ResponseParser::Reset() {
    state_ = State::kStatusLine;
    response_ = Response{};
    header_bytes_ = 0;
    body_remaining_ = 0;
    chunk_remaining_ = 0;
    chunked_ = false;
    body_until_close_ = false;
}

Status ResponseParser::ParseStatusLine(std::string_view line) {
    if (line.size() > limits_.max_request_line) {
        return Status::UpstreamFailure("status line exceeds limit");
    }
    if (line.rfind("HTTP/1.", 0) != 0 || line.size() < 12) {
        return Status::UpstreamFailure("malformed status line");
    }
    response_.keep_alive = line[7] == '1';
    size_t sp = line.find(' ', 8);
    if (sp == std::string_view::npos) return Status::UpstreamFailure("malformed status line");
    std::string_view code = line.substr(sp + 1, 3);
    auto parsed = ParseDecimal(code, 599);
    if (!parsed.has_value() || *parsed < 100) {
        return Status::UpstreamFailure("invalid status code");
    }
    response_.status = static_cast<int>(*parsed);
    if (line.size() > sp + 5) response_.reason = std::string(line.substr(sp + 5));
    return Status::Ok();
}

Status ResponseParser::ParseHeaderLine(std::string_view line) {
    header_bytes_ += line.size();
    if (header_bytes_ > limits_.max_header_bytes) {
        return Status::UpstreamFailure("response headers exceed limit");
    }
    if (response_.headers.size() >= limits_.max_header_count) {
        return Status::UpstreamFailure("too many response headers");
    }
    size_t colon = line.find(':');
    if (colon == std::string_view::npos) return Status::UpstreamFailure("header without colon");
    std::string lower = ToLowerAscii(line.substr(0, colon));
    std::string_view value = TrimOws(line.substr(colon + 1));
    if (!HeaderValueIsSafe(value)) {
        return Status::UpstreamFailure("control character in response header");
    }
    if (lower == "content-length" && response_.Find("content-length") != nullptr) {
        return Status::UpstreamFailure("duplicate content-length from upstream");
    }
    response_.headers.push_back({std::move(lower), std::string(value)});
    return Status::Ok();
}

Status ResponseParser::FinishHeaders() {
    const std::string* te = response_.Find("transfer-encoding");
    const std::string* cl = response_.Find("content-length");
    if (te != nullptr && cl != nullptr) {
        return Status::UpstreamFailure(
            "upstream sent both transfer-encoding and content-length");
    }

    // 1xx, 204 and 304 never have a body regardless of headers. A parser that
    // waits for one on a 304 hangs the connection until the read timeout,
    // which presents as upstream slowness and is really a parser bug.
    if ((response_.status >= 100 && response_.status < 200) || response_.status == 204 ||
        response_.status == 304) {
        state_ = State::kComplete;
        return Status::Ok();
    }

    if (te != nullptr) {
        if (ToLowerAscii(*te) != "chunked") {
            return Status::UpstreamFailure("unsupported transfer-encoding from upstream");
        }
        chunked_ = true;
        state_ = State::kChunkSize;
        return Status::Ok();
    }
    if (cl != nullptr) {
        auto parsed = ParseDecimal(*cl, limits_.max_body_bytes);
        if (!parsed.has_value()) return Status::UpstreamFailure("invalid content-length");
        body_remaining_ = *parsed;
        state_ = body_remaining_ > 0 ? State::kBody : State::kComplete;
        return Status::Ok();
    }

    // Neither header: HTTP/1.0-style body terminated by connection close. The
    // connection cannot be pooled afterwards, which is why keep_alive is
    // forced off here rather than trusted from the status line.
    body_until_close_ = true;
    response_.keep_alive = false;
    state_ = State::kBody;
    body_remaining_ = limits_.max_body_bytes;
    return Status::Ok();
}

Status ResponseParser::Feed(std::string_view data, size_t* consumed) {
    size_t initial_pending = pending_.size();
    pending_.append(data);

    auto finish = [&](size_t leftover) {
        size_t total_available = initial_pending + data.size();
        size_t used_total = total_available - leftover;
        *consumed = used_total > initial_pending ? used_total - initial_pending : 0;
    };

    std::string line;
    while (true) {
        switch (state_) {
            case State::kStatusLine: {
                if (!TakeLine(pending_, &line)) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                if (line.empty()) continue;
                Status s = ParseStatusLine(line);
                if (!s.ok()) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return s;
                }
                state_ = State::kHeaders;
                break;
            }
            case State::kHeaders: {
                if (!TakeLine(pending_, &line)) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                if (line.empty()) {
                    Status s = FinishHeaders();
                    if (!s.ok()) {
                        state_ = State::kError;
                        finish(pending_.size());
                        return s;
                    }
                    bool explicit_close = false;
                    for (const auto& h : response_.headers) {
                        if (h.name == "connection" &&
                            ToLowerAscii(h.value).find("close") != std::string::npos) {
                            explicit_close = true;
                        }
                    }
                    if (explicit_close) response_.keep_alive = false;
                    if (state_ == State::kComplete) {
                        finish(pending_.size());
                        return Status::Ok();
                    }
                    break;
                }
                Status s = ParseHeaderLine(line);
                if (!s.ok()) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return s;
                }
                break;
            }
            case State::kBody: {
                size_t take = std::min(body_remaining_, pending_.size());
                response_.body.append(pending_, 0, take);
                pending_.erase(0, take);
                body_remaining_ -= take;
                if (body_remaining_ == 0 && !body_until_close_) {
                    state_ = State::kComplete;
                }
                finish(pending_.size());
                return Status::Ok();
            }
            case State::kChunkSize: {
                if (!TakeLine(pending_, &line)) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                auto size = ParseHex(line, limits_.max_body_bytes);
                if (!size.has_value()) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return Status::UpstreamFailure("invalid chunk size from upstream");
                }
                chunk_remaining_ = *size;
                if (chunk_remaining_ == 0) {
                    state_ = State::kChunkTrailer;
                    break;
                }
                if (response_.body.size() + chunk_remaining_ > limits_.max_body_bytes) {
                    state_ = State::kError;
                    finish(pending_.size());
                    return Status::UpstreamFailure("response body exceeds limit");
                }
                state_ = State::kChunkData;
                break;
            }
            case State::kChunkData: {
                size_t take = std::min(chunk_remaining_, pending_.size());
                response_.body.append(pending_, 0, take);
                pending_.erase(0, take);
                chunk_remaining_ -= take;
                if (chunk_remaining_ > 0) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                if (!TakeLine(pending_, &line)) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                state_ = State::kChunkSize;
                break;
            }
            case State::kChunkTrailer: {
                if (!TakeLine(pending_, &line)) {
                    finish(pending_.size());
                    return Status::Ok();
                }
                if (line.empty()) {
                    state_ = State::kComplete;
                    finish(pending_.size());
                    return Status::Ok();
                }
                break;
            }
            case State::kComplete:
            case State::kError:
                finish(pending_.size());
                return Status::Ok();
        }
    }
}

bool ResponseParser::MarkEof() {
    if (state_ == State::kComplete) return true;
    if (state_ == State::kBody && body_until_close_) {
        // The close *is* the terminator for this framing.
        state_ = State::kComplete;
        return true;
    }
    // A close anywhere else truncated the response. Left in its current state
    // so the caller can tell a complete response from a severed one; reporting
    // this as complete would hand a client a truncated body as if it were
    // whole, which is worse than a 502.
    return false;
}

}  // namespace relay
