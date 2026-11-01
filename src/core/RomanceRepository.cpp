/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceRepository.h"

#include <algorithm>

namespace romance
{
    std::optional<CompanionRecord> InMemoryRepository::LoadCompanion(CompanionKey const& key)
    {
        std::lock_guard<std::mutex> g(_m);
        auto it = _companions.find(key);
        if (it == _companions.end())
            return std::nullopt;
        return it->second;
    }

    std::vector<CompanionRecord> InMemoryRepository::LoadCompanionsForOwner(Guid owner)
    {
        std::lock_guard<std::mutex> g(_m);
        std::vector<CompanionRecord> out;
        for (auto const& [k, v] : _companions)
            if (k.owner == owner)
                out.push_back(v);
        return out;
    }

    void InMemoryRepository::SaveCompanion(CompanionRecord const& rec)
    {
        std::lock_guard<std::mutex> g(_m);
        _companions[rec.key] = rec;
        ++_writes;
    }

    void InMemoryRepository::DeleteCompanion(CompanionKey const& key)
    {
        std::lock_guard<std::mutex> g(_m);
        _companions.erase(key);
        _memories.erase(key);
        _chat.erase(key);
        // the gift log is kept on purpose: it is the replay/farming protection
        ++_writes;
    }

    std::optional<PlayerProfile> InMemoryRepository::LoadPlayerProfile(uint32_t account, Guid owner)
    {
        std::lock_guard<std::mutex> g(_m);
        auto it = _profiles.find({ account, owner });
        if (it == _profiles.end())
            return std::nullopt;
        return it->second;
    }

    void InMemoryRepository::SavePlayerProfile(uint32_t account, Guid owner, PlayerProfile const& p)
    {
        std::lock_guard<std::mutex> g(_m);
        _profiles[{ account, owner }] = p;
        ++_writes;
    }

    std::vector<MemoryEntry> InMemoryRepository::LoadMemories(CompanionKey const& key)
    {
        std::lock_guard<std::mutex> g(_m);
        std::vector<MemoryEntry> out;
        auto it = _memories.find(key);
        if (it != _memories.end())
            for (auto const& [id, m] : it->second)
                out.push_back(m);
        return out;
    }

    void InMemoryRepository::SaveMemory(CompanionKey const& key, MemoryEntry const& m)
    {
        std::lock_guard<std::mutex> g(_m);
        _memories[key][m.id] = m;
        ++_writes;
    }

    void InMemoryRepository::DeleteMemories(CompanionKey const& key, std::vector<uint32_t> const& ids)
    {
        std::lock_guard<std::mutex> g(_m);
        auto it = _memories.find(key);
        if (it == _memories.end())
            return;
        for (uint32_t id : ids)
            it->second.erase(id);
        ++_writes;
    }

    void InMemoryRepository::DeleteAllMemories(CompanionKey const& key)
    {
        std::lock_guard<std::mutex> g(_m);
        _memories.erase(key);
        ++_writes;
    }

    std::vector<ChatTurn> InMemoryRepository::LoadRecentChat(CompanionKey const& key, uint32_t limit)
    {
        std::lock_guard<std::mutex> g(_m);
        auto it = _chat.find(key);
        if (it == _chat.end())
            return {};
        auto const& v = it->second;
        size_t start = v.size() > limit ? v.size() - limit : 0;
        return std::vector<ChatTurn>(v.begin() + long(start), v.end());
    }

    void InMemoryRepository::AppendChat(CompanionKey const& key, ChatTurn const& t)
    {
        std::lock_guard<std::mutex> g(_m);
        _chat[key].push_back(t);
        ++_writes;
    }

    void InMemoryRepository::PruneChat(CompanionKey const& key, uint32_t keep)
    {
        std::lock_guard<std::mutex> g(_m);
        auto& v = _chat[key];
        if (v.size() > keep)
            v.erase(v.begin(), v.begin() + long(v.size() - keep));
    }

    void InMemoryRepository::ClearChat(CompanionKey const& key)
    {
        std::lock_guard<std::mutex> g(_m);
        _chat.erase(key);
    }

    std::vector<GiftRecord> InMemoryRepository::LoadGifts(CompanionKey const& key, TimePoint since)
    {
        std::lock_guard<std::mutex> g(_m);
        std::vector<GiftRecord> out;
        auto it = _gifts.find(key);
        if (it != _gifts.end())
            for (auto const& r : it->second)
                if (r.at >= since)
                    out.push_back(r);
        return out;
    }

    std::set<uint64_t> InMemoryRepository::LoadRewardedItemGuids(Guid companion)
    {
        std::lock_guard<std::mutex> g(_m);
        std::set<uint64_t> out;
        for (auto const& [k, v] : _gifts)
            if (k.companion == companion)
                for (auto const& r : v)
                    if (r.itemGuid)
                        out.insert(r.itemGuid);
        return out;
    }

    void InMemoryRepository::InsertGift(CompanionKey const& key, GiftRecord const& gr)
    {
        std::lock_guard<std::mutex> g(_m);
        _gifts[key].push_back(gr);
        ++_writes;
    }

    void InMemoryRepository::LogRelationshipEvent(CompanionKey const& key, std::string const& type, std::string const& detail, TimePoint at)
    {
        std::lock_guard<std::mutex> g(_m);
        _events.push_back({ key, type, detail, at });
    }

    std::vector<InMemoryRepository::EventRow> InMemoryRepository::Events() const
    {
        std::lock_guard<std::mutex> g(_m);
        return _events;
    }

    size_t InMemoryRepository::WriteCount() const
    {
        std::lock_guard<std::mutex> g(_m);
        return _writes;
    }
}
