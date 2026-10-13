#pragma once

// A minimal JSON reader, for parsing configuration from the control plane.
//
// Written rather than imported, consistent with the rest of the data plane.
// The justification is narrower here than for the HTTP parser: JSON has no
// equivalent of request smuggling, so this is a dependency decision rather
// than a security one. What it buys is that the proxy links nothing, and the
// cost is that this is a *reader*, not a general library — no writer, no
// pretty printing, no streaming.
//
// Two properties the configuration path actually needs:
//
//   * **Bounded.** Depth and total value count are capped, so a hostile or
//     corrupt config cannot exhaust the stack through nesting or the heap
//     through sheer size. A recursive-descent parser without a depth limit is
//     a stack overflow waiting for a `[[[[[...` input.
//
//   * **Lossless about numbers it cares about.** Integers are kept as int64
//     rather than round-tripped through double, because an endpoint port or a
//     snapshot version silently gaining 0.5 would be a miserable bug. Doubles
//     are available separately for the ratio fields.
//
// Not in scope: full RFC 8259 conformance. Surrogate-pair escapes are decoded
// to UTF-8 but lone surrogates are passed through rather than rejected, and
// duplicate object keys resolve last-wins. Both are noted here rather than
// discovered.

#include <cstdint>
#include <map>
#include <ostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "relay/status.hpp"

namespace relay {

class JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;

class JsonValue {
  public:
    enum class Kind { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

    static const char* KindName(Kind k) {
        switch (k) {
            case Kind::kNull: return "null";
            case Kind::kBool: return "bool";
            case Kind::kInt: return "int";
            case Kind::kDouble: return "double";
            case Kind::kString: return "string";
            case Kind::kArray: return "array";
            case Kind::kObject: return "object";
        }
        return "invalid";
    }

    static JsonPtr MakeNull();
    static JsonPtr MakeBool(bool v);
    static JsonPtr MakeInt(int64_t v);
    static JsonPtr MakeDouble(double v);
    static JsonPtr MakeString(std::string v);
    static JsonPtr MakeArray(std::vector<JsonPtr> v);
    static JsonPtr MakeObject(std::map<std::string, JsonPtr> v);

    Kind kind() const { return kind_; }

    bool is_null() const { return kind_ == Kind::kNull; }
    bool is_object() const { return kind_ == Kind::kObject; }
    bool is_array() const { return kind_ == Kind::kArray; }

    // Accessors return a default rather than throwing, and the caller checks
    // presence separately via Has/Find. A configuration parser that throws on
    // a missing optional field makes every optional field a special case.
    bool AsBool(bool fallback = false) const;
    int64_t AsInt(int64_t fallback = 0) const;
    double AsDouble(double fallback = 0.0) const;
    const std::string& AsString() const;

    const std::vector<JsonPtr>& AsArray() const;
    const std::map<std::string, JsonPtr>& AsObject() const;

    // Object member lookup. Returns nullptr when absent, so a caller can tell
    // "missing" from "present and null" — which matters for config, where a
    // missing field means "use the default" and an explicit null is a
    // mistake worth noticing.
    const JsonValue* Find(std::string_view key) const;
    bool Has(std::string_view key) const { return Find(key) != nullptr; }

    // Typed convenience readers with defaults, for the config mapping.
    int64_t IntField(std::string_view key, int64_t fallback = 0) const;
    double DoubleField(std::string_view key, double fallback = 0.0) const;
    bool BoolField(std::string_view key, bool fallback = false) const;
    std::string StringField(std::string_view key, std::string_view fallback = "") const;

  private:
    Kind kind_ = Kind::kNull;
    bool bool_ = false;
    int64_t int_ = 0;
    double double_ = 0;
    std::string string_;
    std::vector<JsonPtr> array_;
    std::map<std::string, JsonPtr> object_;
};

inline std::ostream& operator<<(std::ostream& os, JsonValue::Kind k) {
    return os << JsonValue::KindName(k);
}

struct JsonLimits {
    // Nesting depth. A recursive-descent parser without this is a stack
    // overflow waiting for enough opening brackets.
    size_t max_depth = 32;
    // Total values parsed, bounding heap use independently of input length.
    size_t max_values = 200000;
    size_t max_string_bytes = 1 << 20;
};

// Parses a complete JSON document. Trailing non-whitespace is an error.
Status JsonParse(std::string_view text, JsonPtr* out, JsonLimits limits = {});

}  // namespace relay
