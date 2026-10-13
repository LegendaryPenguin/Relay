#include <string>

#include "relay/json.hpp"
#include "testing.hpp"

using namespace relay;

namespace {

JsonPtr Parse(const std::string& text) {
    JsonPtr out;
    Status s = JsonParse(text, &out);
    if (!s.ok()) return nullptr;
    return out;
}

bool Rejected(const std::string& text, JsonLimits limits = {}) {
    JsonPtr out;
    return !JsonParse(text, &out, limits).ok();
}

}  // namespace

RELAY_TEST(scalars) {
    CHECK(Parse("null")->is_null());
    CHECK_EQ(Parse("true")->AsBool(), true);
    CHECK_EQ(Parse("false")->AsBool(), false);
    CHECK_EQ(Parse("42")->AsInt(), int64_t(42));
    CHECK_EQ(Parse("-7")->AsInt(), int64_t(-7));
    CHECK_EQ(Parse("0")->AsInt(), int64_t(0));
    CHECK_EQ(Parse("\"hello\"")->AsString(), std::string("hello"));
}

RELAY_TEST(integers_stay_integers) {
    // Round-tripping a port or a snapshot version through double is how a
    // value silently gains 0.5.
    auto v = Parse("9007199254740993");  // 2^53 + 1, unrepresentable as double
    CHECK_EQ(v->kind(), JsonValue::Kind::kInt);
    CHECK_EQ(v->AsInt(), int64_t(9007199254740993));
}

RELAY_TEST(integral_doubles_read_as_ints) {
    // JSON has one number type, so a producer serialising 3 as 3.0 is
    // legitimate and rejecting it would be pedantry.
    auto v = Parse("3.0");
    CHECK_EQ(v->kind(), JsonValue::Kind::kDouble);
    CHECK_EQ(v->AsInt(), int64_t(3));
    // A genuinely fractional value does not, so a port of 3.5 falls back.
    CHECK_EQ(Parse("3.5")->AsInt(-1), int64_t(-1));
}

RELAY_TEST(doubles) {
    CHECK(Parse("1.5")->AsDouble() > 1.49 && Parse("1.5")->AsDouble() < 1.51);
    CHECK(Parse("1e3")->AsDouble() > 999.9);
    CHECK(Parse("-2.5e-2")->AsDouble() < 0);
}

RELAY_TEST(objects_and_arrays) {
    auto v = Parse(R"({"a":1,"b":[1,2,3],"c":{"d":"x"}})");
    CHECK(v->is_object());
    CHECK_EQ(v->IntField("a"), int64_t(1));
    CHECK_EQ(v->Find("b")->AsArray().size(), size_t(3));
    CHECK_EQ(v->Find("c")->StringField("d"), std::string("x"));
    CHECK_EQ(v->Find("b")->AsArray()[2]->AsInt(), int64_t(3));
}

RELAY_TEST(empty_containers) {
    CHECK(Parse("{}")->is_object());
    CHECK_EQ(Parse("{}")->AsObject().size(), size_t(0));
    CHECK(Parse("[]")->is_array());
    CHECK_EQ(Parse("[]")->AsArray().size(), size_t(0));
}

RELAY_TEST(missing_is_distinguishable_from_null) {
    // Matters for config: a missing field means "use the default", an
    // explicit null is a mistake worth noticing.
    auto v = Parse(R"({"present":null})");
    CHECK(v->Has("present"));
    CHECK(v->Find("present")->is_null());
    CHECK(!v->Has("absent"));
    CHECK(v->Find("absent") == nullptr);
}

RELAY_TEST(field_readers_fall_back_rather_than_throwing) {
    // A config parser that throws on a missing optional field makes every
    // optional field a special case.
    auto v = Parse(R"({"n":5,"s":"x","b":true})");
    CHECK_EQ(v->IntField("missing", 99), int64_t(99));
    CHECK_EQ(v->StringField("missing", "dflt"), std::string("dflt"));
    CHECK_EQ(v->BoolField("missing", true), true);
    CHECK_EQ(v->DoubleField("missing", 1.5), 1.5);
    // And a present field of the wrong type also falls back rather than
    // producing nonsense.
    CHECK_EQ(v->IntField("s", 42), int64_t(42));
}

RELAY_TEST(string_escapes) {
    CHECK_EQ(Parse(R"("a\nb")")->AsString(), std::string("a\nb"));
    CHECK_EQ(Parse(R"("a\tb")")->AsString(), std::string("a\tb"));
    CHECK_EQ(Parse(R"("quote\"inside")")->AsString(), std::string("quote\"inside"));
    CHECK_EQ(Parse(R"("back\\slash")")->AsString(), std::string("back\\slash"));
    CHECK_EQ(Parse(R"("A")")->AsString(), std::string("A"));
    // Multi-byte UTF-8 from a BMP escape.
    CHECK_EQ(Parse(R"("é")")->AsString(), std::string("\xc3\xa9"));
}

RELAY_TEST(surrogate_pairs_decode_to_utf8) {
    // U+1F600, which needs a surrogate pair in JSON and four bytes in UTF-8.
    auto v = Parse(R"("😀")");
    CHECK(v != nullptr);
    CHECK_EQ(v->AsString().size(), size_t(4));
    CHECK_EQ(v->AsString(), std::string("\xf0\x9f\x98\x80"));
}

