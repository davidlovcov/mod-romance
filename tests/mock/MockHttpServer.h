/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Tiny single-threaded HTTP server on 127.0.0.1 that impersonates Ollama for
 * network-level tests of HttpRequest/OllamaBackend (status codes, chunked
 * bodies, slow responses for timeout tests). POSIX only.
 */

#ifndef MOD_TRUE_COMPANION_MOCK_HTTP_SERVER_H
#define MOD_TRUE_COMPANION_MOCK_HTTP_SERVER_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace truecompanion::test
{
    struct MockHttpRequest
    {
        std::string method, path, body;
    };

    struct MockHttpResponse
    {
        int status = 200;
        std::string body;
        bool chunked = false;
        std::string chunkedHeader = "Transfer-Encoding: chunked";   // exact header line, to test odd spacing/case
        int delayMs = 0;
        std::string raw;     // if set, sent verbatim instead of a generated response (malformed-HTTP tests)
    };

    class MockHttpServer
    {
    public:
        using Handler = std::function<MockHttpResponse(MockHttpRequest const&)>;

        explicit MockHttpServer(Handler h) : _handler(std::move(h))
        {
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            ::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            socklen_t len = sizeof(addr);
            getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &len);
            _port = ntohs(addr.sin_port);
            ::listen(_fd, 16);
            _thread = std::thread([this]() { Run(); });
        }

        ~MockHttpServer()
        {
            _stop = true;
            if (_thread.joinable())
                _thread.join();
            ::close(_fd);
        }

        std::string Url() const { return "http://127.0.0.1:" + std::to_string(_port); }

        std::vector<MockHttpRequest> Requests()
        {
            std::lock_guard<std::mutex> g(_m);
            return _requests;
        }

    private:
        void Run()
        {
            while (!_stop)
            {
                pollfd p{ _fd, POLLIN, 0 };
                if (poll(&p, 1, 50) <= 0)
                    continue;
                int c = ::accept(_fd, nullptr, nullptr);
                if (c < 0)
                    continue;
                Serve(c);
                ::close(c);
            }
        }

        void Serve(int c)
        {
            std::string raw;
            char buf[8192];
            size_t headerEnd = std::string::npos;
            size_t need = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                pollfd p{ c, POLLIN, 0 };
                if (poll(&p, 1, 50) <= 0)
                {
                    if (headerEnd != std::string::npos && raw.size() >= headerEnd + 4 + need)
                        break;
                    continue;
                }
                ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                raw.append(buf, size_t(n));
                if (headerEnd == std::string::npos && (headerEnd = raw.find("\r\n\r\n")) != std::string::npos)
                {
                    size_t cl = raw.find("Content-Length: ");
                    if (cl != std::string::npos && cl < headerEnd)
                        need = size_t(std::stoul(raw.substr(cl + 16)));
                }
                if (headerEnd != std::string::npos && raw.size() >= headerEnd + 4 + need)
                    break;
            }
            MockHttpRequest req;
            size_t sp1 = raw.find(' '), sp2 = raw.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos)
            {
                req.method = raw.substr(0, sp1);
                req.path = raw.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            if (headerEnd != std::string::npos)
                req.body = raw.substr(headerEnd + 4);
            {
                std::lock_guard<std::mutex> g(_m);
                _requests.push_back(req);
            }
            MockHttpResponse resp = _handler(req);
            for (int waited = 0; waited < resp.delayMs && !_stop; waited += 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!resp.raw.empty())
            {
                ::send(c, resp.raw.data(), resp.raw.size(), MSG_NOSIGNAL);
                return;
            }
            std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " OK\r\nContent-Type: application/json\r\nConnection: close\r\n";
            if (resp.chunked)
            {
                out += resp.chunkedHeader + "\r\n\r\n";
                size_t half = resp.body.size() / 2;
                std::string a = resp.body.substr(0, half), b = resp.body.substr(half);
                char hex[16];
                snprintf(hex, sizeof(hex), "%zx", a.size());
                out += std::string(hex) + "\r\n" + a + "\r\n";
                snprintf(hex, sizeof(hex), "%zx", b.size());
                out += std::string(hex) + "\r\n" + b + "\r\n0\r\n\r\n";
            }
            else
                out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n\r\n" + resp.body;
            ::send(c, out.data(), out.size(), MSG_NOSIGNAL);
        }

        Handler _handler;
        int _fd = -1;
        uint16_t _port = 0;
        std::atomic<bool> _stop{ false };
        std::thread _thread;
        std::mutex _m;
        std::vector<MockHttpRequest> _requests;
    };
}

#endif
