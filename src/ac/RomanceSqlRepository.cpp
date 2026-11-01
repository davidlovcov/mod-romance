/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceSqlRepository.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
#include "Log.h"

#include <algorithm>
#include <sstream>

namespace romance::ac
{
    namespace
    {
        std::string Esc(std::string s)
        {
            CharacterDatabase.EscapeString(s);
            return s;
        }

        CompanionRecord RowToRecord(Field* f)
        {
            CompanionRecord r;
            r.key.owner = f[0].Get<uint32>();
            r.key.companion = f[1].Get<uint32>();
            r.ownerAccount = f[2].Get<uint32>();
            r.companionName = f[3].Get<std::string>();
            r.configured = f[4].Get<uint8>() != 0;
            if (auto j = Json::Parse(f[5].Get<std::string>()))
                r.personality = PersonalityProfile::FromJson(*j);
            if (auto j = Json::Parse(f[6].Get<std::string>()))
                r.relationship = RelationshipState::FromJson(*j);
            if (auto j = Json::Parse(f[7].Get<std::string>()))
                r.settings = CompanionSettings::FromJson(*j);
            r.turnsSinceSummary = f[8].Get<uint32>();
            r.createdAt = f[9].Get<int64>();
            r.updatedAt = f[10].Get<int64>();
            // Stored data is re-validated: a hand-edited row cannot bypass the sanitizer.
            r.personality.Sanitize(4000, 4000);
            return r;
        }

        constexpr char const* kCompanionColumns =
            "owner_guid, companion_guid, owner_account, companion_name, configured, personality, relationship, settings, "
            "turns_since_summary, created_at, updated_at";
    }

    bool RomanceSqlRepository::CheckSchema()
    {
        static char const* const tables[] = { "romance_companion", "romance_player_profile", "romance_memory",
                                              "romance_chat_log", "romance_gift_log", "romance_relationship_log" };
        bool ok = true;
        for (char const* t : tables)
        {
            QueryResult r = CharacterDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", t);
            if (!r || r->Fetch()[0].Get<uint64>() == 0)
            {
                LOG_ERROR("module", "[mod-romance] Table `{}` is missing in the characters database. Apply data/sql/db-characters/base/*.sql "
                                    "(or enable Updates.EnableDatabases for characters). The module is disabled.", t);
                ok = false;
            }
        }
        return ok;
    }

    std::optional<CompanionRecord> RomanceSqlRepository::LoadCompanion(CompanionKey const& key)
    {
        auto cached = _companions.find(K(key));
        if (cached != _companions.end())
            return cached->second;
        QueryResult r = CharacterDatabase.Query("SELECT {} FROM romance_companion WHERE owner_guid = {} AND companion_guid = {}",
                                                kCompanionColumns, uint32(key.owner), uint32(key.companion));
        if (!r)
            return std::nullopt;
        return RowToRecord(r->Fetch());
    }

    std::vector<CompanionRecord> RomanceSqlRepository::LoadCompanionsForOwner(Guid owner)
    {
        std::vector<CompanionRecord> out;
        QueryResult r = CharacterDatabase.Query("SELECT {} FROM romance_companion WHERE owner_guid = {}", kCompanionColumns, uint32(owner));
        if (!r)
            return out;
        do
            out.push_back(RowToRecord(r->Fetch()));
        while (r->NextRow());
        // Overlay records whose latest save may not have reached MySQL yet.
        for (auto& rec : out)
        {
            auto cached = _companions.find(K(rec.key));
            if (cached != _companions.end())
                rec = cached->second;
        }
        for (auto const& [k, rec] : _companions)
            if (k.first == uint64_t(owner) &&
                std::none_of(out.begin(), out.end(), [&](CompanionRecord const& o) { return K(o.key) == k; }))
                out.push_back(rec);
        return out;
    }

