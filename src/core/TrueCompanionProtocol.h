/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Addon <-> server wire protocol (see docs/ADDON_PROTOCOL.md).
 *
 * WoW 3.3.5a addon messages are limited to 255 bytes (prefix + tab + body), so
 * JSON messages are split into chunks:   <id>:<seq>/<total>:<data>
 *   id    1..6 base36 chars chosen by the sender
 *   seq   1-based chunk index, total = number of chunks
 * '|' never appears on the wire (JSON escapes it as |) because the client
 * would interpret it as an escape sequence.
 *
 * The sender's identity always comes from the server session, never from the
 * payload, so a spoofed message can only ever act on the sender's own data.
 */

#ifndef MOD_TRUE_COMPANION_PROTOCOL_H
#define MOD_TRUE_COMPANION_PROTOCOL_H

#include "TrueCompanionJson.h"
#include "TrueCompanionTypes.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace truecompanion
{
    constexpr char const* kAddonPrefix = "TCMP";
    constexpr int kProtocolVersion = 1;
    constexpr char const* kModuleVersion = "2.0.0";   // bump together with the addon's AR.version at every release
    constexpr size_t kMaxChunkData = 200;          // keeps prefix + header + data well below 255 bytes
    constexpr size_t kMaxInboundMessageBytes = 16 * 1024;
    constexpr size_t kMaxInboundChunks = 96;
    constexpr size_t kMaxPartialMessages = 4;
    constexpr int64_t kPartialTimeoutMs = 15000;

    // Serializes `msg` and splits it into wire chunks.
    std::vector<std::string> EncodeAddonMessage(Json const& msg, uint32_t messageId);

    class AddonReassembler
    {
    public:
        enum class Status : uint8_t { Incomplete = 0, Complete, Malformed, TooLarge, TooManyPartials };

        struct Result
        {
            Status status = Status::Incomplete;
            Json message;
            std::string error;
        };

        // Feed one chunk body (without prefix). `nowMs` is a monotonic clock in milliseconds.
        Result Feed(std::string const& chunk, int64_t nowMs);
        void Clear() { _partials.clear(); }
        size_t PartialCount() const { return _partials.size(); }

    private:
        struct Partial
        {
            uint32_t total = 0;
            std::vector<std::string> parts;
            std::vector<bool> have;      // received slots (a chunk may be empty, so `parts` can't tell)
            uint32_t received = 0;
            size_t bytes = 0;
            int64_t firstMs = 0;
        };
        void Expire(int64_t nowMs);
        std::map<std::string, Partial> _partials;
    };

    // Token bucket rate limiter for addon traffic.
    class RateLimiter
    {
    public:
        RateLimiter(double perSecond = 12.0, double burst = 40.0) : _rate(perSecond), _burst(burst), _tokens(burst) { }
        bool Allow(int64_t nowMs, double cost = 1.0);

    private:
        double _rate, _burst, _tokens;
        int64_t _last = 0;
    };

    // Character availability states shown in the addon.
    enum class CharacterStatus : uint8_t
    {
        Available = 0,       // offline, can be summoned
        InParty,             // summoned as my companion bot, in my party
        Summoned,            // logged in as my bot but not in my party
        OnlineAsPlayer,      // a real client is logged in on it
        ControlledElsewhere, // bot controlled by someone else / random bot
        Unavailable,         // e.g. limit reached, banned, self, wrong realm
        Offline,             // offline and not eligible right now (e.g. Playerbots disabled)
        AiDisabled,          // configured but AI disabled for it
    };
    char const* CharacterStatusId(CharacterStatus s);
}

#endif
