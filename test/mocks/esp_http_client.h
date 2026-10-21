#pragma once
// Mock esp_http_client for native unit tests, in the style of the
// esp_websocket_client / mqtt_client mocks. Script responses with
// MockHttpClient::pushScript(); each esp_http_client_perform() consumes one
// step. An empty script serves s_defaultStep (a benign 200-with-Date JSON
// response) so Client-level tests that fetch incidentally keep working.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#ifndef ESP_ERR_NOT_SUPPORTED
#define ESP_ERR_NOT_SUPPORTED 0x106
#endif

#define ESP_ERR_HTTP_BASE         0x7000
#define ESP_ERR_HTTP_MAX_REDIRECT (ESP_ERR_HTTP_BASE + 1)
#define ESP_ERR_HTTP_CONNECT      (ESP_ERR_HTTP_BASE + 2)
#define ESP_ERR_HTTP_FETCH_HEADER (ESP_ERR_HTTP_BASE + 4)
#define ESP_ERR_HTTP_INCOMPLETE_DATA    (ESP_ERR_HTTP_BASE + 12)
#define ESP_ERR_HTTP_REDIRECT_DOWNGRADE (ESP_ERR_HTTP_BASE + 13)

#define DEFAULT_HTTP_BUF_SIZE (512)

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
} esp_http_client_event_id_t;

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
} esp_http_client_method_t;

class MockHttpClient;
typedef MockHttpClient* esp_http_client_handle_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void* data;
    int data_len;
    void* user_data;
    char* header_key;
    char* header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t* evt);

typedef struct {
    const char* url;
    esp_http_client_method_t method;
    int timeout_ms;
    const char* cert_pem;
    esp_err_t (*crt_bundle_attach)(void* conf);
    http_event_handle_cb event_handler;
    void* user_data;
    int buffer_size;
    int buffer_size_tx;
    bool disable_auto_redirect;
} esp_http_client_config_t;

// Native stand-in for the real symbol from esp_crt_bundle.h.
inline esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }

class MockHttpClient {
public:
    // A single intermediate redirect hop fired before the ScriptStep's own
    // (final) status/headers/body — models esp_http_client internally
    // following a 3xx and re-invoking the event handler for the next hop
    // within the same perform() call.
    struct Hop {
        int status = 301;
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<std::string> bodyChunks;
    };

    struct ScriptStep {
        esp_err_t performResult;
        int status;
        long contentLength;  // -2 = derive from total chunk bytes
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<std::string> bodyChunks;
        std::vector<Hop> redirectHops;  // fired, in order, before this step

        ScriptStep() : performResult(ESP_OK), status(200), contentLength(-2) {}
    };

    esp_http_client_config_t config;
    std::string url;
    std::vector<std::pair<std::string, std::string>> requestHeaders;
    std::string postBody;
    bool closed = false;
    int currentStatus = 0;
    long currentContentLength = -1;

    explicit MockHttpClient(const esp_http_client_config_t* cfg) {
        config = *cfg;
        if (cfg->url) url = cfg->url;
        s_lastInstance = this;
        s_lastConfig = *cfg;
        s_instanceCount++;
    }

