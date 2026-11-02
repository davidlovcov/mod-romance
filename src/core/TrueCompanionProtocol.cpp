/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionProtocol.h"

#include <algorithm>
#include <cctype>

namespace truecompanion
{
    namespace
    {
        std::string Base36(uint32_t v)
        {
            static char const digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
            std::string s;
            do
            {
                s.push_back(digits[v % 36]);
                v /= 36;
            } while (v && s.size() < 6);
            std::reverse(s.begin(), s.end());
            return s;
        }

        bool ParseUInt(std::string const& s, uint32_t& out)
        {
            if (s.empty() || s.size() > 6)
                return false;
            uint32_t v = 0;
            for (char c : s)
            {
                if (c < '0' || c > '9')
                    return false;
                v = v * 10 + uint32_t(c - '0');
            }
            out = v;
            return true;
        }

        std::string WireSafe(std::string const& json)
        {
            std::string out;
            out.reserve(json.size());
            for (char c : json)
            {
                if (c == '|')
                    out += "\\u007c";
                else
                    out.push_back(c);
            }
            return out;
        }
    }

    std::vector<std::string> EncodeAddonMessage(Json const& msg, uint32_t messageId)
    {
        std::string data = WireSafe(msg.Dump());
        std::vector<std::string> pieces;
        size_t pos = 0;
        while (pos < data.size())
        {
            size_t len = std::min(kMaxChunkData, data.size() - pos);
            // never split a UTF-8 sequence: the client validates chat text
            while (len > 1 && pos + len < data.size() && (static_cast<unsigned char>(data[pos + len]) & 0xC0) == 0x80)
                --len;
            pieces.push_back(data.substr(pos, len));
            pos += len;
        }
        if (pieces.empty())
            pieces.push_back("{}");
        std::string id = Base36(messageId);
        std::vector<std::string> out;
        out.reserve(pieces.size());
        for (size_t i = 0; i < pieces.size(); ++i)
            out.push_back(id + ":" + std::to_string(i + 1) + "/" + std::to_string(pieces.size()) + ":" + pieces[i]);
        return out;
    }

    void AddonReassembler::Expire(int64_t nowMs)
    {
        for (auto it = _partials.begin(); it != _partials.end();)
        {
            if (nowMs - it->second.firstMs > kPartialTimeoutMs)
                it = _partials.erase(it);
            else
                ++it;
        }
    }

    AddonReassembler::Result AddonReassembler::Feed(std::string const& chunk, int64_t nowMs)
    {
        Result r;
        Expire(nowMs);

        size_t c1 = chunk.find(':');
        size_t slash = c1 == std::string::npos ? std::string::npos : chunk.find('/', c1 + 1);
        size_t c2 = slash == std::string::npos ? std::string::npos : chunk.find(':', slash + 1);
        if (c1 == std::string::npos || slash == std::string::npos || c2 == std::string::npos || c1 == 0 || c1 > 6)
        {
            r.status = Status::Malformed;
            r.error = "bad chunk header";
            return r;
        }
        std::string id = chunk.substr(0, c1);
        for (char c : id)
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                r.status = Status::Malformed;
                r.error = "bad message id";
                return r;
            }
        uint32_t seq = 0, total = 0;
        if (!ParseUInt(chunk.substr(c1 + 1, slash - c1 - 1), seq) || !ParseUInt(chunk.substr(slash + 1, c2 - slash - 1), total) ||
            seq == 0 || total == 0 || seq > total)
        {
            r.status = Status::Malformed;
            r.error = "bad sequence";
            return r;
        }
        if (total > kMaxInboundChunks)
        {
            r.status = Status::TooLarge;
            r.error = "too many chunks";
            return r;
        }
        std::string data = chunk.substr(c2 + 1);

        auto it = _partials.find(id);
        if (it == _partials.end())
        {
            if (_partials.size() >= kMaxPartialMessages)
            {
                r.status = Status::TooManyPartials;
                r.error = "too many concurrent messages";
                return r;
            }
            Partial p;
            p.total = total;
            p.parts.resize(total);
            p.have.assign(total, false);
            p.firstMs = nowMs;
            it = _partials.emplace(id, std::move(p)).first;
        }
        Partial& p = it->second;
        if (p.total != total)
        {
            _partials.erase(it);
            r.status = Status::Malformed;
            r.error = "inconsistent chunk count";
            return r;
        }
        std::string& slot = p.parts[seq - 1];
        if (!p.have[seq - 1])
        {
            p.have[seq - 1] = true;
            p.bytes += data.size();
            ++p.received;
        }
        else
            p.bytes = p.bytes - slot.size() + data.size(); // duplicate chunk (also an empty one): last one wins
        slot = data;
        if (p.bytes > kMaxInboundMessageBytes)
        {
            _partials.erase(it);
            r.status = Status::TooLarge;
            r.error = "message too large";
            return r;
        }
        if (p.received < p.total)
            return r;

        std::string full;
        full.reserve(p.bytes);
        for (auto const& part : p.parts)
            full += part;
        _partials.erase(it);

        std::string err;
        auto j = Json::Parse(full, &err, Json::ParseLimits{ kMaxInboundMessageBytes, 12 });
        if (!j || !j->IsObject() || !(*j)["op"].IsString())
        {
            r.status = Status::Malformed;
            r.error = j ? "missing op" : ("invalid json: " + err);
            return r;
        }
        r.status = Status::Complete;
        r.message = std::move(*j);
        return r;
    }

    bool RateLimiter::Allow(int64_t nowMs, double cost)
    {
        if (_last == 0)
            _last = nowMs;
        double elapsed = double(std::max<int64_t>(0, nowMs - _last)) / 1000.0;
        _last = nowMs;
        _tokens = std::min(_burst, _tokens + elapsed * _rate);
        if (_tokens < cost)
            return false;
        _tokens -= cost;
        return true;
    }

    char const* CharacterStatusId(CharacterStatus s)
    {
        switch (s)
        {
            case CharacterStatus::Available: return "available";
            case CharacterStatus::InParty: return "in_party";
            case CharacterStatus::Summoned: return "summoned";
            case CharacterStatus::OnlineAsPlayer: return "online_player";
            case CharacterStatus::ControlledElsewhere: return "controlled_elsewhere";
            case CharacterStatus::Unavailable: return "unavailable";
            case CharacterStatus::Offline: return "offline";
            case CharacterStatus::AiDisabled: return "ai_disabled";
        }
        return "unavailable";
    }
}
