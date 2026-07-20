#pragma once

/* Minimal ESPAsyncWebServer mock for native tests.
 *
 * Faithful where it matters to HttpMCPServer:
 *  - _tempObject is released with free() in the request destructor, exactly
 *    like the real library — so an aborted upload exercises the same teardown
 *    path (a C++ object stored there would leak / corrupt the heap).
 *  - Header lookup is case-insensitive.
 *  - Handlers are registered per (uri, method); tests fetch them via
 *    mock_async_web::lastServer() and drive body/request callbacks manually.
 */

#include "WString.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

enum WebRequestMethod : int {
    HTTP_GET = 0x01,
    HTTP_POST = 0x02,
    HTTP_DELETE = 0x04,
    HTTP_PUT = 0x08,
    HTTP_PATCH = 0x10,
    HTTP_HEAD = 0x20,
    HTTP_OPTIONS = 0x40,
    HTTP_ANY = 0x7F,
};

class AsyncWebServerRequest;

class AsyncWebHeader {
public:
    explicit AsyncWebHeader(const char* value) : value_(value) {}
    const String& value() const { return value_; }

private:
    String value_;
};

class AsyncWebServerResponse {
public:
    AsyncWebServerResponse(int code, const String& contentType, const String& body)
        : code(code), contentType(contentType.c_str()), body(body.c_str()) {}

    void addHeader(const String& name, const String& value) {
        headers[name.c_str()] = value.c_str();
    }

    int code;
    std::string contentType;
    std::string body;
    std::map<std::string, std::string> headers;
};

class AsyncWebServerRequest {
public:
    AsyncWebServerRequest() = default;
    AsyncWebServerRequest(const AsyncWebServerRequest&) = delete;
    AsyncWebServerRequest& operator=(const AsyncWebServerRequest&) = delete;

    ~AsyncWebServerRequest() {
        /* Mirrors ~AsyncWebServerRequest in the real library: _tempObject is
         * released with free(), destructors are NOT run. */
        if (_tempObject) {
            free(_tempObject);
            _tempObject = nullptr;
        }
    }

    /* ---- API consumed by HttpMCPServer ---- */

    bool hasHeader(const String& name) const {
        return headers_.count(lower(name.c_str())) > 0;
    }

    AsyncWebHeader* getHeader(const String& name) {
        auto it = headers_.find(lower(name.c_str()));
        return it == headers_.end() ? nullptr : &it->second;
    }

    size_t contentLength() const { return contentLength_; }

    const String& contentType() const { return contentType_; }

    AsyncWebServerResponse* beginResponse(int code, const String& contentType, const String& body) {
        return new AsyncWebServerResponse(code, contentType, body);
    }

    void send(int code) { record(code, "", "", {}); }

    void send(int code, const String& contentType, const String& body) {
        record(code, contentType.c_str(), body.c_str(), {});
    }

    void send(AsyncWebServerResponse* response) {
        record(response->code, response->contentType, response->body, response->headers);
        delete response;
    }

    void* _tempObject = nullptr;

    /* ---- test-side setup & inspection ---- */

    void setHeader(const char* name, const char* value) {
        headers_.emplace(lower(name), AsyncWebHeader(value));
    }

    void setContentLength(size_t length) { contentLength_ = length; }

    void setContentType(const char* type) { contentType_ = type; }

    int responseCount = 0;
    int lastCode = -1;
    std::string lastContentType;
    std::string lastBody;
    std::map<std::string, std::string> lastHeaders;

private:
    static std::string lower(const char* s) {
        std::string out(s ? s : "");
        for (char& c : out) {
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    void record(int code, std::string contentType, std::string body, std::map<std::string, std::string> headers) {
        responseCount++;
        lastCode = code;
        lastContentType = std::move(contentType);
        lastBody = std::move(body);
        lastHeaders = std::move(headers);
    }

    std::map<std::string, AsyncWebHeader> headers_;
    size_t contentLength_ = 0;
    String contentType_ = String("application/json");
};

using ArRequestHandlerFunction = std::function<void(AsyncWebServerRequest*)>;
using ArUploadHandlerFunction =
    std::function<void(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool)>;
using ArBodyHandlerFunction = std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)>;

class AsyncWebServer;

namespace mock_async_web {
/* The AsyncWebServer most recently constructed — HttpMCPServer allocates its
 * server internally, so tests reach it through this hook. */
inline AsyncWebServer*& lastServer() {
    static AsyncWebServer* server = nullptr;
    return server;
}
}  // namespace mock_async_web

class AsyncWebServer {
public:
    struct Route {
        std::string uri;
        int method;
        ArRequestHandlerFunction onRequest;
        ArBodyHandlerFunction onBody;
    };

    explicit AsyncWebServer(uint16_t port) : port(port) {
        mock_async_web::lastServer() = this;
    }

    ~AsyncWebServer() {
        if (mock_async_web::lastServer() == this) {
            mock_async_web::lastServer() = nullptr;
        }
    }

    void on(const char* uri, int method, ArRequestHandlerFunction onRequest) {
        routes.push_back({uri, method, std::move(onRequest), nullptr});
    }

    void on(const char* uri, int method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload,
            ArBodyHandlerFunction onBody) {
        (void)onUpload;
        routes.push_back({uri, method, std::move(onRequest), std::move(onBody)});
    }

    void onNotFound(ArRequestHandlerFunction handler) { notFound = std::move(handler); }

    void begin() { started = true; }

    const Route* findRoute(const char* uri, int method) const {
        for (const auto& route : routes) {
            if (route.uri == uri && (route.method & method) != 0) {
                return &route;
            }
        }
        return nullptr;
    }

    uint16_t port;
    bool started = false;
    std::vector<Route> routes;
    ArRequestHandlerFunction notFound;
};
