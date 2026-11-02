/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionLlm.h"

#include <chrono>

namespace truecompanion
{
    char const* LlmStatusId(LlmResult::Status s)
    {
        switch (s)
        {
            case LlmResult::Status::Ok: return "ok";
            case LlmResult::Status::Offline: return "offline";
            case LlmResult::Status::Timeout: return "timeout";
            case LlmResult::Status::Cancelled: return "cancelled";
            case LlmResult::Status::ModelMissing: return "model_missing";
            case LlmResult::Status::HttpError: return "http_error";
            case LlmResult::Status::BadResponse: return "bad_response";
        }
        return "unknown";
    }

    std::string OllamaBackend::BuildChatBody(LlmRequest const& req, std::string const& model, bool includeThink, bool includeFormat)
    {
        Json body = Json::MakeObject();
        body.Set("model", model);
        Json msgs = Json::MakeArray();
        for (auto const& m : req.messages)
        {
            Json jm = Json::MakeObject();
            jm.Set("role", m.role);
            jm.Set("content", m.content);
            msgs.Push(jm);
        }
        body.Set("messages", msgs);
        body.Set("stream", false);
        if (includeThink)
            body.Set("think", req.think);
        if (includeFormat && req.schema)
            body.Set("format", *req.schema);
        Json options = Json::MakeObject();
        options.Set("temperature", req.temperature);
        options.Set("num_ctx", int64_t(req.numCtx));
        options.Set("num_predict", int64_t(req.numPredict));
        options.Set("repeat_penalty", 1.12);
        body.Set("options", options);
        if (!req.keepAlive.empty())
            body.Set("keep_alive", req.keepAlive);
        return body.Dump();
    }

    LlmResult OllamaBackend::ChatOnce(LlmRequest const& req, std::string const& model, bool includeThink, bool includeFormat,
                                      CancelToken const& cancel)
    {
        LlmResult r;
        r.modelUsed = model;
        auto start = std::chrono::steady_clock::now();
        HttpOptions opt;
        opt.connectTimeoutMs = req.connectTimeoutMs;
        opt.totalTimeoutMs = req.timeoutMs;
        opt.cancel = cancel;
        HttpResponse h = HttpRequest("POST", _baseUrl, "/api/chat", BuildChatBody(req, model, includeThink, includeFormat), opt);
        r.latencyMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());

        switch (h.error)
        {
            case HttpResponse::Error::None: break;
            case HttpResponse::Error::Timeout: r.status = LlmResult::Status::Timeout; r.error = h.errorMessage; return r;
            case HttpResponse::Error::Cancelled: r.status = LlmResult::Status::Cancelled; r.error = "cancelled"; return r;
            case HttpResponse::Error::Connect:
            case HttpResponse::Error::Resolve:
            case HttpResponse::Error::BadUrl:
                r.status = LlmResult::Status::Offline; r.error = h.errorMessage; return r;
            default: r.status = LlmResult::Status::HttpError; r.error = h.errorMessage; return r;
        }

