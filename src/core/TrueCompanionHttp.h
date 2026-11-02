/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Minimal blocking HTTP/1.1 client for talking to a local Ollama server.
 * It is only ever called from worker threads (never the world thread), with
 * connect/total timeouts and a cancellation flag checked while waiting.
 * Plain http:// only; Ollama is expected on localhost or a trusted LAN host.
 */

#ifndef MOD_TRUE_COMPANION_HTTP_H
#define MOD_TRUE_COMPANION_HTTP_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace truecompanion
{
    using CancelToken = std::shared_ptr<std::atomic<bool>>;
    inline CancelToken MakeCancelToken() { return std::make_shared<std::atomic<bool>>(false); }

    struct HttpUrl
    {
        std::string host;
        uint16_t port = 80;
        std::string basePath;   // without trailing slash
        static bool Parse(std::string const& url, HttpUrl& out, std::string* error);
    };

    struct HttpResponse
    {
        enum class Error : uint8_t { None = 0, BadUrl, Resolve, Connect, Timeout, Cancelled, Io, Protocol, TooLarge };
        Error error = Error::None;
        int status = 0;
        std::string body;
        std::string errorMessage;
        bool Ok() const { return error == Error::None && status >= 200 && status < 300; }
    };

    struct HttpOptions
    {
        uint32_t connectTimeoutMs = 3000;
        uint32_t totalTimeoutMs = 45000;
        size_t maxBodyBytes = 4 * 1024 * 1024;
        CancelToken cancel;
    };

    HttpResponse HttpRequest(std::string const& method, std::string const& baseUrl, std::string const& path,
                             std::string const& body, HttpOptions const& opt);
}

#endif
