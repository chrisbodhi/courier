#ifndef COURIER_JSON_IN_PLACE_H
#define COURIER_JSON_IN_PLACE_H

// In-place handling of large top-level JSON string values, for
// Client::dispatchJSON.
//
// ArduinoJson 7 has no zero-copy mode: every string is copied into the
// document, grown by doubling as it parses. A 4.6 KB string field (a pushed
// app's source) therefore needs one contiguous 8 KB block mid-parse, on top
// of the receive buffer that already holds it — on a no-PSRAM board with a
// fragmented heap, that block is often not there and the message is dropped.
//
// Instead: find the large top-level string values, parse everything else
// with a Filter that skips them (skipping a string allocates nothing), then
// unescape each one in place in the receive buffer (JSON unescaping only
// ever shrinks) and link it into the document without copying.

#include <cstddef>
#include <cstdint>

namespace Courier {
namespace detail {

struct InPlaceString {
    const char* key;     // points into the buffer; NOT NUL-terminated
    size_t keyLen;
    char* value;         // first byte after the opening quote
    size_t rawLen;       // escaped length, up to (not incl.) the closing quote
};

inline const char* jipSkipWs(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// p points at an opening quote. Returns a pointer to the matching closing
// quote, or nullptr if the string is unterminated. Sets *hasEscape.
inline const char* jipScanString(const char* p, const char* end, bool* hasEscape) {
    *hasEscape = false;
    for (p++; p < end; p++) {
        if (*p == '\\') { *hasEscape = true; p++; continue; }
        if (*p == '"') return p;
    }
    return nullptr;
}

// Skips one non-string value (object, array, number, literal). Returns a
// pointer just past it, or nullptr if malformed.
inline const char* jipSkipValue(const char* p, const char* end) {
    int depth = 0;
    bool esc;
    while (p < end) {
        char c = *p;
        if (c == '"') {
            p = jipScanString(p, end, &esc);
            if (!p) return nullptr;
            p++;
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (depth == 0) return p;  // end of the enclosing object
            depth--;
            if (depth == 0) return p + 1;
        } else if (c == ',' && depth == 0) {
            return p;
        }
        p++;
    }
    return depth == 0 ? p : nullptr;
}

// Scans a top-level JSON object and records string members whose escaped
// length is >= minLen. Members with escaped keys are ignored (they parse
// the ordinary way). Returns the number recorded (at most maxOut); 0 when
// the input is not an object or is malformed — ArduinoJson then reports it.
inline size_t findLargeTopLevelStrings(char* json, size_t len, size_t minLen,
                                       InPlaceString* out, size_t maxOut) {
    const char* p = json;
    const char* end = json + len;
    size_t n = 0;
    p = jipSkipWs(p, end);
    if (p >= end || *p != '{') return 0;
    p = jipSkipWs(p + 1, end);
    if (p < end && *p == '}') return 0;
    while (p < end) {
        if (*p != '"') return 0;
        bool keyEsc;
        const char* keyEnd = jipScanString(p, end, &keyEsc);
        if (!keyEnd) return 0;
        const char* key = p + 1;
        p = jipSkipWs(keyEnd + 1, end);
        if (p >= end || *p != ':') return 0;
        p = jipSkipWs(p + 1, end);
        if (p >= end) return 0;
        if (*p == '"') {
            bool valEsc;
            const char* valEnd = jipScanString(p, end, &valEsc);
            if (!valEnd) return 0;
            size_t rawLen = (size_t)(valEnd - (p + 1));
            if (rawLen >= minLen && !keyEsc && n < maxOut) {
                out[n].key = key;
                out[n].keyLen = (size_t)(keyEnd - key);
                out[n].value = json + (p + 1 - json);
                out[n].rawLen = rawLen;
                n++;
            }
            p = valEnd + 1;
        } else {
            p = jipSkipValue(p, end);
            if (!p) return 0;
        }
        p = jipSkipWs(p, end);
        if (p >= end) return 0;
        if (*p == '}') return n;
        if (*p != ',') return 0;
        p = jipSkipWs(p + 1, end);
    }
    return 0;
}

inline int jipHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool jipReadU16(const char* s, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = jipHex(s[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

// Unescapes a JSON string body of rawLen escaped bytes. With write=false it
// only validates (the buffer is untouched); with write=true it rewrites in
// place and NUL-terminates. Returns the decoded length, or SIZE_MAX if the
// escapes are invalid or decode to a NUL (not representable as a C string).
inline size_t unescapeJsonStringInPlace(char* s, size_t rawLen, bool write) {
    size_t r = 0, w = 0;
    while (r < rawLen) {
        char c = s[r++];
        if (c != '\\') {
            if (write) s[w] = c;
            w++;
            continue;
        }
        if (r >= rawLen) return SIZE_MAX;
        char e = s[r++];
        char out;
        switch (e) {
            case '"': out = '"'; break;
            case '\\': out = '\\'; break;
            case '/': out = '/'; break;
            case 'b': out = '\b'; break;
            case 'f': out = '\f'; break;
            case 'n': out = '\n'; break;
            case 'r': out = '\r'; break;
            case 't': out = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (r + 4 > rawLen || !jipReadU16(s + r, &cp)) return SIZE_MAX;
                r += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (r + 6 > rawLen || s[r] != '\\' || s[r + 1] != 'u' ||
                        !jipReadU16(s + r + 2, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return SIZE_MAX;
                    r += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return SIZE_MAX;
                }
                if (cp == 0) return SIZE_MAX;
                // UTF-8 is never longer than the \uXXXX (or surrogate pair)
                // escape it replaces, so w stays behind r.
                uint8_t buf[4];
                size_t k;
                if (cp < 0x80) { buf[0] = (uint8_t)cp; k = 1; }
                else if (cp < 0x800) {
                    buf[0] = (uint8_t)(0xC0 | (cp >> 6));
                    buf[1] = (uint8_t)(0x80 | (cp & 0x3F)); k = 2;
                } else if (cp < 0x10000) {
                    buf[0] = (uint8_t)(0xE0 | (cp >> 12));
                    buf[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                    buf[2] = (uint8_t)(0x80 | (cp & 0x3F)); k = 3;
                } else {
                    buf[0] = (uint8_t)(0xF0 | (cp >> 18));
                    buf[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                    buf[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                    buf[3] = (uint8_t)(0x80 | (cp & 0x3F)); k = 4;
                }
                for (size_t i = 0; i < k; i++) {
                    if (write) s[w] = (char)buf[i];
                    w++;
                }
                continue;
            }
            default:
                return SIZE_MAX;
        }
        if (write) s[w] = out;
        w++;
    }
    if (write) s[w] = '\0';
    return w;
}

}  // namespace detail
}  // namespace Courier

#endif  // COURIER_JSON_IN_PLACE_H