        auto j = Json::Parse(h.body, nullptr, Json::ParseLimits{ 4 * 1024 * 1024, 16 });
        if (h.status < 200 || h.status >= 300)
        {
            std::string err = j ? j->GetString("error", h.body) : h.body;
            r.error = LimitUtf8("HTTP " + std::to_string(h.status) + ": " + err, 300);
            r.status = (h.status == 404 || err.find("not found") != std::string::npos) ? LlmResult::Status::ModelMissing
                                                                                       : LlmResult::Status::HttpError;
            return r;
        }
        if (!j || !j->IsObject())
        {
            r.status = LlmResult::Status::BadResponse;
            r.error = "response is not JSON";
            return r;
        }
        if (j->Has("error"))
        {
            r.status = LlmResult::Status::HttpError;
            r.error = LimitUtf8(j->GetString("error"), 300);
            return r;
        }
        r.content = (*j)["message"].GetString("content");
        r.evalCount = uint32_t(std::max<int64_t>(0, j->GetInt("eval_count", 0)));
        if (r.content.empty())
        {
            r.status = LlmResult::Status::BadResponse;
            r.error = "empty message content";
            return r;
        }
        r.status = LlmResult::Status::Ok;
        return r;
    }

    LlmResult OllamaBackend::Chat(LlmRequest const& req, CancelToken const& cancel)
    {
        bool think = true, format = true;
        std::string model = req.model;
        LlmResult r;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            r = ChatOnce(req, model, think, format, cancel);
            if (r.Ok() || r.status == LlmResult::Status::Cancelled || r.status == LlmResult::Status::Offline ||
                r.status == LlmResult::Status::Timeout)
                return r;
            // Older servers / models without thinking or schema support: retry without those fields.
            if (r.status == LlmResult::Status::HttpError && think && r.error.find("think") != std::string::npos)
            {
                think = false;
                continue;
            }
            if (r.status == LlmResult::Status::HttpError && format && r.error.find("format") != std::string::npos)
            {
                format = false;
                continue;
            }
            if (r.status == LlmResult::Status::ModelMissing && !req.fallbackModel.empty() && model != req.fallbackModel)
            {
                model = req.fallbackModel;
                continue;
            }
            return r;
        }
        return r;
    }

    std::optional<std::vector<std::string>> OllamaBackend::ListModels(uint32_t timeoutMs)
    {
        HttpOptions opt;
        opt.connectTimeoutMs = std::min<uint32_t>(timeoutMs, 3000);
        opt.totalTimeoutMs = timeoutMs;
        HttpResponse h = HttpRequest("GET", _baseUrl, "/api/tags", "", opt);
        if (!h.Ok())
            return std::nullopt;
        auto j = Json::Parse(h.body);
        if (!j)
            return std::nullopt;
        std::vector<std::string> names;
        for (Json const& m : (*j)["models"].AsArray())
        {
            std::string n = m.GetString("name", m.GetString("model"));
            if (!n.empty())
                names.push_back(n);
        }
        return names;
    }

    std::optional<std::string> OllamaBackend::Version(uint32_t timeoutMs)
    {
        HttpOptions opt;
        opt.connectTimeoutMs = std::min<uint32_t>(timeoutMs, 3000);
        opt.totalTimeoutMs = timeoutMs;
        HttpResponse h = HttpRequest("GET", _baseUrl, "/api/version", "", opt);
        if (!h.Ok())
            return std::nullopt;
        auto j = Json::Parse(h.body);
        return j ? j->GetString("version", "unknown") : std::string("unknown");
    }

    // ---------------------------------------------------------------- WorkerPool

    WorkerPool::WorkerPool(size_t threads, size_t maxQueue) : _maxQueue(std::max<size_t>(1, maxQueue))
    {
        threads = std::max<size_t>(1, std::min<size_t>(threads, 16));
        for (size_t i = 0; i < threads; ++i)
            _threads.emplace_back([this]() { Run(); });
    }

    WorkerPool::~WorkerPool() { Shutdown(); }

    bool WorkerPool::Submit(Job job)
    {
        {
            std::lock_guard<std::mutex> g(_mutex);
            if (_stopping || _queue.size() >= _maxQueue)
                return false;
            _queue.push_back(std::move(job));
        }
        _cv.notify_one();
        return true;
    }

    void WorkerPool::Shutdown()
    {
        {
            std::lock_guard<std::mutex> g(_mutex);
            if (_stopping && _threads.empty())
                return;
            _stopping = true;
            _queue.clear();
        }
        _cv.notify_all();
        for (auto& t : _threads)
            if (t.joinable())
                t.join();
        _threads.clear();
    }

    size_t WorkerPool::Queued() const
    {
        std::lock_guard<std::mutex> g(_mutex);
        return _queue.size();
    }

    size_t WorkerPool::Busy() const
    {
        std::lock_guard<std::mutex> g(_mutex);
        return _busy;
    }

    void WorkerPool::Run()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lk(_mutex);
                _cv.wait(lk, [this]() { return _stopping || !_queue.empty(); });
                if (_stopping)
                    return;
                job = std::move(_queue.front());
                _queue.pop_front();
                ++_busy;
            }
            try
            {
                job();
            }
            catch (...)
            {
                // A job must never take down a worker (or the worldserver).
            }
            {
                std::lock_guard<std::mutex> g(_mutex);
                --_busy;
            }
        }
    }
}