    void RomanceSqlRepository::SaveCompanion(CompanionRecord const& rec)
    {
        _companions[K(rec.key)] = rec;
        CharacterDatabase.Execute(
            "INSERT INTO romance_companion ({}, stage) VALUES ({}, {}, {}, '{}', {}, '{}', '{}', '{}', {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE owner_account = VALUES(owner_account), companion_name = VALUES(companion_name), "
            "configured = VALUES(configured), personality = VALUES(personality), relationship = VALUES(relationship), "
            "settings = VALUES(settings), turns_since_summary = VALUES(turns_since_summary), updated_at = VALUES(updated_at), "
            "stage = VALUES(stage)",
            kCompanionColumns, uint32(rec.key.owner), uint32(rec.key.companion), rec.ownerAccount, Esc(LimitUtf8(rec.companionName, 12)),
            rec.configured ? 1 : 0, Esc(rec.personality.ToJson().Dump()), Esc(rec.relationship.ToJson().Dump()),
            Esc(rec.settings.ToJson().Dump()), rec.turnsSinceSummary, int64(rec.createdAt), int64(rec.updatedAt),
            uint32(rec.relationship.stage));
    }

    void RomanceSqlRepository::DeleteCompanion(CompanionKey const& key)
    {
        // The gift log is intentionally kept: it is the replay/farming protection.
        _companions.erase(K(key));
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM romance_companion WHERE owner_guid = {} AND companion_guid = {}", uint32(key.owner), uint32(key.companion));
        trans->Append("DELETE FROM romance_memory WHERE owner_guid = {} AND companion_guid = {}", uint32(key.owner), uint32(key.companion));
        trans->Append("DELETE FROM romance_chat_log WHERE owner_guid = {} AND companion_guid = {}", uint32(key.owner), uint32(key.companion));
        CharacterDatabase.DirectCommitTransaction(trans);
    }

    std::optional<PlayerProfile> RomanceSqlRepository::LoadPlayerProfile(uint32_t account, Guid owner)
    {
        auto cached = _profiles.find({ uint64_t(account), uint64_t(owner) });
        if (cached != _profiles.end())
            return cached->second;
        QueryResult r = CharacterDatabase.Query("SELECT profile FROM romance_player_profile WHERE account_id = {} AND owner_guid = {}",
                                                account, uint32(owner));
        if (!r)
            return std::nullopt;
        auto j = Json::Parse(r->Fetch()[0].Get<std::string>());
        if (!j)
            return std::nullopt;
        PlayerProfile p = PlayerProfile::FromJson(*j);
        p.Sanitize(4000);
        return p;
    }

    void RomanceSqlRepository::SavePlayerProfile(uint32_t account, Guid owner, PlayerProfile const& p)
    {
        _profiles[{ uint64_t(account), uint64_t(owner) }] = p;
        CharacterDatabase.Execute(
            "INSERT INTO romance_player_profile (account_id, owner_guid, profile, updated_at) VALUES ({}, {}, '{}', UNIX_TIMESTAMP()) "
            "ON DUPLICATE KEY UPDATE profile = VALUES(profile), updated_at = VALUES(updated_at)",
            account, uint32(owner), Esc(p.ToJson().Dump()));
    }

    std::vector<MemoryEntry> RomanceSqlRepository::LoadMemories(CompanionKey const& key)
    {
        std::vector<MemoryEntry> out;
        QueryResult r = CharacterDatabase.Query(
            "SELECT id, kind, source, importance, text, pinned, recall_count, created_at, updated_at, last_recalled_at "
            "FROM romance_memory WHERE owner_guid = {} AND companion_guid = {} ORDER BY id",
            uint32(key.owner), uint32(key.companion));
        if (!r)
            return out;
        do
        {
            Field* f = r->Fetch();
            MemoryEntry m;
            m.id = f[0].Get<uint32>();
            auto kind = MemoryKindFromId(f[1].Get<std::string>());
            auto source = MemorySourceFromId(f[2].Get<std::string>());
            if (!kind || !source)
                continue;
            m.kind = *kind;
            m.source = *source;
            m.importance = f[3].Get<uint8>();
            m.text = f[4].Get<std::string>();
            m.pinned = f[5].Get<uint8>() != 0;
            m.recallCount = f[6].Get<uint32>();
            m.createdAt = f[7].Get<int64>();
            m.updatedAt = f[8].Get<int64>();
            m.lastRecalledAt = f[9].Get<int64>();
            m.text.erase(std::remove(m.text.begin(), m.text.end(), '|'), m.text.end());
            if (!m.text.empty())
                out.push_back(m);
        } while (r->NextRow());
        return out;
    }