    esp_err_t perform() {
        s_performCount++;
        ScriptStep step = s_defaultStep;
        if (s_scriptIndex < (int)s_script.size()) step = s_script[s_scriptIndex++];

        if (!requestFitsTxBuffer()) {
            // esp_http_client could not build the request in its transmit
            // buffer: the server never sees a complete request, no events
            // fire, and perform() fails as a transport-class error would.
            s_txOverflowCount++;
            currentStatus = 0;
            currentContentLength = -1;
            return ESP_FAIL;
        }

        if (step.performResult != ESP_OK && step.status < 100) {
            // Transport-class failure: server never reached, no events fire.
            currentStatus = 0;
            currentContentLength = -1;
            return step.performResult;
        }

        for (auto& hop : step.redirectHops) {
            currentStatus = hop.status;
            size_t hopTotal = 0;
            for (auto& c : hop.bodyChunks) hopTotal += c.size();
            currentContentLength = (long)hopTotal;
            for (auto& h : hop.headers) fireHeader(h.first.c_str(), h.second.c_str());
            for (auto& c : hop.bodyChunks) {
                if (closed) break;
                fireData(c.data(), c.size());
            }
            if (closed) return ESP_FAIL;
            // esp_http_client_perform() ends every pass of its redirect loop
            // with ON_FINISH, on IDF 4.4 and 5.x alike.
            fireFinish();
        }

        currentStatus = step.status;
        size_t total = 0;
        for (auto& c : step.bodyChunks) total += c.size();
        currentContentLength =
            (step.contentLength == -2) ? (long)total : step.contentLength;

        for (auto& h : step.headers) fireHeader(h.first.c_str(), h.second.c_str());

        // IDF 4.4 semantics (esp_http_check_response): with
        // disable_auto_redirect set, ANY non-2xx final status makes perform()
        // return ESP_ERR_HTTP_MAX_REDIRECT after headers are parsed, without
        // reading the body. (IDF 5.x instead treats a 3xx as a normal final
        // response.) Model the stricter 4.4 behavior so the transport's
        // classification is exercised against the worst case.
        if (config.disable_auto_redirect && step.status >= 300) {
            return ESP_ERR_HTTP_MAX_REDIRECT;
        }

        for (auto& c : step.bodyChunks) {
            if (closed) break;
            fireData(c.data(), c.size());
        }
        if (closed) return ESP_FAIL;
        if (step.performResult == ESP_OK) fireFinish();
        return step.performResult;
    }

    static void pushScript(const ScriptStep& step) { s_script.push_back(step); }
    static void resetMock() {
        s_script.clear();
        s_scriptIndex = 0;
        s_performCount = 0;
        s_cleanupCount = 0;
        s_instanceCount = 0;
        s_txOverflowCount = 0;
        s_lastInstance = nullptr;
        s_lastConfig = {};
        s_defaultStep = ScriptStep{};
        // Near-future date (real wall clock + 1 day), computed dynamically so
        // it always clears the buildEpoch() floor in
        // Client::syncTimeFromHttpDate() (buildEpoch() reflects the real
        // compile-time clock) while staying well under the 10-year
        // plausibility ceiling added alongside that floor - regardless of
        // when tests actually run. A fixed literal here would eventually
        // fall behind the real clock and start failing builds.
        time_t t = time(nullptr) + 86400;
        char dateBuf[40];
        struct tm tmv;
        gmtime_r(&t, &tmv);
        strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
        s_defaultStep.headers = {{"Content-Type", "application/json"},
                                 {"Date", dateBuf}};
        s_defaultStep.bodyChunks = {"{}"};
    }

    // Models esp_http_client's request writer on IDF 4.4 and 5.x
    // (http_client_prepare_first_line and esp_http_client_request_send in
    // esp_http_client.c, http_header_generate_string in lib/http_header.c).
    // The request line must fit buffer_size_tx (0 = DEFAULT_HTTP_BUF_SIZE).
    // Headers are then written in buffer-sized pieces, the first sharing the
    // buffer with the request line; a header line that cannot fit in an
    // otherwise empty piece leaves the request unfinished. The header list is
    // what IDF builds: User-Agent and Host from init, the caller's headers,
    // set_post_field's default Content-Type, then Content-Length (body or
    // non-GET/HEAD/DELETE).
    bool requestFitsTxBuffer() const {
        static const char* kMethods[] = {"GET", "POST", "PUT", "PATCH",
                                         "DELETE", "HEAD"};
        int buf = config.buffer_size_tx > 0 ? config.buffer_size_tx
                                            : DEFAULT_HTTP_BUF_SIZE;
        std::string host, target = "/";
        size_t scheme = url.find("://");
        size_t hostStart = scheme == std::string::npos ? 0 : scheme + 3;
        size_t slash = url.find('/', hostStart);
        host = url.substr(hostStart, slash == std::string::npos
                                         ? std::string::npos
                                         : slash - hostStart);
        if (slash != std::string::npos) target = url.substr(slash);

        int firstLine = (int)(strlen(kMethods[config.method]) + 1 +
                              target.size() + strlen(" HTTP/1.1\r\n"));
        if (firstLine >= buf) return false;

        std::vector<std::pair<std::string, std::string>> headers = {
            {"User-Agent", "ESP32 HTTP Client/1.0"}, {"Host", host}};
        bool hasContentType = false;
        for (auto& h : requestHeaders) {
            headers.push_back(h);
            if (h.first == "Content-Type") hasContentType = true;
        }
        if (!postBody.empty() && !hasContentType) {
            // esp_http_client_set_post_field's default.
            headers.push_back(
                {"Content-Type", "application/x-www-form-urlencoded"});
        }
        bool lengthRequired = config.method != HTTP_METHOD_GET &&
                              config.method != HTTP_METHOD_HEAD &&
                              config.method != HTTP_METHOD_DELETE;
        if (!postBody.empty() || lengthRequired) {
            headers.push_back(
                {"Content-Length", std::to_string(postBody.size())});
        }

        int avail = buf - firstLine;
        int used = 0;
        for (auto& h : headers) {
            int line = (int)(h.first.size() + h.second.size() + 4);
            if (used + line + 1 > avail - 2) {
                if (used == 0) return false;  // cannot fit even on its own
                avail = buf;                   // next piece, whole buffer
                used = 0;
                if (line + 1 > avail - 2) return false;
            }
            used += line;
        }
        return true;
    }

