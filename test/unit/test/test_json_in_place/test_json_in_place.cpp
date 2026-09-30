#include <unity.h>
#include <JsonInPlace.h>
#include <cstring>
#include <string>

using namespace Courier::detail;

static size_t scan(std::string& s, size_t minLen, InPlaceString* out, size_t maxOut) {
    return findLargeTopLevelStrings(&s[0], s.size(), minLen, out, maxOut);
}

static std::string keyOf(const InPlaceString& e) { return std::string(e.key, e.keyLen); }
static std::string rawOf(const InPlaceString& e) { return std::string(e.value, e.rawLen); }

void setUp(void) {}
void tearDown(void) {}

void test_finds_top_level_strings_at_or_over_threshold() {
    std::string s = "{\"type\":\"app\",\"code\":\"abcdef\",\"n\":3}";
    InPlaceString out[4];
    TEST_ASSERT_EQUAL(1, scan(s, 4, out, 4));
    TEST_ASSERT_EQUAL_STRING("code", keyOf(out[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("abcdef", rawOf(out[0]).c_str());
}

void test_skips_nested_values_containing_json_punctuation() {
    std::string s =
        " {\n \"meta\" : {\"a\":\"}],{\\\"\",\"b\":[1,{\"c\":\"xxxxxxxx\"}]} ,"
        " \"arr\":[\"yyyyyyyy\"], \"t\":true, \"z\":null, \"f\":-1.5e3,"
        " \"code\" : \"q\\\"uoted\\\\\" } ";
    InPlaceString out[4];
    // Nested strings over threshold (xxxxxxxx, yyyyyyyy) are NOT candidates.
    TEST_ASSERT_EQUAL(1, scan(s, 6, out, 4));
    TEST_ASSERT_EQUAL_STRING("code", keyOf(out[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("q\\\"uoted\\\\", rawOf(out[0]).c_str());
}

void test_respects_max_out() {
    std::string s = "{\"a\":\"1111\",\"b\":\"2222\",\"c\":\"3333\"}";
    InPlaceString out[2];
    TEST_ASSERT_EQUAL(2, scan(s, 4, out, 2));
    TEST_ASSERT_EQUAL_STRING("a", keyOf(out[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("b", keyOf(out[1]).c_str());
}

void test_ignores_escaped_keys() {
    std::string s = "{\"co\\u0064e\":\"xxxxxxxx\",\"k\":\"yyyyyyyy\"}";
    InPlaceString out[4];
    TEST_ASSERT_EQUAL(1, scan(s, 4, out, 4));
    TEST_ASSERT_EQUAL_STRING("k", keyOf(out[0]).c_str());
}

void test_returns_zero_for_non_objects_and_malformed_input() {
    InPlaceString out[4];
    const char* cases[] = {
        "[\"xxxxxxxx\"]",
        "\"xxxxxxxx\"",
        "{}",
        "{\"a\":\"xxxxxxxx\"",          // unterminated object
        "{\"a\":\"xxxxxxxx",            // unterminated string
        "{\"a\" \"xxxxxxxx\"}",         // missing colon
        "{\"a\":\"xxxxxxxx\" \"b\":1}", // missing comma
        "not json",
        "",
    };
    for (const char* c : cases) {
        std::string s = c;
        TEST_ASSERT_EQUAL_MESSAGE(0, scan(s, 4, out, 4), c);
    }
}

static std::string unescape(const char* raw, size_t* outLen = nullptr) {
    std::string s = raw;
    size_t n = unescapeJsonStringInPlace(&s[0], s.size(), true);
    if (outLen) *outLen = n;
    if (n == SIZE_MAX) return "<invalid>";
    TEST_ASSERT_EQUAL('\0', s[n]);
    return s.substr(0, n);
}

void test_unescape_simple_escapes() {
    TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\be\ff\ng\rh\ti",
        unescape("a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti").c_str());
}

void test_unescape_unicode_to_utf8() {
    TEST_ASSERT_EQUAL_STRING("A", unescape("\\u0041").c_str());
    TEST_ASSERT_EQUAL_STRING("\xC2\xB7", unescape("\\u00b7").c_str());           // middle dot
    TEST_ASSERT_EQUAL_STRING("\xE2\x82\xAC", unescape("\\u20AC").c_str());       // euro
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", unescape("\\ud83d\\ude00").c_str()); // emoji
}

void test_unescape_plain_utf8_passes_through() {
    TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 \xE2\x82\xAC", unescape("caf\xC3\xA9 \xE2\x82\xAC").c_str());
}

void test_unescape_rejects_invalid() {
    const char* bad[] = {
        "trailing\\",
        "\\x",
        "\\u12",
        "\\u12G4",
        "\\ud83d",           // lone high surrogate
        "\\ud83dx\\ude00",   // high surrogate not followed by \u
        "\\ude00",           // lone low surrogate
        "\\ud83d\\u0041",    // high surrogate + non-low
        "\\u0000",           // decodes to NUL
    };
    for (const char* b : bad) {
        size_t n;
        unescape(b, &n);
        TEST_ASSERT_EQUAL_MESSAGE(SIZE_MAX, n, b);
    }
}

void test_validate_only_leaves_buffer_untouched() {
    std::string s = "x\\ny\\u0041";
    std::string before = s;
    TEST_ASSERT_EQUAL(4, unescapeJsonStringInPlace(&s[0], s.size(), false));
    TEST_ASSERT_EQUAL_STRING(before.c_str(), s.c_str());
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_finds_top_level_strings_at_or_over_threshold);
    RUN_TEST(test_skips_nested_values_containing_json_punctuation);
    RUN_TEST(test_respects_max_out);
    RUN_TEST(test_ignores_escaped_keys);
    RUN_TEST(test_returns_zero_for_non_objects_and_malformed_input);
    RUN_TEST(test_unescape_simple_escapes);
    RUN_TEST(test_unescape_unicode_to_utf8);
    RUN_TEST(test_unescape_plain_utf8_passes_through);
    RUN_TEST(test_unescape_rejects_invalid);
    RUN_TEST(test_validate_only_leaves_buffer_untouched);
    return UNITY_END();
}
