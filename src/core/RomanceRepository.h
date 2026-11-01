/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Persistence interface. The AzerothCore build uses RomanceSqlRepository
 * (characters database, async writes); tests use InMemoryRepository, which
 * also simulates a server restart by surviving the service object.
 */

#ifndef MOD_ROMANCE_REPOSITORY_H
#define MOD_ROMANCE_REPOSITORY_H

#include "RomancePresets.h"
#include "RomanceTypes.h"

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace romance
{
    struct CompanionRecord
    {
        CompanionKey key;
        uint32_t ownerAccount = 0;
        std::string companionName;
        PersonalityProfile personality;
        RelationshipState relationship;
        CompanionSettings settings;
        bool configured = false;      // player saved a personality at least once
        uint32_t turnsSinceSummary = 0;
        TimePoint createdAt = 0;
        TimePoint updatedAt = 0;
    };

    struct GiftRecord
    {
        uint64_t itemGuid = 0;
        uint32_t entry = 0;
        uint32_t count = 1;
        double score = 0;
        std::string category;
        std::string name;
        TimePoint at = 0;
    };

    class IRomanceRepository
    {
    public:
        virtual ~IRomanceRepository() = default;

        virtual std::optional<CompanionRecord> LoadCompanion(CompanionKey const& key) = 0;
        virtual std::vector<CompanionRecord> LoadCompanionsForOwner(Guid owner) = 0;
        virtual void SaveCompanion(CompanionRecord const& rec) = 0;
        virtual void DeleteCompanion(CompanionKey const& key) = 0;

        // owner == 0 addresses the account-level default profile
        virtual std::optional<PlayerProfile> LoadPlayerProfile(uint32_t account, Guid owner) = 0;
        virtual void SavePlayerProfile(uint32_t account, Guid owner, PlayerProfile const& p) = 0;

        virtual std::vector<MemoryEntry> LoadMemories(CompanionKey const& key) = 0;
        virtual void SaveMemory(CompanionKey const& key, MemoryEntry const& m) = 0;   // upsert
        virtual void DeleteMemories(CompanionKey const& key, std::vector<uint32_t> const& ids) = 0;
        virtual void DeleteAllMemories(CompanionKey const& key) = 0;

        virtual std::vector<ChatTurn> LoadRecentChat(CompanionKey const& key, uint32_t limit) = 0;
        virtual void AppendChat(CompanionKey const& key, ChatTurn const& t) = 0;
        virtual void PruneChat(CompanionKey const& key, uint32_t keep) = 0;
        virtual void ClearChat(CompanionKey const& key) = 0;

        virtual std::vector<GiftRecord> LoadGifts(CompanionKey const& key, TimePoint since) = 0;
        // Item instance guids already rewarded for this companion (from any owner): replay protection.
        virtual std::set<uint64_t> LoadRewardedItemGuids(Guid companion) = 0;
        virtual void InsertGift(CompanionKey const& key, GiftRecord const& g) = 0;

        virtual void LogRelationshipEvent(CompanionKey const& key, std::string const& type, std::string const& detail, TimePoint at) = 0;

        // Custom presets belong to an account; `number` is the n of "custom:<n>".
        virtual std::vector<PersonalityPreset> LoadCustomPresets(uint32_t account) = 0;
        virtual void SaveCustomPreset(uint32_t account, uint32_t number, PersonalityPreset const& p) = 0; // upsert
        virtual void DeleteCustomPreset(uint32_t account, uint32_t number) = 0;
    };

    // Thread-safe in-process repository (tests, and a fallback if the DB layer is disabled).
    class InMemoryRepository : public IRomanceRepository
    {
    public:
        std::optional<CompanionRecord> LoadCompanion(CompanionKey const& key) override;
        std::vector<CompanionRecord> LoadCompanionsForOwner(Guid owner) override;
        void SaveCompanion(CompanionRecord const& rec) override;
        void DeleteCompanion(CompanionKey const& key) override;
        std::optional<PlayerProfile> LoadPlayerProfile(uint32_t account, Guid owner) override;
        void SavePlayerProfile(uint32_t account, Guid owner, PlayerProfile const& p) override;
        std::vector<MemoryEntry> LoadMemories(CompanionKey const& key) override;
        void SaveMemory(CompanionKey const& key, MemoryEntry const& m) override;
        void DeleteMemories(CompanionKey const& key, std::vector<uint32_t> const& ids) override;
        void DeleteAllMemories(CompanionKey const& key) override;
        std::vector<ChatTurn> LoadRecentChat(CompanionKey const& key, uint32_t limit) override;
        void AppendChat(CompanionKey const& key, ChatTurn const& t) override;
        void PruneChat(CompanionKey const& key, uint32_t keep) override;
        void ClearChat(CompanionKey const& key) override;
        std::vector<GiftRecord> LoadGifts(CompanionKey const& key, TimePoint since) override;
        std::set<uint64_t> LoadRewardedItemGuids(Guid companion) override;
        void InsertGift(CompanionKey const& key, GiftRecord const& g) override;
        void LogRelationshipEvent(CompanionKey const& key, std::string const& type, std::string const& detail, TimePoint at) override;
        std::vector<PersonalityPreset> LoadCustomPresets(uint32_t account) override;
        void SaveCustomPreset(uint32_t account, uint32_t number, PersonalityPreset const& p) override;
        void DeleteCustomPreset(uint32_t account, uint32_t number) override;

        struct EventRow { CompanionKey key; std::string type, detail; TimePoint at; };
        std::vector<EventRow> Events() const;
        size_t WriteCount() const;

    private:
        mutable std::mutex _m;
        std::map<CompanionKey, CompanionRecord> _companions;
        std::map<std::pair<uint32_t, Guid>, PlayerProfile> _profiles;
        std::map<CompanionKey, std::map<uint32_t, MemoryEntry>> _memories;
        std::map<CompanionKey, std::vector<ChatTurn>> _chat;
        std::map<CompanionKey, std::vector<GiftRecord>> _gifts;
        std::vector<EventRow> _events;
        std::map<uint32_t, std::map<uint32_t, PersonalityPreset>> _presets;
        size_t _writes = 0;
    };
}

#endif