    static MockHttpClient* lastInstance() { return s_lastInstance; }
    static const esp_http_client_config_t& lastConfig() { return s_lastConfig; }
    static int performCount() { return s_performCount; }
    static int cleanupCount() { return s_cleanupCount; }
    static int instanceCount() { return s_instanceCount; }
    static int txOverflowCount() { return s_txOverflowCount; }

    inline static int s_cleanupCount = 0;

private:
    void fireHeader(const char* k, const char* v) {
        if (!config.event_handler) return;
        esp_http_client_event_t evt = {};
        evt.event_id = HTTP_EVENT_ON_HEADER;
        evt.client = this;
        evt.user_data = config.user_data;
        evt.header_key = const_cast<char*>(k);
        evt.header_value = const_cast<char*>(v);
        config.event_handler(&evt);
    }
    void fireFinish() {
        if (!config.event_handler) return;
        esp_http_client_event_t evt = {};
        evt.event_id = HTTP_EVENT_ON_FINISH;
        evt.client = this;
        evt.user_data = config.user_data;
        config.event_handler(&evt);
    }
    void fireData(const char* d, size_t n) {
        if (!config.event_handler) return;
        esp_http_client_event_t evt = {};
        evt.event_id = HTTP_EVENT_ON_DATA;
        evt.client = this;
        evt.user_data = config.user_data;
        evt.data = const_cast<char*>(d);
        evt.data_len = (int)n;
        config.event_handler(&evt);
    }

    inline static std::vector<ScriptStep> s_script;
    inline static int s_scriptIndex = 0;
    inline static int s_performCount = 0;
    inline static int s_instanceCount = 0;
    inline static int s_txOverflowCount = 0;
    inline static MockHttpClient* s_lastInstance = nullptr;
    inline static esp_http_client_config_t s_lastConfig = {};
    inline static ScriptStep s_defaultStep;
};

inline esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t* config)
{
    return new MockHttpClient(config);
}
inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,
                                            const char* key, const char* value)
{
    c->requestHeaders.push_back({key, value});
    return ESP_OK;
}
inline esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,
                                                const char* data, int len)
{
    c->postBody.assign(data, (size_t)len);
    return ESP_OK;
}
inline esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    return c->perform();
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    return c->currentStatus;
}
inline int64_t esp_http_client_get_content_length(esp_http_client_handle_t c)
{
    return c->currentContentLength;
}
inline esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    c->closed = true;
    return ESP_OK;
}
inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    MockHttpClient::s_cleanupCount++;
    // Note: we intentionally DON'T delete c here, to allow tests to access
    // instance data (postBody, requestHeaders) after cleanup. This is a
    // memory leak in tests, but safe for unit testing.
    (void)c;
    return ESP_OK;
}
