/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * IRomanceRepository on the AzerothCore characters database.
 *  - reads are synchronous and only happen when a companion is activated,
 *    configured or inspected (small, indexed queries);
 *  - frequent writes (companion state, player profile, chat log) are asynchronous
 *    (DatabaseWorkerPool::Execute), idempotent upserts, so the world thread never
 *    waits on MySQL during gameplay. Because async writes and sync reads use
 *    different connections, saved companion records and player profiles are kept
 *    in a write-through cache: a load right after a save always sees the save;
 *  - rare, player-triggered writes (memory edits, deletes, clears) run directly
 *    so the next read cannot see the old rows.
 * Must be used from the world thread only (like the RomanceService).
 */

#ifndef MOD_ROMANCE_SQL_REPOSITORY_H
#define MOD_ROMANCE_SQL_REPOSITORY_H

#include "RomanceRepository.h"

#include <map>
#include <utility>

namespace romance::ac
{
    class RomanceSqlRepository : public IRomanceRepository
    {
    public:
        // Verifies the tables exist; logs a clear error if the SQL was not applied.
        bool CheckSchema();

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

    private:
        using PairKey = std::pair<uint64_t, uint64_t>;
        static PairKey K(CompanionKey const& k) { return { uint64_t(k.owner), uint64_t(k.companion) }; }

        std::map<PairKey, CompanionRecord> _companions; // last saved state per companion
        std::map<PairKey, PlayerProfile> _profiles;      // (account, owner) -> last saved profile
    };
}

#endif
