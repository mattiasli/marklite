#include "test.h"
#include "json.h"

TEST(json_parses_objects_arrays_and_scalars) {
    JVal v;
    CHECK(JParse(R"({"a": 1, "b": [true, false, null], "c": "x", "d": -2.5e2})", v));
    CHECK_EQ((int)v.t, (int)JVal::Obj);
    CHECK_EQ(v["a"].Int(), 1LL);
    CHECK_EQ(v["b"][(size_t)0].b, true);
    CHECK_EQ(v["b"][(size_t)1].b, false);
    CHECK(v["b"][(size_t)2].IsNull());
    CHECK_EQ(v["c"].Str_(), std::string("x"));
    CHECK_EQ(v["d"].n, -250.0);
}

TEST(json_missing_keys_and_indexes_are_null) {
    JVal v;
    CHECK(JParse(R"({"a": [1]})", v));
    CHECK(v["nope"].IsNull());
    CHECK(v["a"][(size_t)5].IsNull());
    CHECK(v["a"]["x"].IsNull());
    CHECK(v["nope"]["deeper"][(size_t)0].IsNull());
}

TEST(json_decodes_escapes_and_unicode) {
    JVal v;
    CHECK(JParse(R"(["a\"b\\c\/d\n\t", "\u00e9\u20ac", "\ud83d\ude00"])", v));
    CHECK_EQ(v[(size_t)0].s, std::string("a\"b\\c/d\n\t"));
    CHECK_EQ(v[(size_t)1].s, std::string("\xC3\xA9\xE2\x82\xAC"));   // é and €
    CHECK_EQ(v[(size_t)2].s, std::string("\xF0\x9F\x98\x80"));       // a surrogate pair becomes U+1F600
}

TEST(json_rejects_malformed_input) {
    const char* bad[] = {"", "{", "[1,", R"({"a" 1})", R"("unterminated)", "tru", "[1 2]"};
    for (const char* b : bad) {
        JVal v;
        if (JParse(b, v)) TestFail(__FILE__, __LINE__, std::string("accepted: ") + b);
    }
}

TEST(json_rejects_very_deep_nesting) {
    std::string deep(100000, '[');
    JVal v;
    CHECK(!JParse(deep, v));
}

TEST(json_quote_escapes_control_characters) {
    std::string out;
    JQuote(out, std::string("say \"hi\"\\\n\x01", 11));
    CHECK_EQ(out, std::string(R"("say \"hi\"\\\n\u0001")"));
}

TEST(json_quote_round_trips_through_parse) {
    std::string text = "line 1\r\nline 2\t\"quoted\" \xC3\xA9 \\ end";
    std::string q;
    JQuote(q, text);
    JVal v;
    CHECK(JParse(q, v));
    CHECK_EQ(v.s, text);
}
