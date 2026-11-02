/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * LLM backend abstraction (Ollama in production, scripted mock in tests)
 * and the bounded worker pool that runs requests off the game thread.
 */

#ifndef MOD_TRUE_COMPANION_LLM_H
#define MOD_TRUE_COMPANION_LLM_H

#include "TrueCompanionHttp.h"
#include "TrueCompanionJson.h"
#include "TrueCompanionPrompt.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace truecompanion
{
    struct LlmRequest
    {
        std::string model;
        std::string fallbackModel;
        std::vector<ChatMessage> messages;
        std::optional<Json> schema;     // structured output ("format")
        double temperature = 0.8;
        uint32_t numCtx = 8192;
        uint32_t numPredict = 300;
        bool think = false;
        std::string keepAlive = "10m";
        uint32_t timeoutMs = 45000;
        uint32_t connectTimeoutMs = 3000;
    };

    struct LlmResult
    {
        enum class Status : uint8_t { Ok = 0, Offline, Timeout, Cancelled, ModelMissing, HttpError, BadResponse };
        Status status = Status::BadResponse;
        std::string content;
        std::string modelUsed;
        std::string error;
        uint32_t latencyMs = 0;
        uint32_t evalCount = 0;
        bool Ok() const { return status == Status::Ok; }
    };
    char const* LlmStatusId(LlmResult::Status s);

    class ILlmBackend
    {
    public:
        virtual ~ILlmBackend() = default;
        virtual LlmResult Chat(LlmRequest const& req, CancelToken const& cancel) = 0;
        // Returns the installed model names, or nullopt when the server is unreachable.
        virtual std::optional<std::vector<std::string>> ListModels(uint32_t timeoutMs) = 0;
        virtual std::optional<std::string> Version(uint32_t timeoutMs) = 0;
    };

    class OllamaBackend : public ILlmBackend
    {
    public:
        explicit OllamaBackend(std::string baseUrl) : _baseUrl(std::move(baseUrl)) { }
        LlmResult Chat(LlmRequest const& req, CancelToken const& cancel) override;
        std::optional<std::vector<std::string>> ListModels(uint32_t timeoutMs) override;
        std::optional<std::string> Version(uint32_t timeoutMs) override;

        static std::string BuildChatBody(LlmRequest const& req, std::string const& model, bool includeThink, bool includeFormat);

    private:
        LlmResult ChatOnce(LlmRequest const& req, std::string const& model, bool includeThink, bool includeFormat,
                           CancelToken const& cancel);
        std::string _baseUrl;
    };

    // Fixed-size thread pool with a bounded queue. Submit() never blocks: when the
    // queue is full the job is rejected (backpressure) and the caller degrades gracefully.
    class WorkerPool
    {
    public:
        using Job = std::function<void()>;

        WorkerPool(size_t threads, size_t maxQueue);
        ~WorkerPool();
        WorkerPool(WorkerPool const&) = delete;
        WorkerPool& operator=(WorkerPool const&) = delete;

        bool Submit(Job job);
        void Shutdown();               // stops accepting, drops queued jobs, joins threads
        size_t Queued() const;
        size_t Busy() const;
        size_t Capacity() const { return _maxQueue; }

    private:
        void Run();

        mutable std::mutex _mutex;
        std::condition_variable _cv;
        std::deque<Job> _queue;
        std::vector<std::thread> _threads;
        size_t _maxQueue;
        size_t _busy = 0;
        bool _stopping = false;
    };

    // Thread-safe hand-off of completed work back to the game thread.
    template <typename T>
    class ResultQueue
    {
    public:
        void Push(T v)
        {
            std::lock_guard<std::mutex> g(_m);
            _q.push_back(std::move(v));
        }
        std::vector<T> Drain()
        {
            std::lock_guard<std::mutex> g(_m);
            std::vector<T> out(std::make_move_iterator(_q.begin()), std::make_move_iterator(_q.end()));
            _q.clear();
            return out;
        }
        size_t Size() const
        {
            std::lock_guard<std::mutex> g(_m);
            return _q.size();
        }

    private:
        mutable std::mutex _m;
        std::deque<T> _q;
    };
}

#endif