    void RomanceSqlRepository::SaveMemory(CompanionKey const& key, MemoryEntry const& m)
    {
        CharacterDatabase.DirectExecute(
            "INSERT INTO romance_memory (owner_guid, companion_guid, id, kind, source, importance, text, pinned, recall_count, "
            "created_at, updated_at, last_recalled_at) VALUES ({}, {}, {}, '{}', '{}', {}, '{}', {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE kind = VALUES(kind), source = VALUES(source), importance = VALUES(importance), "
            "text = VALUES(text), pinned = VALUES(pinned), recall_count = VALUES(recall_count), updated_at = VALUES(updated_at), "
            "last_recalled_at = VALUES(last_recalled_at)",
            uint32(key.owner), uint32(key.companion), m.id, MemoryKindId(m.kind), MemorySourceId(m.source),
            std::max(0, std::min(100, m.importance)), Esc(LimitUtf8(m.text, 400)), m.pinned ? 1 : 0, m.recallCount,
            int64(m.createdAt), int64(m.updatedAt), int64(m.lastRecalledAt));
    }

    void RomanceSqlRepository::DeleteMemories(CompanionKey const& key, std::vector<uint32_t> const& ids)
    {
        if (ids.empty())
            return;
        std::ostringstream list;
        for (size_t i = 0; i < ids.size(); ++i)
            list << (i ? "," : "") << ids[i];
        CharacterDatabase.DirectExecute("DELETE FROM romance_memory WHERE owner_guid = {} AND companion_guid = {} AND id IN ({})",
                                  uint32(key.owner), uint32(key.companion), list.str());
    }

    void RomanceSqlRepository::DeleteAllMemories(CompanionKey const& key)
    {
        CharacterDatabase.DirectExecute("DELETE FROM romance_memory WHERE owner_guid = {} AND companion_guid = {}", uint32(key.owner), uint32(key.companion));
    }

    std::vector<ChatTurn> RomanceSqlRepository::LoadRecentChat(CompanionKey const& key, uint32_t limit)
    {
        std::vector<ChatTurn> out;
        QueryResult r = CharacterDatabase.Query(
            "SELECT role, text, created_at FROM romance_chat_log WHERE owner_guid = {} AND companion_guid = {} ORDER BY id DESC LIMIT {}",
            uint32(key.owner), uint32(key.companion), limit);
        if (!r)
            return out;
        do
        {
            Field* f = r->Fetch();
            ChatTurn t;
            uint8 role = f[0].Get<uint8>();
            t.role = role == 1 ? ChatTurn::Role::Companion : role == 2 ? ChatTurn::Role::Event : ChatTurn::Role::Player;
            t.text = f[1].Get<std::string>();
            t.at = f[2].Get<int64>();
            out.push_back(t);
        } while (r->NextRow());
        std::reverse(out.begin(), out.end());
        return out;
    }

    void RomanceSqlRepository::AppendChat(CompanionKey const& key, ChatTurn const& t)
    {
        CharacterDatabase.Execute("INSERT INTO romance_chat_log (owner_guid, companion_guid, role, text, created_at) VALUES ({}, {}, {}, '{}', {})",
                                  uint32(key.owner), uint32(key.companion), uint32(t.role), Esc(LimitUtf8(t.text, 2000)), int64(t.at));
    }

