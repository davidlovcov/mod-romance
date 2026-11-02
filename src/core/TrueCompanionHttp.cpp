/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionHttp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socket_t = SOCKET;
#  define TRUE_COMPANION_INVALID_SOCKET INVALID_SOCKET
#  define TRUE_COMPANION_POLL WSAPoll
#  define TRUE_COMPANION_CLOSE closesocket
#else
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
   using socket_t = int;
#  define TRUE_COMPANION_INVALID_SOCKET (-1)
#  define TRUE_COMPANION_POLL poll
#  define TRUE_COMPANION_CLOSE ::close
#endif

namespace truecompanion
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

#ifdef _WIN32
        void EnsureWinsock()
        {
            static std::once_flag once;
            std::call_once(once, []()
            {
                WSADATA data;
                WSAStartup(MAKEWORD(2, 2), &data);
            });
        }
        bool WouldBlock() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
        void SetNonBlocking(socket_t s) { u_long mode = 1; ioctlsocket(s, FIONBIO, &mode); }
#else
        void EnsureWinsock() { }
        bool WouldBlock() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
        void SetNonBlocking(socket_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

        struct SocketGuard
        {
            socket_t s = TRUE_COMPANION_INVALID_SOCKET;
            ~SocketGuard() { if (s != TRUE_COMPANION_INVALID_SOCKET) TRUE_COMPANION_CLOSE(s); }
        };

        int RemainingMs(Clock::time_point deadline)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            return int(std::max<int64_t>(0, ms));
        }

        // Waits for `events` in short slices so a cancellation is noticed quickly.
        // Returns 1 ready, 0 timeout, -1 error, -2 cancelled.
        int WaitFor(socket_t s, short events, Clock::time_point deadline, CancelToken const& cancel)
        {
            for (;;)
            {
                if (cancel && cancel->load())
                    return -2;
                int left = RemainingMs(deadline);
                if (left <= 0)
                    return 0;
                pollfd pfd{};
                pfd.fd = s;
                pfd.events = events;
                int r = TRUE_COMPANION_POLL(&pfd, 1, std::min(left, 100));
                if (r < 0)
                {
#ifndef _WIN32
                    if (errno == EINTR)
                        continue;
#endif
                    return -1;
                }
                if (r > 0)
                {
                    if (pfd.revents & (POLLERR | POLLNVAL))
                        return -1;
                    return 1;
                }
            }
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Value of header `name` (lower-case) in a lower-cased header block, without surrounding blanks; "" if absent.
        std::string HeaderValue(std::string const& headers, std::string const& name)
        {
            size_t at = headers.find("\r\n" + name + ":");
            if (at == std::string::npos)
                return "";
            size_t start = at + 2 + name.size() + 1;
            size_t end = headers.find("\r\n", start);
            std::string v = headers.substr(start, end == std::string::npos ? std::string::npos : end - start);
            size_t a = v.find_first_not_of(" \t");
            size_t b = v.find_last_not_of(" \t");
            return a == std::string::npos ? "" : v.substr(a, b - a + 1);
        }

        bool DecodeChunked(std::string const& in, std::string& out)
        {
            size_t p = 0;
            out.clear();
            while (p < in.size())
            {
                size_t eol = in.find("\r\n", p);
                if (eol == std::string::npos)
                    return false;
                std::string sizeHex = in.substr(p, eol - p);
                size_t semi = sizeHex.find(';');
                if (semi != std::string::npos)
                    sizeHex.resize(semi);
                char* end = nullptr;
                unsigned long n = std::strtoul(sizeHex.c_str(), &end, 16);
                if (end == sizeHex.c_str())
                    return false;
                p = eol + 2;
                if (n == 0)
                    return true;
                if (n > in.size() - p)   // also rejects absurd sizes that would overflow p + n
                    return false;
                out.append(in, p, n);
                p += n + 2; // skip CRLF
            }
            return false;
        }
    }

    bool HttpUrl::Parse(std::string const& url, HttpUrl& out, std::string* error)
    {
        out = HttpUrl{};
        std::string const scheme = "http://";
        if (url.compare(0, scheme.size(), scheme) != 0)
        {
            if (error)
                *error = "only http:// URLs are supported (use a local reverse proxy for TLS)";
            return false;
        }
        std::string rest = url.substr(scheme.size());
        size_t slash = rest.find('/');
        std::string hostPort = rest.substr(0, slash);
        out.basePath = slash == std::string::npos ? "" : rest.substr(slash);
        while (!out.basePath.empty() && out.basePath.back() == '/')
            out.basePath.pop_back();
        std::string portText;
        if (!hostPort.empty() && hostPort[0] == '[')
        {
            // IPv6 literal: http://[::1]:11434
            size_t close = hostPort.find(']');
            if (close == std::string::npos)
            {
                if (error)
                    *error = "invalid IPv6 address";
                return false;
            }
            out.host = hostPort.substr(1, close - 1);
            std::string rest2 = hostPort.substr(close + 1);
            if (!rest2.empty())
            {
                if (rest2[0] != ':')
                {
                    if (error)
                        *error = "invalid host";
                    return false;
                }
                portText = rest2.substr(1);
                if (portText.empty())
                {
                    if (error)
                        *error = "invalid port";
                    return false;
                }
            }
        }
        else
        {
            size_t colon = hostPort.rfind(':');
            if (colon != std::string::npos)
            {
                portText = hostPort.substr(colon + 1);
                out.host = hostPort.substr(0, colon);
                if (portText.empty())
                {
                    if (error)
                        *error = "invalid port";
                    return false;
                }
            }
            else
                out.host = hostPort;
        }
        if (!portText.empty())
        {
            int port = 0;
            for (char c : portText)
            {
                if (c < '0' || c > '9' || port > 65535)
                {
                    port = -1;
                    break;
                }
                port = port * 10 + (c - '0');
            }
            if (port <= 0 || port > 65535)
            {
                if (error)
                    *error = "invalid port";
                return false;
            }
            out.port = uint16_t(port);
        }
        if (out.host.empty() || out.host.find_first_of(" \r\n\t") != std::string::npos)
        {
            if (error)
                *error = "invalid host";
            return false;
        }
        return true;
    }

    HttpResponse HttpRequest(std::string const& method, std::string const& baseUrl, std::string const& path,
                             std::string const& body, HttpOptions const& opt)
    {
        HttpResponse resp;
        HttpUrl url;
        if (!HttpUrl::Parse(baseUrl, url, &resp.errorMessage))
        {
            resp.error = HttpResponse::Error::BadUrl;
            return resp;
        }
        EnsureWinsock();
        auto start = Clock::now();
        auto totalDeadline = start + std::chrono::milliseconds(opt.totalTimeoutMs);
        auto connectDeadline = std::min(totalDeadline, start + std::chrono::milliseconds(opt.connectTimeoutMs));

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string portStr = std::to_string(url.port);
        if (getaddrinfo(url.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        {
            resp.error = HttpResponse::Error::Resolve;
            resp.errorMessage = "cannot resolve host " + url.host;
            return resp;
        }

        SocketGuard sock;
        std::string lastErr = "connection refused";
        for (addrinfo* ai = res; ai; ai = ai->ai_next)
        {
            socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == TRUE_COMPANION_INVALID_SOCKET)
                continue;
            SetNonBlocking(s);
            int c = ::connect(s, ai->ai_addr, socklen_t(ai->ai_addrlen));
            if (c == 0 || WouldBlock())
            {
                int w = c == 0 ? 1 : WaitFor(s, POLLOUT, connectDeadline, opt.cancel);
                if (w == 1)
                {
                    int soErr = 0;
                    socklen_t len = sizeof(soErr);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
                    if (soErr == 0)
                    {
                        sock.s = s;
                        break;
                    }
                }
                else if (w == -2)
                {
                    TRUE_COMPANION_CLOSE(s);
                    freeaddrinfo(res);
                    resp.error = HttpResponse::Error::Cancelled;
                    resp.errorMessage = "cancelled";
                    return resp;
                }
                else if (w == 0)
                    lastErr = "connect timeout";
            }
            TRUE_COMPANION_CLOSE(s);
        }
        freeaddrinfo(res);
        if (sock.s == TRUE_COMPANION_INVALID_SOCKET)
        {
            resp.error = lastErr == "connect timeout" ? HttpResponse::Error::Timeout : HttpResponse::Error::Connect;
            resp.errorMessage = lastErr + " (" + url.host + ":" + portStr + ")";
            return resp;
        }

        std::string req = method + " " + url.basePath + path + " HTTP/1.1\r\n";
        bool ipv6 = url.host.find(':') != std::string::npos;
        req += "Host: " + (ipv6 ? "[" + url.host + "]" : url.host) + ":" + portStr + "\r\n";
        req += "User-Agent: mod-true-companion/1.0\r\n";
        req += "Accept: application/json\r\n";
        req += "Connection: close\r\n";
        if (!body.empty() || method == "POST")
        {
            req += "Content-Type: application/json\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "\r\n";
        req += body;

        size_t sent = 0;
        while (sent < req.size())
        {
            int w = WaitFor(sock.s, POLLOUT, totalDeadline, opt.cancel);
            if (w != 1)
            {
                resp.error = w == -2 ? HttpResponse::Error::Cancelled : w == 0 ? HttpResponse::Error::Timeout : HttpResponse::Error::Io;
                resp.errorMessage = "send failed";
                return resp;
            }
#ifdef _WIN32
            int n = ::send(sock.s, req.data() + sent, int(req.size() - sent), 0);
#else
            ssize_t n = ::send(sock.s, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
#endif
            if (n <= 0)
            {
                if (n < 0 && WouldBlock())
                    continue;
                resp.error = HttpResponse::Error::Io;
                resp.errorMessage = "send failed";
                return resp;
            }
            sent += size_t(n);
        }

        std::string raw;
        size_t headerEnd = std::string::npos;
        long long contentLength = -1;
        bool chunked = false;
        char buf[16384];
        for (;;)
        {
            if (headerEnd != std::string::npos && contentLength >= 0 &&
                raw.size() >= headerEnd + 4 + size_t(contentLength))
                break;
            if (headerEnd != std::string::npos && chunked && raw.find("\r\n0\r\n", headerEnd) != std::string::npos &&
                raw.size() >= 4 && raw.compare(raw.size() - 4, 4, "\r\n\r\n") == 0)
                break;
            int w = WaitFor(sock.s, POLLIN, totalDeadline, opt.cancel);
            if (w != 1)
            {
                resp.error = w == -2 ? HttpResponse::Error::Cancelled : w == 0 ? HttpResponse::Error::Timeout : HttpResponse::Error::Io;
                resp.errorMessage = w == 0 ? "request timed out" : w == -2 ? "cancelled" : "receive failed";
                return resp;
            }
#ifdef _WIN32
            int n = ::recv(sock.s, buf, int(sizeof(buf)), 0);
#else
            ssize_t n = ::recv(sock.s, buf, sizeof(buf), 0);
#endif
            if (n == 0)
                break; // connection closed
            if (n < 0)
            {
                if (WouldBlock())
                    continue;
                resp.error = HttpResponse::Error::Io;
                resp.errorMessage = "receive failed";
                return resp;
            }
            raw.append(buf, size_t(n));
            if (raw.size() > opt.maxBodyBytes + 64 * 1024)
            {
                resp.error = HttpResponse::Error::TooLarge;
                resp.errorMessage = "response too large";
                return resp;
            }
            if (headerEnd == std::string::npos)
            {
                headerEnd = raw.find("\r\n\r\n");
                if (headerEnd != std::string::npos)
                {
                    std::string headers = Lower(raw.substr(0, headerEnd));
                    std::string cl = HeaderValue(headers, "content-length");
                    if (!cl.empty() && cl.find_first_not_of("0123456789") == std::string::npos && cl.size() <= 12)
                        contentLength = std::atoll(cl.c_str());   // digits only: a bad value means "read until close"
                    chunked = HeaderValue(headers, "transfer-encoding").find("chunked") != std::string::npos;
                }
                else if (raw.size() > 64 * 1024)
                {
                    resp.error = HttpResponse::Error::Protocol;
                    resp.errorMessage = "headers too large";
                    return resp;
                }
            }
        }

        if (headerEnd == std::string::npos || raw.compare(0, 5, "HTTP/") != 0)
        {
            resp.error = HttpResponse::Error::Protocol;
            resp.errorMessage = "malformed HTTP response";
            return resp;
        }
        size_t sp = raw.find(' ');
        resp.status = sp != std::string::npos ? std::atoi(raw.c_str() + sp + 1) : 0;
        std::string payload = raw.substr(headerEnd + 4);
        if (chunked)
        {
            if (!DecodeChunked(payload, resp.body))
            {
                resp.error = HttpResponse::Error::Protocol;
                resp.errorMessage = "bad chunked encoding";
                return resp;
            }
        }
        else if (contentLength >= 0)
            resp.body = payload.substr(0, size_t(std::min<long long>(contentLength, (long long)payload.size())));
        else
            resp.body = payload;
        return resp;
    }
}
