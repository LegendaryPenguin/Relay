#include "relay/json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace relay {

namespace {

const std::string kEmptyString;
const std::vector<JsonPtr> kEmptyArray;
const std::map<std::string, JsonPtr> kEmptyObject;

}  // namespace

JsonPtr JsonValue::MakeNull() { return std::make_shared<JsonValue>(); }

JsonPtr JsonValue::MakeBool(bool v) {
    auto p = std::make_shared<JsonValue>();
    p->kind_ = Kind::kBool;
    p->bool_ = v;
    return p;
}

JsonPtr JsonValue::MakeInt(int64_t v) {
    auto p = std::make_shared<JsonValue>();
    p->kind_ = Kind::kInt;
    p->int_ = v;
    p->double_ = static_cast<double>(v);
    return p;
}

JsonPtr JsonValue::MakeDouble(double v) {
    auto p = std::make_shared<JsonValue>();
    p->kind_ = Kind::kDouble;
    p->double_ = v;
    p->int_ = static_cast<int64_t>(v);
    return p;
}

JsonPtr JsonValue::MakeString(std::string v) {
    auto p = std::make_shared<JsonValue>();
    p->kind_ = Kind::kString;
    p->string_ = std::move(v);
    return p;
}

JsonPtr JsonValue::MakeArray(std::vector<JsonPtr> v) {
    auto p = std::make_shared<JsonValue>();
    p->kind_ = Kind::kArray;
    p->array_ = std::move(v);
    return p;
}

JsonPtr JsonValue::MakeObject(std::map<std::string, JsonPtr> v) {
    auto p = std::make_shared<JsonValue>();
    p->kind_ = Kind::kObject;
    p->object_ = std::move(v);
    return p;
}

bool JsonValue::AsBool(bool fallback) const {
    return kind_ == Kind::kBool ? bool_ : fallback;
}

int64_t JsonValue::AsInt(int64_t fallback) const {
    if (kind_ == Kind::kInt) return int_;
    // A double is accepted when it is exactly integral. JSON has one number
    // type, so a control plane serialising 3 as 3.0 is legitimate and
    // rejecting it would be pedantry; 3.5 as a port is not.
    if (kind_ == Kind::kDouble && double_ == std::trunc(double_)) {
        return static_cast<int64_t>(double_);
    }
    return fallback;
}

double JsonValue::AsDouble(double fallback) const {
    if (kind_ == Kind::kDouble || kind_ == Kind::kInt) return double_;
    return fallback;
}

const std::string& JsonValue::AsString() const {
    return kind_ == Kind::kString ? string_ : kEmptyString;
}

const std::vector<JsonPtr>& JsonValue::AsArray() const {
    return kind_ == Kind::kArray ? array_ : kEmptyArray;
}

const std::map<std::string, JsonPtr>& JsonValue::AsObject() const {
    return kind_ == Kind::kObject ? object_ : kEmptyObject;
}

const JsonValue* JsonValue::Find(std::string_view key) const {
    if (kind_ != Kind::kObject) return nullptr;
    auto it = object_.find(std::string(key));
    if (it == object_.end() || !it->second) return nullptr;
    return it->second.get();
}

int64_t JsonValue::IntField(std::string_view key, int64_t fallback) const {
    const JsonValue* v = Find(key);
    return v == nullptr ? fallback : v->AsInt(fallback);
}

double JsonValue::DoubleField(std::string_view key, double fallback) const {
    const JsonValue* v = Find(key);
    return v == nullptr ? fallback : v->AsDouble(fallback);
}

bool JsonValue::BoolField(std::string_view key, bool fallback) const {
    const JsonValue* v = Find(key);
    return v == nullptr ? fallback : v->AsBool(fallback);
}