    void RomanceSqlRepository::PruneChat(CompanionKey const& key, uint32_t keep)
    {
        // Derived table so MySQL allows reading the table being deleted from.
        CharacterDatabase.Execute(
            "DELETE FROM romance_chat_log WHERE owner_guid = {0} AND companion_guid = {1} AND id < "
            "(SELECT min_id FROM (SELECT MIN(id) AS min_id FROM (SELECT id FROM romance_chat_log WHERE owner_guid = {0} AND "
            "companion_guid = {1} ORDER BY id DESC LIMIT {2}) newest) t)",
            uint32(key.owner), uint32(key.companion), std::max<uint32_t>(1, keep));
    }

    void RomanceSqlRepository::ClearChat(CompanionKey const& key)
    {
        CharacterDatabase.DirectExecute("DELETE FROM romance_chat_log WHERE owner_guid = {} AND companion_guid = {}", uint32(key.owner), uint32(key.companion));
    }

    std::vector<GiftRecord> RomanceSqlRepository::LoadGifts(CompanionKey const& key, TimePoint since)
    {
        std::vector<GiftRecord> out;
        QueryResult r = CharacterDatabase.Query(
            "SELECT item_guid, item_entry, item_count, score, category, item_name, created_at FROM romance_gift_log "
            "WHERE owner_guid = {} AND companion_guid = {} AND created_at >= {} ORDER BY id",
            uint32(key.owner), uint32(key.companion), int64(since));
        if (!r)
            return out;
        do
        {
            Field* f = r->Fetch();
            GiftRecord g;
            g.itemGuid = f[0].IsNull() ? 0 : f[0].Get<uint32>();
            g.entry = f[1].Get<uint32>();
            g.count = f[2].Get<uint32>();
            g.score = f[3].Get<float>();
            g.category = f[4].Get<std::string>();
            g.name = f[5].Get<std::string>();
            g.at = f[6].Get<int64>();
            out.push_back(g);
        } while (r->NextRow());
        return out;
    }

    std::set<uint64_t> RomanceSqlRepository::LoadRewardedItemGuids(Guid companion)
    {
        std::set<uint64_t> out;
        QueryResult r = CharacterDatabase.Query("SELECT item_guid FROM romance_gift_log WHERE companion_guid = {} AND item_guid IS NOT NULL",
                                                uint32(companion));
        if (!r)
            return out;
        do
            out.insert(r->Fetch()[0].Get<uint32>());
        while (r->NextRow());
        return out;
    }

    void RomanceSqlRepository::InsertGift(CompanionKey const& key, GiftRecord const& g)
    {
        std::string itemGuid = g.itemGuid ? std::to_string(uint32(g.itemGuid)) : "NULL";
        // INSERT IGNORE + UNIQUE(companion_guid, item_guid): a replayed item can never be logged (or rewarded) twice.
        CharacterDatabase.Execute(
            "INSERT IGNORE INTO romance_gift_log (owner_guid, companion_guid, item_guid, item_entry, item_count, item_name, category, score, created_at) "
            "VALUES ({}, {}, {}, {}, {}, '{}', '{}', {}, {})",
            uint32(key.owner), uint32(key.companion), itemGuid, g.entry, g.count, Esc(LimitUtf8(g.name, 120)), Esc(g.category),
            float(g.score), int64(g.at));
    }

    void RomanceSqlRepository::LogRelationshipEvent(CompanionKey const& key, std::string const& type, std::string const& detail, TimePoint at)
    {
        CharacterDatabase.Execute("INSERT INTO romance_relationship_log (owner_guid, companion_guid, type, detail, created_at) VALUES ({}, {}, '{}', '{}', {})",
                                  uint32(key.owner), uint32(key.companion), Esc(LimitUtf8(type, 32)), Esc(LimitUtf8(detail, 255)), int64(at));
    }
}
