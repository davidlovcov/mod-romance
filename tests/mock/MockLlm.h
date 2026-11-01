/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Deterministic, scriptable LLM backend for tests. A responder function maps
 * each request to a result; every request is recorded together with the
 * thread it ran on (used to prove no LLM work happens on the game thread).
 */

#ifndef MOD_ROMANCE_MOCK_LLM_H
#define MOD_ROMANCE_MOCK_LLM_H

#include "RomanceLlm.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace romance::test
{
    inline std::string ReplyJson(std::string const& reply, std::string const& emotion = "happy", std::string const& sentiment = "positive",
                                 std::vector<std::string> const& signals = {}, std::string const& proposalKind = "none",
                                 std::string const& proposalResponse = "none", std::string const& action = "none",
                                 Json memories = Json::MakeArray())
    {
        Json j = Json::MakeObject();
        j.Set("reply", reply);
        j.Set("emotion", emotion);
        j.Set("sentiment", sentiment);
        Json s = Json::MakeArray();
        for (auto const& x : signals)
            s.Push(x);
        j.Set("signals", s);
        Json p = Json::MakeObject();
        p.Set("kind", proposalKind);
        p.Set("response", proposalResponse);
        j.Set("proposal", p);
        j.Set("memories", memories);
        j.Set("action", action);
        return j.Dump();
    }

    class MockLlm : public ILlmBackend
    {
    public:
        using Responder = std::function<LlmResult(LlmRequest const&)>;

        struct Call
        {
            LlmRequest req;
            std::thread::id thread;
        };

        void SetResponder(Responder r)
        {
            std::lock_guard<std::mutex> g(_m);
            _responder = std::move(r);
        }

        void QueueReply(std::string const& content)
        {
            std::lock_guard<std::mutex> g(_m);
            LlmResult r;
            r.status = LlmResult::Status::Ok;
            r.content = content;
            _queue.push_back(r);
        }

        void QueueResult(LlmResult r)
        {
            std::lock_guard<std::mutex> g(_m);
            _queue.push_back(std::move(r));
        }

        void SetDelayMs(int ms) { _delayMs = ms; }
        void SetOnline(bool on) { _online = on; }

        LlmResult Chat(LlmRequest const& req, CancelToken const& cancel) override
        {
            Responder resp;
            LlmResult queued;
            bool haveQueued = false;
            {
                std::lock_guard<std::mutex> g(_m);
                _calls.push_back({ req, std::this_thread::get_id() });
                if (!_queue.empty())
                {
                    queued = _queue.front();
                    _queue.pop_front();
                    haveQueued = true;
                }
                else
                    resp = _responder;
            }
            for (int waited = 0; waited < _delayMs; waited += 5)
            {
                if (cancel && cancel->load())
                {
                    LlmResult c;
                    c.status = LlmResult::Status::Cancelled;
                    return c;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!_online)
            {
                LlmResult r;
                r.status = LlmResult::Status::Offline;
                r.error = "connection refused (mock)";
                return r;
            }
            if (haveQueued)
                return queued;
            if (resp)
                return resp(req);
            LlmResult r;
            r.status = LlmResult::Status::Ok;
            r.content = ReplyJson("Mm-hm.", "relaxed", "neutral");
            return r;
        }

        std::optional<std::vector<std::string>> ListModels(uint32_t) override
        {
            if (!_online)
                return std::nullopt;
            return std::vector<std::string>{ "qwen3.5:4b", "qwen3:4b-instruct" };
        }

        std::optional<std::string> Version(uint32_t) override
        {
            if (!_online)
                return std::nullopt;
            return std::string("0.12.0-mock");
        }

        std::vector<Call> Calls() const
        {
            std::lock_guard<std::mutex> g(_m);
            return _calls;
        }

        size_t CallCount() const
        {
            std::lock_guard<std::mutex> g(_m);
            return _calls.size();
        }

        void ClearCalls()
        {
            std::lock_guard<std::mutex> g(_m);
            _calls.clear();
        }

    private:
        mutable std::mutex _m;
        Responder _responder;
        std::deque<LlmResult> _queue;
        std::vector<Call> _calls;
        std::atomic<int> _delayMs{ 0 };
        std::atomic<bool> _online{ true };
    };
}

#endif