RELAY_TEST(malformed_documents_are_rejected) {
    const char* bad[] = {
        "",            "{",          "}",            "[",          "]",
        "{\"a\"}",     "{\"a\":}",   "{a:1}",        "[1,]",       "{\"a\":1,}",
        "tru",         "nul",        "01",           "-",          "1.",
        "1e",          "1e+",        ".5",           "\"unterminated",
        "{\"a\":1} x", "[1 2]",      "'single'",     "+1",
    };
    for (const char* text : bad) {
        if (!Rejected(text)) {
            std::fprintf(stderr, "        accepted: %s\n", text);
            CHECK(false);
        }
    }
}

RELAY_TEST(leading_zeros_rejected) {
    // JSON forbids them, and accepting them means disagreeing with the
    // producer about the value.
    CHECK(Rejected("01"));
    CHECK(Rejected("-01"));
    CHECK(Rejected("[00]"));
    // But a bare zero and a zero with a fraction are fine.
    CHECK(Parse("0") != nullptr);
    CHECK(Parse("0.5") != nullptr);
}

RELAY_TEST(raw_control_characters_in_strings_rejected) {
    std::string text = "\"a\nb\"";
    CHECK(Rejected(text));
}

RELAY_TEST(trailing_content_is_an_error) {
    // A truncated response concatenated with the next would otherwise parse
    // as the first document and silently discard the rest.
    CHECK(Rejected("{} {}"));
    CHECK(Rejected("1 2"));
    // Trailing whitespace is fine.
    CHECK(Parse("{}  \n ") != nullptr);
}

RELAY_TEST(depth_limit_prevents_stack_overflow) {
    // A recursive-descent parser without this is a stack overflow waiting for
    // enough opening brackets.
    JsonLimits limits;
    limits.max_depth = 8;
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(Rejected(deep, limits));

    // And a document inside the limit still parses.
    std::string shallow(4, '[');
    shallow += std::string(4, ']');
    JsonPtr out;
    CHECK_OK(JsonParse(shallow, &out, limits));
}

RELAY_TEST(value_limit_bounds_heap_use) {
    JsonLimits limits;
    limits.max_values = 16;
    std::string many = "[";
    for (int i = 0; i < 100; ++i) {
        if (i > 0) many += ",";
        many += "1";
    }
    many += "]";
    CHECK(Rejected(many, limits));
}

RELAY_TEST(string_length_limit) {
    JsonLimits limits;
    limits.max_string_bytes = 16;
    std::string long_string = "\"" + std::string(100, 'a') + "\"";
    CHECK(Rejected(long_string, limits));
}

RELAY_TEST(a_realistic_snapshot_parses) {
    // Guards against the whole file passing because the parser rejects
    // everything.
    const char* snapshot = R"({
      "version": 7,
      "clusters": [
        {"name":"api","endpoints":[
          {"host":"10.0.0.1","port":8080,"weight":1,"health":"healthy","zone":"a"},
          {"host":"10.0.0.2","port":8080,"weight":2,"health":"unhealthy","zone":"b"}
        ]}
      ],
      "routes": [
        {"cluster":"api","path_prefix":"/v1","balancer":"p2c",
         "per_attempt_timeout_ms":500,"overall_deadline_ms":1500,
         "retry":{"max_attempts":2,"jitter":0.5,"budget_ratio":0.1},
         "breaker":{"failure_threshold":3},
         "shedder":{"max_queue_depth":64,"soft_threshold":0.5},
         "rate_limit":{"enabled":true,"per_second":100.0}}
      ]
    })";
    auto v = Parse(snapshot);
    CHECK(v != nullptr);
    CHECK_EQ(v->IntField("version"), int64_t(7));
    CHECK_EQ(v->Find("clusters")->AsArray().size(), size_t(1));
    const JsonValue& cluster = *v->Find("clusters")->AsArray()[0];
    CHECK_EQ(cluster.StringField("name"), std::string("api"));
    CHECK_EQ(cluster.Find("endpoints")->AsArray().size(), size_t(2));
    const JsonValue& route = *v->Find("routes")->AsArray()[0];
    CHECK_EQ(route.StringField("balancer"), std::string("p2c"));
    CHECK_EQ(route.Find("retry")->IntField("max_attempts"), int64_t(2));
    CHECK_EQ(route.Find("rate_limit")->BoolField("enabled"), true);
}

RELAY_TEST(duplicate_keys_are_last_wins) {
    // RFC 8259 leaves this undefined; pinning the behaviour so it is a
    // documented choice rather than an accident.
    auto v = Parse(R"({"a":1,"a":2})");
    CHECK(v != nullptr);
    CHECK_EQ(v->IntField("a"), int64_t(2));
}

RELAY_TEST(whitespace_everywhere) {
    auto v = Parse(" {\n \"a\" : [ 1 , 2 ] \t } \r\n ");
    CHECK(v != nullptr);
    CHECK_EQ(v->Find("a")->AsArray().size(), size_t(2));
}

RELAY_TEST_MAIN()