std::string JsonValue::StringField(std::string_view key, std::string_view fallback) const {
    const JsonValue* v = Find(key);
    if (v == nullptr || v->kind() != Kind::kString) return std::string(fallback);
    return v->AsString();
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

class Parser {
  public:
    Parser(std::string_view text, JsonLimits limits) : text_(text), limits_(limits) {}

    Status Parse(JsonPtr* out) {
        SkipWhitespace();
        Status s = ParseValue(0, out);
        if (!s.ok()) return s;
        SkipWhitespace();
        if (pos_ != text_.size()) {
            // Trailing content is an error rather than ignored. A truncated
            // response concatenated with the next one would otherwise parse
            // as the first document and silently discard the rest.
            return Error("trailing content after the document");
        }
        return Status::Ok();
    }

  private:
    Status Error(const std::string& what) const {
        return Status::InvalidRequest("json: " + what + " at offset " + std::to_string(pos_));
    }

    void SkipWhitespace() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool Literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) == 0) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    Status ParseValue(size_t depth, JsonPtr* out) {
        if (depth > limits_.max_depth) {
            return Error("nesting exceeds the depth limit");
        }
        if (++values_ > limits_.max_values) {
            return Error("document exceeds the value limit");
        }
        if (pos_ >= text_.size()) return Error("unexpected end of input");

        char c = text_[pos_];
        switch (c) {
            case '{': return ParseObject(depth, out);
            case '[': return ParseArray(depth, out);
            case '"': {
                std::string s;
                Status st = ParseString(&s);
                if (!st.ok()) return st;
                *out = JsonValue::MakeString(std::move(s));
                return Status::Ok();
            }
            case 't':
                if (Literal("true")) {
                    *out = JsonValue::MakeBool(true);
                    return Status::Ok();
                }
                return Error("invalid literal");
            case 'f':
                if (Literal("false")) {
                    *out = JsonValue::MakeBool(false);
                    return Status::Ok();
                }
                return Error("invalid literal");
            case 'n':
                if (Literal("null")) {
                    *out = JsonValue::MakeNull();
                    return Status::Ok();
                }
                return Error("invalid literal");
            default:
                if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
                    return ParseNumber(out);
                }
                return Error("unexpected character");
        }
    }

    Status ParseObject(size_t depth, JsonPtr* out) {
        if (!Consume('{')) return Error("expected '{'");
        std::map<std::string, JsonPtr> members;
        SkipWhitespace();
        if (Consume('}')) {
            *out = JsonValue::MakeObject(std::move(members));
            return Status::Ok();
        }
        while (true) {
            SkipWhitespace();
            std::string key;
            Status s = ParseString(&key);
            if (!s.ok()) return s;
            SkipWhitespace();
            if (!Consume(':')) return Error("expected ':' after a key");
            SkipWhitespace();
            JsonPtr value;
            s = ParseValue(depth + 1, &value);
            if (!s.ok()) return s;
            // Last-wins on a duplicate key. RFC 8259 leaves this undefined;
            // noting it rather than rejecting, because a config generator
            // emitting a duplicate is a bug on their side that this parser
            // cannot usefully diagnose.
            members[std::move(key)] = std::move(value);
            SkipWhitespace();
            if (Consume(',')) continue;
            if (Consume('}')) break;
            return Error("expected ',' or '}' in an object");
        }
        *out = JsonValue::MakeObject(std::move(members));
        return Status::Ok();
    }

    Status ParseArray(size_t depth, JsonPtr* out) {
        if (!Consume('[')) return Error("expected '['");
        std::vector<JsonPtr> items;
        SkipWhitespace();
        if (Consume(']')) {
            *out = JsonValue::MakeArray(std::move(items));
            return Status::Ok();
        }
        while (true) {
            SkipWhitespace();
            JsonPtr value;
            Status s = ParseValue(depth + 1, &value);
            if (!s.ok()) return s;
            items.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(',')) continue;
            if (Consume(']')) break;
            return Error("expected ',' or ']' in an array");
        }
        *out = JsonValue::MakeArray(std::move(items));
        return Status::Ok();
    }

    // Appends a code point as UTF-8.
    static void AppendUtf8(std::string* out, uint32_t cp) {
        if (cp <= 0x7f) {
            out->push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool ParseHex4(uint32_t* out) {
        if (pos_ + 4 > text_.size()) return false;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_ + static_cast<size_t>(i)];
            int digit;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else {
                return false;
            }
            value = value * 16 + static_cast<uint32_t>(digit);
        }
        pos_ += 4;
        *out = value;
        return true;
    }

    Status ParseString(std::string* out) {
        if (!Consume('"')) return Error("expected a string");
        out->clear();
        while (true) {
            if (pos_ >= text_.size()) return Error("unterminated string");
            char c = text_[pos_++];
            if (c == '"') return Status::Ok();
            if (out->size() > limits_.max_string_bytes) {
                return Error("string exceeds the length limit");
            }
            if (c != '\\') {
                // Raw control characters are not legal in a JSON string.
                if (static_cast<unsigned char>(c) < 0x20) {
                    return Error("unescaped control character in a string");
                }
                out->push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return Error("unterminated escape");
            char e = text_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(&cp)) return Error("malformed \\u escape");
                    // Surrogate pair. A lone surrogate is passed through as
                    // its code point rather than rejected — documented in the
                    // header as a known deviation.
                    if (cp >= 0xd800 && cp <= 0xdbff && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        size_t save = pos_;
                        pos_ += 2;
                        uint32_t low = 0;
                        if (ParseHex4(&low) && low >= 0xdc00 && low <= 0xdfff) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        } else {
                            pos_ = save;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return Error("unknown escape");
            }
        }
    }

    Status ParseNumber(JsonPtr* out) {
        size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;

        // JSON forbids leading zeros ("01"), and accepting them means
        // disagreeing with the producer about the value.
        if (pos_ < text_.size() && text_[pos_] == '0') {
            ++pos_;
        } else {
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                return Error("expected a digit");
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }

        bool is_double = false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            is_double = true;
            ++pos_;
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                return Error("expected a digit after the decimal point");
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            is_double = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                return Error("expected a digit in the exponent");
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }

        std::string_view token = text_.substr(start, pos_ - start);
        if (!is_double) {
            int64_t value = 0;
            auto r = std::from_chars(token.data(), token.data() + token.size(), value, 10);
            if (r.ec == std::errc() && r.ptr == token.data() + token.size()) {
                // Integers stay integers. Round-tripping a port or a version
                // through double is how a value silently gains 0.5.
                *out = JsonValue::MakeInt(value);
                return Status::Ok();
            }
            // Too large for int64: fall through to double rather than
            // failing, since a huge number is not necessarily fatal for a
            // field the caller may not read.
        }
        // from_chars for double is not available on every standard library
        // this might be built against, so strtod with a NUL-terminated copy.
        std::string copy(token);
        char* end = nullptr;
        double d = std::strtod(copy.c_str(), &end);
        if (end != copy.c_str() + copy.size()) return Error("malformed number");
        *out = JsonValue::MakeDouble(d);
        return Status::Ok();
    }

    std::string_view text_;
    JsonLimits limits_;
    size_t pos_ = 0;
    size_t values_ = 0;
};

}  // namespace

Status JsonParse(std::string_view text, JsonPtr* out, JsonLimits limits) {
    Parser parser(text, limits);
    return parser.Parse(out);
}

}  // namespace relay
