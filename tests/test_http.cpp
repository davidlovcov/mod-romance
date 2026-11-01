/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 * Network-level tests: real sockets against a local fake Ollama server.
 */

#include "TestFramework.h"

#include "MockHttpServer.h"
#include "RomanceLlm.h"

#include <chrono>
#include <string>
#include <thread>

using namespace romance;
using namespace romance::test;

namespace
{
    std::string ChatOk(std::string const& content)
    {
        Json msg = Json::MakeObject();
        msg.Set("role", "assistant");
        msg.Set("content", content);
        Json j = Json::MakeObject();
        j.Set("model", "qwen3.5:4b");
        j.Set("message", msg);
        j.Set("done", true);
        j.Set("eval_count", 42);
        return j.Dump();
    }

    LlmRequest BasicRequest()
    {
        LlmRequest r;
        r.model = "qwen3.5:4b";
        r.fallbackModel = "qwen3:4b-instruct";
        r.messages = { { "system", "You are Elara." }, { "user", "Hi!" } };
        r.schema = DialogueContextBuilder::ReplySchema();
        r.timeoutMs = 3000;
        r.connectTimeoutMs = 1000;
        return r;
    }

    uint16_t FreePort()
    {
        MockHttpServer tmp([](MockHttpRequest const&) { return MockHttpResponse{}; });
        std::string u = tmp.Url();
        return uint16_t(std::stoi(u.substr(u.rfind(':') + 1)));
    }
}

TEST(Http, UrlParsing)
{
    HttpUrl u;
    CHECK(HttpUrl::Parse("http://127.0.0.1:11434", u, nullptr));
    CHECK_EQ(u.host, std::string("127.0.0.1"));
    CHECK_EQ(u.port, uint16_t(11434));
    CHECK(HttpUrl::Parse("http://ollama.lan/proxy/", u, nullptr));
    CHECK_EQ(u.port, uint16_t(80));
    CHECK_EQ(u.basePath, std::string("/proxy"));
    std::string err;
    CHECK(!HttpUrl::Parse("https://example.com", u, &err));
    CHECK(!HttpUrl::Parse("http://host:99999", u, &err));
}

TEST(Http, OllamaChatSendsStructuredNonThinkingRequest)
{
    MockHttpServer server([](MockHttpRequest const& r)
    {
        MockHttpResponse resp;
        if (r.path == "/api/chat")
            resp.body = ChatOk("{\"reply\":\"Hello!\"}");
        else
            resp.status = 404;
        return resp;
    });
    OllamaBackend backend(server.Url());
    LlmResult r = backend.Chat(BasicRequest(), MakeCancelToken());
    CHECK(r.Ok());
    CHECK_EQ(r.content, std::string("{\"reply\":\"Hello!\"}"));
    CHECK_EQ(r.evalCount, 42u);
    auto reqs = server.Requests();
    CHECK_EQ(reqs.size(), size_t(1));
    auto body = Json::Parse(reqs[0].body);
    CHECK(body.has_value());
    CHECK_EQ(body->GetString("model"), std::string("qwen3.5:4b"));
    CHECK(!body->GetBool("stream", true));
    CHECK(body->Has("think") && !body->GetBool("think", true));
    CHECK((*body)["format"].IsObject());
    CHECK_EQ((*body)["messages"].Size(), size_t(2));
    CHECK((*body)["options"].Has("num_ctx"));
}

TEST(Http, ChunkedResponseIsDecoded)
{
    MockHttpServer server([](MockHttpRequest const&)
    {
        MockHttpResponse resp;
        resp.body = ChatOk("chunked works");
        resp.chunked = true;
        return resp;
    });
    OllamaBackend backend(server.Url());
    LlmResult r = backend.Chat(BasicRequest(), MakeCancelToken());
    CHECK(r.Ok());
    CHECK_EQ(r.content, std::string("chunked works"));
}

TEST(Http, OfflineServerFailsFastWithoutThrowing)
{
    uint16_t port = FreePort();   // server destroyed -> nothing listens
    OllamaBackend backend("http://127.0.0.1:" + std::to_string(port));
    auto t0 = std::chrono::steady_clock::now();
    LlmResult r = backend.Chat(BasicRequest(), MakeCancelToken());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.status == LlmResult::Status::Offline);
    CHECK(ms < 2000);
    CHECK(!backend.Version(500).has_value());
    CHECK(!backend.ListModels(500).has_value());
}

TEST(Http, TimeoutAndCancellation)
{
    MockHttpServer server([](MockHttpRequest const&)
    {
        MockHttpResponse resp;
        resp.body = ChatOk("too late");
        resp.delayMs = 1500;
        return resp;
    });
    OllamaBackend backend(server.Url());
    LlmRequest req = BasicRequest();
    req.timeoutMs = 300;
    auto t0 = std::chrono::steady_clock::now();
    LlmResult r = backend.Chat(req, MakeCancelToken());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.status == LlmResult::Status::Timeout);
    CHECK(ms < 1000);

    CancelToken tok = MakeCancelToken();
    std::thread canceller([tok]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        tok->store(true);
    });
    req.timeoutMs = 5000;
    t0 = std::chrono::steady_clock::now();
    r = backend.Chat(req, tok);
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    canceller.join();
    CHECK(r.status == LlmResult::Status::Cancelled);
    CHECK(ms < 1000);
}

TEST(Http, MissingModelFallsBackAndUnsupportedThinkRetries)
{
    MockHttpServer server([](MockHttpRequest const& r)
    {
        MockHttpResponse resp;
        auto body = Json::Parse(r.body);
        std::string model = body ? body->GetString("model") : "";
        if (model == "qwen3.5:4b")
        {
            resp.status = 404;
            resp.body = "{\"error\":\"model 'qwen3.5:4b' not found\"}";
        }
        else if (body && body->Has("think"))
        {
            resp.status = 400;
            resp.body = "{\"error\":\"\\\"qwen3:4b-instruct\\\" does not support thinking\"}";
        }
        else
            resp.body = ChatOk("fallback ok");
        return resp;
    });
    OllamaBackend backend(server.Url());
    LlmResult r = backend.Chat(BasicRequest(), MakeCancelToken());
    CHECK(r.Ok());
    CHECK_EQ(r.modelUsed, std::string("qwen3:4b-instruct"));
    CHECK_EQ(r.content, std::string("fallback ok"));
    CHECK_EQ(server.Requests().size(), size_t(3));
}

TEST(Http, TagsAndVersion)
{
    MockHttpServer server([](MockHttpRequest const& r)
    {
        MockHttpResponse resp;
        if (r.path == "/api/version")
            resp.body = "{\"version\":\"0.12.3\"}";
        else if (r.path == "/api/tags")
            resp.body = "{\"models\":[{\"name\":\"qwen3.5:4b\"},{\"name\":\"llama3.2:3b\"}]}";
        else
            resp.status = 404;
        return resp;
    });
    OllamaBackend backend(server.Url());
    CHECK_EQ(backend.Version(1000).value_or(""), std::string("0.12.3"));
    auto models = backend.ListModels(1000);
    CHECK(models.has_value());
    CHECK_EQ(models->size(), size_t(2));
}

TEST(Http, ErrorBodyAndGarbage)
{
    MockHttpServer server([](MockHttpRequest const&)
    {
        MockHttpResponse resp;
        resp.body = "this is not json";
        return resp;
    });
    OllamaBackend backend(server.Url());
    LlmResult r = backend.Chat(BasicRequest(), MakeCancelToken());
    CHECK(r.status == LlmResult::Status::BadResponse);
}
