/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceControl.h"
#include "Log.h"
#include "RomanceGift.h"
#include "RomancePlayerbots.h"

#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdio>
#include <set>
#include <unordered_map>

namespace romance::ac
{
    namespace
    {
        constexpr uint8 kSkullIcon = 7;
        constexpr float kTradeDistance = 10.0f;
        constexpr float kHandOverDistance = 30.0f;   // gold is handed over directly, no trade window
        constexpr time_t kAutoSellCooldown = 60;
        constexpr time_t kBagsFullCooldown = 900;
        constexpr size_t kMaxKnowledgeItems = 30;
        constexpr size_t kMaxKnowledgeSpells = 40;

        std::string Lower(std::string s)
        {
            for (auto& c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Lowercase, letters/digits only (single spaces): "Power Word: Fortitude" -> "power word fortitude".
        std::string Simplify(std::string const& s)
        {
            std::string out;
            for (char ch : Lower(s))
            {
                unsigned char c = static_cast<unsigned char>(ch);
                if (std::isalnum(c) || c >= 0x80)
                    out += ch;
                else if (!out.empty() && out.back() != ' ')
                    out += ' ';
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        }

        // "wolves" -> "wolf", "murlocs" -> "murloc" (rough, only used as a second try).
        std::string Singular(std::string const& s)
        {
            if (s.size() > 4 && s.compare(s.size() - 3, 3, "ves") == 0)
                return s.substr(0, s.size() - 3) + "f";
            if (s.size() > 4 && s.compare(s.size() - 3, 3, "ies") == 0)
                return s.substr(0, s.size() - 3) + "y";
            if (s.size() > 3 && s.back() == 's' && s[s.size() - 2] != 's')
                return s.substr(0, s.size() - 1);
            return s;
        }

        // Match score of `name` for the player's words `query` (0 = no match).
        int MatchScore(std::string const& name, std::string const& query)
        {
            if (query.empty())
                return 0;
            std::string n = Simplify(name);
            std::string q = Simplify(query);
            if (n == q)
                return 100;
            if (n.find(q) != std::string::npos)
                return 60 + int(std::min<size_t>(30, q.size()));
            std::string sq = Singular(q);
            if (sq != q && n.find(sq) != std::string::npos)
                return 50 + int(std::min<size_t>(30, sq.size()));
            return 0;
        }

        std::string Money(uint64 copper)
        {
            uint64 g = copper / 10000, s = (copper / 100) % 100, c = copper % 100;
            std::string out;
            if (g)
                out += std::to_string(g) + "g";
            if (s)
                out += (out.empty() ? "" : " ") + std::to_string(s) + "s";
            if (c || out.empty())
                out += (out.empty() ? "" : " ") + std::to_string(c) + "c";
            return out;
        }

        std::string ItemLink(ItemTemplate const* proto)
        {
            char buf[512];
            uint32 color = ItemQualityColors[std::min<uint32>(proto->Quality, MAX_ITEM_QUALITY - 1)];
            std::snprintf(buf, sizeof(buf), "|c%08x|Hitem:%u:0:0:0:0:0:0:0:0|h[%s]|h|r", color, proto->ItemId, proto->Name1.c_str());
            return buf;
        }

        std::string QuestLink(Quest const* q)
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "|cffffff00|Hquest:%u:%d|h[%s]|h|r", q->GetQuestId(), q->GetQuestLevel(), q->GetTitle().c_str());
            return buf;
        }

        char const* QualityName(uint32 q)
        {
            static char const* const names[] = { "poor", "common", "uncommon", "rare", "epic", "legendary", "artifact", "heirloom" };
            return q < 8 ? names[q] : "common";
        }

        template <typename F>
        void ForEachBagItem(Player* p, F&& fn)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    fn(item);
            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                if (Bag* b = p->GetBagByPos(bag))
                    for (uint32 j = 0; j < b->GetBagSize(); ++j)
                        if (Item* item = b->GetItemByPos(uint8(j)))
                            fn(item);
        }

        uint32 TotalBagSlots(Player* p)
        {
            uint32 total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                if (Bag* b = p->GetBagByPos(bag))
                    total += b->GetBagSize();
            return total;
        }

        struct ItemGroup
        {
            ItemTemplate const* proto = nullptr;
            uint32 count = 0;
            uint32 stacks = 0;
            bool soulbound = false;
        };

        std::vector<ItemGroup> GroupedBagItems(Player* p)
        {
            std::map<uint32, ItemGroup> groups;
            ForEachBagItem(p, [&](Item* item)
            {
                ItemTemplate const* proto = item->GetTemplate();
                if (!proto)
                    return;
                ItemGroup& g = groups[proto->ItemId];
                g.proto = proto;
                g.count += item->GetCount();
                ++g.stacks;
                g.soulbound = g.soulbound || item->IsSoulBound();
            });
            std::vector<ItemGroup> out;
            for (auto& [id, g] : groups)
                out.push_back(g);
            std::sort(out.begin(), out.end(), [](ItemGroup const& a, ItemGroup const& b)
            {
                if (a.proto->Quality != b.proto->Quality)
                    return a.proto->Quality > b.proto->Quality;
                return a.proto->Name1 < b.proto->Name1;
            });
            return out;
        }

        // Highest learned, active, castable spells of the companion, one entry per name.
        std::vector<SpellInfo const*> KnownSpells(Player* p)
        {
            std::map<std::string, SpellInfo const*> byName;
            for (auto const& [id, ps] : p->GetSpellMap())
            {
                if (!ps || ps->State == PLAYERSPELL_REMOVED || !ps->Active)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                if (!info || info->IsPassive() || info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) || !info->SpellName[0] ||
                    !*info->SpellName[0])
                    continue;
                // class spells only (professions, mounts and racials live in other families/skill lines)
                if (info->SpellFamilyName == SPELLFAMILY_GENERIC)
                    continue;
                std::string name = info->SpellName[0];
                auto it = byName.find(name);
                if (it == byName.end() || it->second->Id < info->Id)
                    byName[name] = info;
            }
            std::vector<SpellInfo const*> out;
            for (auto& [n, info] : byName)
                out.push_back(info);
            return out;
        }

        std::string QuestProgress(Player* p, Quest const* q, uint16 slot)
        {
            QuestStatus st = p->GetQuestStatus(q->GetQuestId());
            if (st == QUEST_STATUS_COMPLETE)
                return "ready to turn in";
            if (st == QUEST_STATUS_FAILED)
                return "failed";
            std::vector<std::string> parts;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (!q->RequiredNpcOrGo[i] || !q->RequiredNpcOrGoCount[i])
                    continue;
                std::string what;
                if (q->RequiredNpcOrGo[i] > 0)
                {
                    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(uint32(q->RequiredNpcOrGo[i])))
                        what = ct->Name;
                }
                else if (GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(uint32(-q->RequiredNpcOrGo[i])))
                    what = gt->name;
                if (!q->ObjectiveText[i].empty())
                    what = q->ObjectiveText[i];
                parts.push_back((what.empty() ? "objective" : what) + " " + std::to_string(p->GetQuestSlotCounter(slot, i)) + "/" +
                                std::to_string(q->RequiredNpcOrGoCount[i]));
            }
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                if (!q->RequiredItemId[i] || !q->RequiredItemCount[i])
                    continue;
                ItemTemplate const* it = sObjectMgr->GetItemTemplate(q->RequiredItemId[i]);
                parts.push_back((it ? it->Name1 : std::string("item")) + " " +
                                std::to_string(std::min(p->GetItemCount(q->RequiredItemId[i]), q->RequiredItemCount[i])) + "/" +
                                std::to_string(q->RequiredItemCount[i]));
            }
            if (parts.empty())
                return "in progress";
            std::string out;
            for (auto const& s : parts)
                out += (out.empty() ? "" : ", ") + s;
            return out;
        }

        // The reward slot a companion takes from a quest that offers a choice of items (0 when there is no choice):
        // gear that upgrades what she wears and that she can use, in her best armor type, comes first.
        uint32 BestRewardChoice(Player* bot, Quest const* q)
        {
            std::vector<GiftItem> options;
            for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
            {
                GiftItem g;   // entry 0 = empty slot
                ItemTemplate const* proto = q->RewardChoiceItemId[i] ? sObjectMgr->GetItemTemplate(q->RewardChoiceItemId[i]) : nullptr;
                if (proto)
                {
                    g.entry = proto->ItemId;
                    g.itemClass = int(proto->Class);
                    g.itemSubClass = int(proto->SubClass);
                    g.inventoryType = int(proto->InventoryType);
                    g.sellPrice = proto->SellPrice;
                    g.itemLevel = int(proto->ItemLevel);
                    g.isEquippable = proto->InventoryType != INVTYPE_NON_EQUIP;
                    if (g.isEquippable)
                    {
                        g.usableByCompanion = bot->CanUseItem(proto) == EQUIP_ERR_OK;
                        uint8 slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
                        if (slot < EQUIPMENT_SLOT_END)
                        {
                            Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                            g.equippedItemLevel = worn && worn->GetTemplate() ? int(worn->GetTemplate()->ItemLevel) : -1;
                        }
                    }
                }
                options.push_back(g);
            }
            return uint32(PickQuestReward(options, PreferredArmorTier(int(bot->getClass()), int(bot->GetLevel()))));
        }

        std::vector<std::pair<Quest const*, uint16>> QuestLog(Player* p)
        {
            std::vector<std::pair<Quest const*, uint16>> out;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (uint32 id = p->GetQuestSlotQuestId(slot))
                    if (Quest const* q = sObjectMgr->GetQuestTemplate(id))
                        out.push_back({ q, slot });
            return out;
        }

        char const* CreatureTypeName(uint32 type)
        {
            switch (type)
            {
                case CREATURE_TYPE_BEAST: return "beast";
                case CREATURE_TYPE_DRAGONKIN: return "dragonkin";
                case CREATURE_TYPE_DEMON: return "demon";
                case CREATURE_TYPE_ELEMENTAL: return "elemental";
                case CREATURE_TYPE_GIANT: return "giant";
                case CREATURE_TYPE_UNDEAD: return "undead";
                case CREATURE_TYPE_HUMANOID: return "humanoid";
                case CREATURE_TYPE_CRITTER: return "critter";
                case CREATURE_TYPE_MECHANICAL: return "mechanical";
                default: return "creature";
            }
        }

        std::string DescribeUnit(Unit* u, Player* bot)
        {
            std::string out = u->GetName() + " (level " + std::to_string(u->GetLevel());
            if (Creature* c = u->ToCreature())
            {
                out += std::string(", ") + CreatureTypeName(c->GetCreatureType());
                if (c->isElite())
                    out += ", elite";
                if (c->IsVendor())
                    out += ", merchant";
                else if (c->IsQuestGiver())
                    out += ", quest giver";
            }
            else if (u->ToPlayer())
                out += ", player";
            out += std::string(", ") + (u->IsAlive() ? std::to_string(int(u->GetHealthPct())) + "% health" : "dead");
            if (bot)
                out += bot->IsValidAttackTarget(u) ? ", hostile" : ", not hostile";
            return out + ")";
        }

        std::vector<Creature*> NearbyCreatures(WorldObject* center, float range)
        {
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(center, range);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(center, units, check);
            Cell::VisitObjects(center, searcher, range);
            std::vector<Creature*> out;
            for (Unit* u : units)
                if (Creature* c = u->ToCreature())
                    out.push_back(c);
            std::sort(out.begin(), out.end(), [center](Creature* a, Creature* b) { return center->GetDistance(a) < center->GetDistance(b); });
            return out;
        }

        // A merchant within `range` of the companion (no need to target or walk up to him).
        Creature* NearbyVendor(Player* bot, float range)
        {
            for (Creature* c : NearbyCreatures(bot, range))
                if (c->IsVendor() && c->IsAlive() && !c->IsHostileTo(bot))
                    return c;
            return nullptr;
        }

        // Sells items directly (like the vendor window would): money in, items gone.
        uint64 SellItems(Player* bot, std::vector<Item*> const& items, uint32* sold, uint32* units = nullptr)
        {
            uint64 total = 0;
            uint32 n = 0, count = 0;
            for (Item* item : items)
            {
                ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                if (!proto || !proto->SellPrice || item->IsInTrade())
                    continue;
                total += uint64(proto->SellPrice) * item->GetCount();
                count += item->GetCount();
                ++n;
                bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            }
            if (total)
                bot->ModifyMoney(int32(std::min<uint64>(total, 0x7FFFFFFF)));
            if (sold)
                *sold = n;
            if (units)
                *units = count;
            return total;
        }

        // Sells exactly `want` units of the given stacks (a part of a stack leaves the rest in the bag).
        uint64 SellUnits(Player* bot, std::vector<Item*> const& items, uint32 want, uint32* units)
        {
            uint64 total = 0;
            uint32 left = want;
            for (Item* item : items)
            {
                if (!left)
                    break;
                ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                if (!proto || !proto->SellPrice || item->IsInTrade())
                    continue;
                uint32 take = std::min<uint32>(left, item->GetCount());
                total += uint64(proto->SellPrice) * take;
                left -= take;
                if (take >= item->GetCount())
                    bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                else
                {
                    item->SetCount(item->GetCount() - take);
                    item->SetState(ITEM_CHANGED, bot);
                }
            }
            if (total)
                bot->ModifyMoney(int32(std::min<uint64>(total, 0x7FFFFFFF)));
            if (units)
                *units = want - left;
            return total;
        }

        std::vector<Item*> JunkItems(Player* bot)
        {
            std::vector<Item*> out;
            ForEachBagItem(bot, [&](Item* item)
            {
                ItemTemplate const* proto = item->GetTemplate();
                if (proto && proto->Quality == ITEM_QUALITY_POOR && proto->SellPrice > 0)
                    out.push_back(item);
            });
            return out;
        }

        // Quests offered (or finished) by creatures within `range` of `center`: quest id -> creature.
        std::map<uint32, Creature*> QuestNpcs(WorldObject* center, float range, bool enders)
        {
            std::map<uint32, Creature*> out;
            for (Creature* c : NearbyCreatures(center, range))
            {
                if (!c->IsAlive() || !c->IsQuestGiver())
                    continue;
                QuestRelationBounds b = enders ? sObjectMgr->GetCreatureQuestInvolvedRelationBounds(c->GetEntry())
                                               : sObjectMgr->GetCreatureQuestRelationBounds(c->GetEntry());
                for (auto it = b.first; it != b.second; ++it)
                    out.emplace(it->second, c);
            }
            return out;
        }

        constexpr float kAreaRadius = 250.0f;

        // "north-east" etc. from `from` towards (x, y); in WoW +x is north and +y is west.
        char const* Direction(WorldObject const* from, float x, float y)
        {
            static char const* const names[] = { "north", "north-west", "west", "south-west", "south", "south-east", "east", "north-east" };
            float a = std::atan2(y - from->GetPositionY(), x - from->GetPositionX());   // 0 = north, +pi/2 = west
            int idx = int(std::lround(a / (3.14159265358979 / 4.0))) & 7;
            return names[idx];
        }

        bool IsClassQuest(Quest const* q) { return q->GetRequiredClasses() != 0; }

        bool CanTakeNow(Player* p, Quest const* q)
        {
            return q && p->FindQuestSlot(q->GetQuestId()) >= MAX_QUEST_LOG_SIZE && !p->GetQuestRewardStatus(q->GetQuestId()) &&
                   p->CanTakeQuest(q, false) && p->CanAddQuest(q, false);
        }

        // Spawned creatures that give or take quests, grouped by map. The creature table has hundreds of thousands of
        // rows and does not change while the server runs, so it is walked once (and again only if its size changes,
        // e.g. a GM adds a spawn) instead of on every request. Without this the Character tab lagged.
        struct QuestSpawn
        {
            uint32 id;
            float posX, posY, posZ;
        };

        std::vector<QuestSpawn> const& QuestSpawns(uint32 mapId)
        {
            static std::unordered_map<uint32, std::vector<QuestSpawn>> byMap;
            static size_t builtFor = std::numeric_limits<size_t>::max();
            auto const& all = sObjectMgr->GetAllCreatureData();
            if (builtFor != all.size())
            {
                byMap.clear();
                for (auto const& [spawnId, data] : all)
                {
                    QuestRelationBounds starts = sObjectMgr->GetCreatureQuestRelationBounds(data.id);
                    QuestRelationBounds ends = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(data.id);
                    if (starts.first == starts.second && ends.first == ends.second)
                        continue;
                    byMap[data.mapid].push_back({ data.id, data.posX, data.posY, data.posZ });
                }
                builtFor = all.size();
            }
            static std::vector<QuestSpawn> const none;
            auto it = byMap.find(mapId);
            return it == byMap.end() ? none : it->second;
        }

        // Quests that say "kill 8 Sharptooth Wolves" often count a kill-credit creature: the wolves themselves carry
        // that creature as KillCredit. Maps a credit entry to the creatures that give it (built once, like QuestSpawns).
        std::vector<uint32> const& CreditMobs(uint32 credit)
        {
            static std::unordered_map<uint32, std::vector<uint32>> byCredit;
            static size_t builtFor = std::numeric_limits<size_t>::max();
            auto const& all = sObjectMgr->GetAllCreatureData();
            if (builtFor != all.size())
            {
                byCredit.clear();
                std::set<uint32> seen;
                for (auto const& [spawnId, data] : all)
                {
                    if (!seen.insert(data.id).second)
                        continue;
                    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id))
                        for (uint32 c : ct->KillCredit)
                            if (c && c != data.id)
                                byCredit[c].push_back(data.id);
                }
                builtFor = all.size();
            }
            static std::vector<uint32> const none;
            auto it = byCredit.find(credit);
            return it == byCredit.end() ? none : it->second;
        }

        struct GiverInfo
        {
            uint32 entry = 0;
            std::string name;
            float x = 0, y = 0, z = 0;
            float dist = 0;
            std::vector<Quest const*> quests;
        };

        // Quest givers (spawned creatures) within `radius` that offer quests `p` can take right now; nearest first.
        std::vector<GiverInfo> AvailableGivers(Player* p, float radius, std::string const& filter)
        {
            bool classOnly = filter == "class";
            std::map<uint32, GiverInfo> byEntry;
            FactionTemplateEntry const* mine = p->GetFactionTemplateEntry();
            for (QuestSpawn const& data : QuestSpawns(p->GetMapId()))
            {
                float d = p->GetDistance2d(data.posX, data.posY);
                if (d > radius)
                    continue;
                auto known = byEntry.find(data.id);
                if (known != byEntry.end())
                {
                    if (d < known->second.dist)
                    {
                        known->second.dist = d;
                        known->second.x = data.posX;
                        known->second.y = data.posY;
                        known->second.z = data.posZ;
                    }
                    continue;
                }
                QuestRelationBounds b = sObjectMgr->GetCreatureQuestRelationBounds(data.id);
                if (b.first == b.second)
                    continue;
                CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
                if (!ct)
                    continue;
                if (FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(ct->faction))
                    if (mine && mine->IsHostileTo(*ft))
                        continue;
                GiverInfo g;
                g.entry = data.id;
                g.name = ct->Name;
                g.x = data.posX;
                g.y = data.posY;
                g.z = data.posZ;
                g.dist = d;
                for (auto it = b.first; it != b.second; ++it)
                {
                    Quest const* q = sObjectMgr->GetQuestTemplate(it->second);
                    if (!CanTakeNow(p, q))
                        continue;
                    if (classOnly ? !IsClassQuest(q) : (!filter.empty() && MatchScore(q->GetTitle(), filter) == 0))
                        continue;
                    g.quests.push_back(q);
                }
                byEntry[data.id] = g;
            }
            std::vector<GiverInfo> out;
            for (auto& [e, g] : byEntry)
                if (!g.quests.empty())
                    out.push_back(std::move(g));
            std::sort(out.begin(), out.end(), [](GiverInfo const& a, GiverInfo const& b) { return a.dist < b.dist; });
            return out;
        }

        // Spawned quest enders within `radius` that take one of `ready` (finished quests); nearest first.
        std::vector<GiverInfo> QuestEnders(Player* p, float radius, std::vector<Quest const*> const& ready)
        {
            std::map<uint32, GiverInfo> byEntry;
            FactionTemplateEntry const* mine = p->GetFactionTemplateEntry();
            for (QuestSpawn const& data : QuestSpawns(p->GetMapId()))
            {
                float d = p->GetDistance2d(data.posX, data.posY);
                if (d > radius)
                    continue;
                auto known = byEntry.find(data.id);
                if (known != byEntry.end())
                {
                    if (d < known->second.dist)
                    {
                        known->second.dist = d;
                        known->second.x = data.posX;
                        known->second.y = data.posY;
                        known->second.z = data.posZ;
                    }
                    continue;
                }
                QuestRelationBounds b = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(data.id);
                if (b.first == b.second)
                    continue;
                CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
                if (!ct)
                    continue;
                if (FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(ct->faction))
                    if (mine && mine->IsHostileTo(*ft))
                        continue;
                GiverInfo g;
                g.entry = data.id;
                g.name = ct->Name;
                g.x = data.posX;
                g.y = data.posY;
                g.z = data.posZ;
                g.dist = d;
                for (auto it = b.first; it != b.second; ++it)
                    for (Quest const* q : ready)
                        if (q->GetQuestId() == it->second)
                            g.quests.push_back(q);
                byEntry[data.id] = g;
            }
            std::vector<GiverInfo> out;
            for (auto& [e, g] : byEntry)
                if (!g.quests.empty())
                    out.push_back(std::move(g));
            std::sort(out.begin(), out.end(), [](GiverInfo const& a, GiverInfo const& b) { return a.dist < b.dist; });
            return out;
        }

        std::string QuestList(std::vector<Quest const*> const& qs)
        {
            std::string s;
            for (Quest const* q : qs)
                s += (s.empty() ? "\"" : ", \"") + q->GetTitle() + "\"" + (IsClassQuest(q) ? " (class quest)" : "");
            return s;
        }

        bool Near(Player* a, Player* b, float range)
        {
            return a && b && a->IsInWorld() && b->IsInWorld() && a->GetMapId() == b->GetMapId() && a->GetDistance(b) <= range;
        }

        // Unfinished kill objectives of `q` for `p`.
        std::set<uint32> KillEntries(Player* p, Quest const* q)
        {
            std::set<uint32> out;
            uint16 slot = p->FindQuestSlot(q->GetQuestId());
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (q->RequiredNpcOrGo[i] > 0 && q->RequiredNpcOrGoCount[i] &&
                    (slot >= MAX_QUEST_LOG_SIZE || p->GetQuestSlotCounter(slot, i) < q->RequiredNpcOrGoCount[i]))
                {
                    out.insert(uint32(q->RequiredNpcOrGo[i]));
                    for (uint32 mob : CreditMobs(uint32(q->RequiredNpcOrGo[i])))
                        out.insert(mob);
                }
            return out;
        }

        // Creatures that drop the quest items `p` still needs for `q` ("bring 8 Lynx Collars").
        std::set<uint32> DropEntries(Player* p, Quest const* q)
        {
            static std::map<uint32, std::set<uint32>> cache;   // item -> creature entries (world data doesn't change)
            std::set<uint32> out;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                uint32 item = q->RequiredItemId[i];
                if (!item || !q->RequiredItemCount[i] || p->GetItemCount(item, true) >= q->RequiredItemCount[i])
                    continue;
                auto it = cache.find(item);
                if (it == cache.end())
                {
                    std::set<uint32> found;
                    if (QueryResult r = WorldDatabase.Query(
                            "SELECT ct.entry FROM creature_template ct JOIN creature_loot_template l ON l.Entry = ct.lootid "
                            "WHERE l.Item = {} AND ct.lootid <> 0 LIMIT 50", item))
                        do
                            found.insert(r->Fetch()[0].Get<uint32>());
                        while (r->NextRow());
                    it = cache.emplace(item, std::move(found)).first;
                }
                out.insert(it->second.begin(), it->second.end());
            }
            return out;
        }

        // Everything `p` still has to kill for `q`: kill objectives and creatures that drop the needed items.
        std::set<uint32> TargetEntries(Player* p, Quest const* q)
        {
            std::set<uint32> out = KillEntries(p, q);
            for (uint32 e : DropEntries(p, q))
                out.insert(e);
            return out;
        }

        // Where to go for `q`: nearest spawn of an unfinished target, else the quest map marker (POI) of an objective.
        bool QuestDestination(Player* bot, Quest const* q, uint32& map, float& x, float& y, float& z, bool& haveZ)
        {
            std::set<uint32> entries = TargetEntries(bot, q);
            float best = -1;
            if (!entries.empty())
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                {
                    if (data.mapid != bot->GetMapId() || !entries.count(data.id))
                        continue;
                    float d = bot->GetDistance(data.posX, data.posY, data.posZ);
                    if (best < 0 || d < best)
                    {
                        best = d;
                        x = data.posX;
                        y = data.posY;
                        z = data.posZ;
                    }
                }
            if (best >= 0)
            {
                map = bot->GetMapId();
                haveZ = true;
                return true;
            }
            // objective markers first; index -1 is where the quest is turned in, not where it is done
            if (QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(q->GetQuestId()))
                for (int pass = 0; pass < 2; ++pass)
                for (QuestPOI const& poi : *pois)
                {
                    if (poi.MapId != bot->GetMapId() || poi.points.empty() || (pass == 0) != (poi.ObjectiveIndex >= 0))
                        continue;
                    double sx = 0, sy = 0;
                    for (auto const& pt : poi.points)
                    {
                        sx += pt.x;
                        sy += pt.y;
                    }
                    map = poi.MapId;
                    x = float(sx / poi.points.size());
                    y = float(sy / poi.points.size());
                    z = bot->GetPositionZ();
                    haveZ = false;
                    return true;
                }
            return false;
        }
    }

    void CompanionControl::WarmUp() const
    {
        QuestSpawns(0);
        CreditMobs(0);
    }

    // ------------------------------------------------------------------ knowledge

    void CompanionControl::AddKnowledge(WorldContext& ctx, Player* owner, Player* bot, uint32 topics) const
    {
        if (!_cfg.controlEnabled || !owner || !bot || !topics)
            return;
        auto& k = ctx.knowledge;
        if (topics & (knowledge::Gold | knowledge::Inventory))
            k.push_back("Your gold: " + Money(bot->GetMoney()));
        if (topics & knowledge::Xp)
        {
            uint32 xp = bot->GetUInt32Value(PLAYER_XP), next = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
            if (bot->GetLevel() >= DEFAULT_MAX_LEVEL || !next)
                k.push_back("You are level " + std::to_string(bot->GetLevel()) + " (maximum level).");
            else
                k.push_back("You are level " + std::to_string(bot->GetLevel()) + " with " + std::to_string(xp) + " of " +
                            std::to_string(next) + " XP to the next level (" + std::to_string(int(100.0 * xp / next)) + "%).");
            k.push_back(owner->GetName() + " is level " + std::to_string(owner->GetLevel()) + ".");
        }
        if (topics & knowledge::Inventory)
        {
            uint32 freeSlots = bot->GetFreeInventorySpace();
            k.push_back("Bags: " + std::to_string(freeSlots) + " of " + std::to_string(TotalBagSlots(bot)) + " slots free.");
            std::string list;
            size_t n = 0;
            for (ItemGroup const& g : GroupedBagItems(bot))
            {
                if (++n > kMaxKnowledgeItems)
                {
                    list += ", and more";
                    break;
                }
                list += (list.empty() ? "" : ", ") + g.proto->Name1 + (g.count > 1 ? " x" + std::to_string(g.count) : "") +
                        (g.proto->Quality >= ITEM_QUALITY_UNCOMMON ? std::string(" (") + QualityName(g.proto->Quality) + ")" : "") +
                        (g.proto->Quality == ITEM_QUALITY_POOR ? " (junk)" : "");
            }
            k.push_back("In your bags: " + (list.empty() ? std::string("nothing") : list) + ".");
        }
        if (topics & knowledge::Gear)
        {
            std::string list;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (ItemTemplate const* proto = item->GetTemplate())
                        list += (list.empty() ? "" : ", ") + proto->Name1;
            k.push_back("You are wearing: " + (list.empty() ? std::string("nothing special") : list) + ".");
        }
        if (topics & knowledge::Spells)
        {
            std::string list;
            size_t n = 0;
            for (SpellInfo const* info : KnownSpells(bot))
            {
                if (++n > kMaxKnowledgeSpells)
                {
                    list += ", and more";
                    break;
                }
                list += (list.empty() ? "" : ", ") + std::string(info->SpellName[0]);
            }
            k.push_back("Your spells and abilities: " + (list.empty() ? std::string("none worth mentioning") : list) + ".");
        }
        if (topics & knowledge::Quests)
        {
            auto mine = QuestLog(bot);
            if (mine.empty())
                k.push_back("Your quest log is empty.");
            for (auto const& [q, slot] : mine)
            {
                bool theyHave = owner->FindQuestSlot(q->GetQuestId()) < MAX_QUEST_LOG_SIZE;
                k.push_back("Your quest \"" + q->GetTitle() + "\" (level " + std::to_string(q->GetQuestLevel()) + "): " +
                            QuestProgress(bot, q, slot) + (theyHave ? " - they have it too" : " - they don't have it"));
            }
            auto givers = AvailableGivers(bot, kAreaRadius, "");
            if (givers.empty())
                k.push_back("No quest giver around here has a new quest you could take.");
            for (size_t i = 0; i < givers.size() && i < 5; ++i)
                k.push_back("You could pick up from " + givers[i].name + " (about " + std::to_string(int(givers[i].dist)) + " yards " +
                            Direction(bot, givers[i].x, givers[i].y) + "): " + QuestList(givers[i].quests) + ".");
            std::string theirs;
            for (auto const& [q, slot] : QuestLog(owner))
                if (bot->FindQuestSlot(q->GetQuestId()) >= MAX_QUEST_LOG_SIZE)
                    theirs += (theirs.empty() ? "\"" : ", \"") + q->GetTitle() + "\"";
            if (!theirs.empty())
                k.push_back("Quests only " + owner->GetName() + " has: " + theirs + ".");
        }
        if (topics & knowledge::Target)
        {
            ObjectGuid tg = owner->GetTarget();
            Unit* target = tg ? ObjectAccessor::GetUnit(*owner, tg) : nullptr;
            if (!target)
                k.push_back(owner->GetName() + " has nothing targeted right now.");
            else if (target == bot)
                k.push_back(owner->GetName() + " is looking at you.");
            else if (target == owner)
                k.push_back(owner->GetName() + " has targeted themselves.");
            else
                k.push_back(owner->GetName() + " is looking at " + DescribeUnit(target, bot) + ".");
        }
    }

    // ------------------------------------------------------------------ commands

    std::string CompanionControl::Execute(Player* owner, Player* bot, CompanionCommand const& cmd, std::optional<Stage> stage, time_t now)
    {
        if (!_cfg.controlEnabled || !owner || !bot)
            return "";
        if (!bot->IsAlive() && cmd.kind != CompanionCommandKind::StopLead)
            return "Nothing - you are dead right now and cannot do that.";
        bool giving = cmd.kind == CompanionCommandKind::GiveItem || cmd.kind == CompanionCommandKind::GiveGold;
        if (giving && stage && uint32(*stage) < _cfg.giveMinStage)
            return "Nothing. You did NOT give them anything: you don't know them well enough yet to hand over your belongings. "
                   "Refuse kindly and do not pretend to give it.";
        switch (cmd.kind)
        {
            case CompanionCommandKind::GiveItem: return GiveItem(owner, bot, cmd, now);
            case CompanionCommandKind::GiveGold: return GiveGold(owner, bot, cmd, now);
            case CompanionCommandKind::Cast: return Cast(owner, bot, cmd);
            case CompanionCommandKind::AttackNamed:
            case CompanionCommandKind::AttackTarget: return Attack(owner, bot, cmd, now);
            case CompanionCommandKind::AttackAll: return AttackAll(owner, bot, cmd, now);
            case CompanionCommandKind::StopAttack: return StopAttack(owner, bot);
            case CompanionCommandKind::AcceptQuests: return AcceptQuests(owner, bot, cmd, now);
            case CompanionCommandKind::TurnInQuests: return TurnInQuests(owner, bot, cmd, now);
            case CompanionCommandKind::SellJunk:
            case CompanionCommandKind::SellItem: return Sell(owner, bot, cmd);
            case CompanionCommandKind::ShareQuests:
            case CompanionCommandKind::ShareQuest: return Share(owner, bot, cmd, now);
            case CompanionCommandKind::Lead: return StartLead(owner, bot, cmd, now);
            case CompanionCommandKind::StopLead: return StopLead(owner, bot);
            case CompanionCommandKind::PauseLead:
            {
                auto it = _leads.find(bot->GetGUID());
                if (it == _leads.end())
                    return "";   // not leading: the normal Playerbots "stay"
                it->second.paused = true;
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                return "You stopped and wait here. You are still leading the way to \"" + it->second.title +
                       "\" and continue when they say so.";
            }
            case CompanionCommandKind::ResumeLead:
                return _leads.count(bot->GetGUID()) ? ResumeLead(owner, bot) : "";
            default: return "";
        }
    }

    void CompanionControl::Trade(Player* owner, Player* bot, std::string const& command, time_t now)
    {
        // Playerbots' "t" only opens the window when no trade is running; the items/gold are added by a
        // second "t" once the window is open (see Update()).
        if (bot->GetTrader() == owner)
        {
            PlayerbotsBridge::Command(bot, owner, command);
            return;
        }
        PlayerbotsBridge::Command(bot, owner, "t");
        _pending.push_back({ bot->GetGUID(), owner->GetGUID(), command, now, now + 15, true });
    }

    std::string CompanionControl::GiveItem(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        ItemGroup const* best = nullptr;
        int bestScore = 0;
        auto groups = GroupedBagItems(bot);
        for (ItemGroup const& g : groups)
        {
            int score = MatchScore(g.proto->Name1, cmd.arg);
            if (score > bestScore)
            {
                best = &g;
                bestScore = score;
            }
        }
        if (!best)
            return "Nothing. They asked for \"" + cmd.arg + "\", but you have no such item in your bags; tell them.";
        if (best->soulbound)
            return "Nothing. They asked for your " + best->proto->Name1 + ", but it is soulbound to you and cannot be traded; tell them.";
        if (bot->GetMapId() != owner->GetMapId() || bot->GetDistance(owner) > kTradeDistance)
            return "Nothing yet. They asked for your " + best->proto->Name1 + ", but you are too far apart to trade; ask them to come closer.";
        if (owner->GetTrader() && owner->GetTrader() != bot)
            return "Nothing. They are trading with someone else right now.";

        // Trades move whole stacks: offer enough stacks to cover the requested amount.
        uint32 wanted = cmd.amount ? uint32(std::min<uint64>(cmd.amount, best->count)) : best->count;
        uint32 perStack = std::max<uint32>(1, best->proto->GetMaxStackSize());
        uint32 stacks = std::max<uint32>(1, std::min<uint32>(best->stacks, (wanted + perStack - 1) / perStack));
        stacks = std::min<uint32>(stacks, 6);
        Trade(owner, bot, "t " + ItemLink(best->proto) + " " + std::to_string(stacks), now);
        std::string what = best->proto->Name1 + (best->count > 1 ? " (you have " + std::to_string(best->count) + ")" : "");
        std::string note = cmd.amount && cmd.amount > best->count ? " You had fewer than they asked for." : "";
        return "You opened a trade with them and put your " + what + " in it; they only need to accept." + note;
    }

    std::string CompanionControl::GiveGold(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        uint64 amount = cmd.amount;
        if (bot->GetMoney() < amount)
            return "Nothing. They asked for " + Money(amount) + ", but you only have " + Money(bot->GetMoney()) + "; tell them.";
        if (_cfg.maxGoldPerRequest && amount > _cfg.maxGoldPerRequest)
            return "Nothing. They asked for " + Money(amount) + ", which is more than you are willing to hand over at once (at most " +
                   Money(_cfg.maxGoldPerRequest) + "); tell them.";
        if (bot->GetMapId() != owner->GetMapId() || bot->GetDistance(owner) > kHandOverDistance)
            return "Nothing yet. They asked for " + Money(amount) + ", but you are too far apart; ask them to come closer.";
        if (amount == 0 || amount > uint64(std::numeric_limits<int32>::max()))
            return "Nothing. That is not an amount you can hand over at once; tell them.";
        uint64 before = owner->GetMoney();
        if (!owner->ModifyMoney(int32(amount), false))
            return "Nothing. They can't carry any more gold; tell them.";
        if (uint64(owner->GetMoney()) < before + amount)
        {
            LOG_WARN("module", "[mod-romance] gold handover failed: {} had {} copper, asked {}, has {} after", owner->GetName(), before,
                     amount, owner->GetMoney());
            return "Nothing. The gold did not arrive; tell them something went wrong.";
        }
        bot->ModifyMoney(-int32(amount));
        (void)now;
        return "You handed them " + Money(amount) + " (it is in their bags now). You have " + Money(bot->GetMoney()) + " left.";
    }

    std::string CompanionControl::Cast(Player* owner, Player* bot, CompanionCommand const& cmd)
    {
        SpellInfo const* best = nullptr;
        int bestScore = 0;
        for (SpellInfo const* info : KnownSpells(bot))
        {
            int score = MatchScore(info->SpellName[0], cmd.arg);
            if (score > bestScore)
            {
                best = info;
                bestScore = score;
            }
        }
        if (!best)
            return "Nothing. They asked you to cast \"" + cmd.arg + "\", but you don't know such a spell; tell them.";
        std::string name = best->SpellName[0];
        if (bot->HasSpellCooldown(best->Id))
            return "Nothing yet. " + name + " is still on cooldown; tell them.";
        std::string command = "cast " + name;
        std::string target;
        if (cmd.onMe)
        {
            command += " on " + owner->GetName();
            target = "on them";
        }
        else
        {
            ObjectGuid tg = owner->GetTarget();
            Unit* u = tg ? ObjectAccessor::GetUnit(*owner, tg) : nullptr;
            target = u ? (u == bot ? "on yourself" : "on " + u->GetName()) : "on yourself";
        }
        PlayerbotsBridge::Command(bot, owner, command);
        return "You cast " + name + " " + target + ".";
    }

    std::string CompanionControl::Attack(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        if (cmd.kind == CompanionCommandKind::AttackTarget)
        {
            ObjectGuid tg = owner->GetTarget();
            Unit* u = tg ? ObjectAccessor::GetUnit(*owner, tg) : nullptr;
            if (u && bot->IsValidAttackTarget(u))
            {
                if (!bot->IsWithinLOSInMap(u))
                    return ChaseAttack(owner, bot, u, now);   // Playerbots won't attack what it can't see
                PlayerbotsBridge::Command(bot, owner, "attack");
                return "You attack " + u->GetName() + ".";
            }
            // nothing targeted: the nearest enemy
        }

        uint32 type = CreatureTypeFromWord(cmd.arg);
        std::list<Unit*> units;
        float range = _cfg.commandRange;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        Unit* best = nullptr;
        float bestDist = 0;
        Unit* hidden = nullptr;
        for (Unit* u : units)
        {
            if (!u->IsAlive() || !bot->IsValidAttackTarget(u) || u->ToPlayer())
                continue;
            Creature* c = u->ToCreature();
            bool match = cmd.arg.empty() ? (!c || c->GetCreatureType() != CREATURE_TYPE_CRITTER)
                         : type ? (c && c->GetCreatureType() == type) : MatchScore(u->GetName(), cmd.arg) > 0;
            if (!match)
                continue;
            if (!bot->IsWithinLOSInMap(u))
            {
                // Playerbots refuses to attack what it cannot see: only if nothing visible matches, walk closer to this one
                if (!hidden || bot->GetDistance(u) < bot->GetDistance(hidden))
                    hidden = u;
                continue;
            }
            float d = bot->GetDistance(u);
            if (!best || d < bestDist)
            {
                best = u;
                bestDist = d;
            }
        }
        if (!best && hidden)
            return ChaseAttack(owner, bot, hidden, now);
        if (!best)
            return cmd.arg.empty() ? "Nothing. There is no enemy nearby to attack; tell them."
                                   : "Nothing. They told you to attack \"" + cmd.arg + "\", but there is no such enemy nearby; tell them.";
        Group* group = bot->GetGroup();
        if (group && group->IsMember(owner->GetGUID()))
        {
            // Mark it with a skull so the player sees which one, then attack the marked target.
            group->SetTargetIcon(kSkullIcon, owner->GetGUID(), best->GetGUID());
            if (PlayerbotsBridge::IsHealer(bot))
                return "You marked " + DescribeUnit(best, bot) + " with a skull. You are the group's healer, so you don't attack "
                       "yourself; you keep them alive while they fight. Tell them.";
            PlayerbotsBridge::SetRti(bot, "skull");
            PlayerbotsBridge::Command(bot, owner, "d attack rti target");
            return "You marked " + DescribeUnit(best, bot) + " with a skull and attacked it.";
        }
        return "Nothing. You can only pick fights together when you are in the same group; tell them.";
    }

    std::string CompanionControl::ChaseAttack(Player* owner, Player* bot, Unit* target, time_t now)
    {
        Group* group = bot->GetGroup();
        if (!group || !group->IsMember(owner->GetGUID()))
            return "Nothing. You can only pick fights together when you are in the same group; tell them.";
        auto old = _hunts.find(owner->GetGUID());
        if (old != _hunts.end())
        {
            if (old->second.forLead)
                return "Nothing yet. You are fighting for the quest you are leading them to; tell them you are busy with that.";
            ReleaseChasers(old->second);
            _hunts.erase(old);
        }
        Hunt h;
        h.owner = owner->GetGUID();
        h.bots = { bot->GetGUID() };
        h.only = target->GetGUID();
        h.radius = _cfg.commandRange + 20.0f;
        h.until = now + 60;
        _hunts[owner->GetGUID()] = h;
        HuntStep(_hunts[owner->GetGUID()], now);
        return "You can't see " + target->GetName() + " from where you stand, so you move closer and attack as soon as you see it.";
    }

    void CompanionControl::ReleaseChasers(Hunt& h)
    {
        for (ObjectGuid g : h.unfollowed)
            if (Player* b = ObjectAccessor::FindConnectedPlayer(g))
                if (Player* owner = ObjectAccessor::FindConnectedPlayer(h.owner))
                    if (!_leads.count(g))
                        PlayerbotsBridge::SetFollow(b, owner, true);
        h.unfollowed.clear();
    }

    bool CompanionControl::IsBusy(ObjectGuid owner, ObjectGuid bot) const
    {
        if (_leads.count(bot))
            return true;
        auto h = _hunts.find(owner);
        return h != _hunts.end() && std::find(h->second.bots.begin(), h->second.bots.end(), bot) != h->second.bots.end();
    }

    void CompanionControl::OnPlayerFollowOrder(Player* owner, Player* bot)
    {
        if (!owner || !bot)
            return;
        if (_leads.count(bot->GetGUID()))
        {
            StopLead(owner, bot);
            return;
        }
        auto h = _hunts.find(owner->GetGUID());
        if (h != _hunts.end() && !h->second.forLead &&
            std::find(h->second.bots.begin(), h->second.bots.end(), bot->GetGUID()) != h->second.bots.end())
        {
            ReleaseChasers(h->second);
            if (Group* group = bot->GetGroup())
                group->SetTargetIcon(kSkullIcon, owner->GetGUID(), ObjectGuid::Empty);
            _hunts.erase(h);
        }
    }

    std::string CompanionControl::Sell(Player* /*owner*/, Player* bot, CompanionCommand const& cmd)
    {
        Creature* vendor = NearbyVendor(bot, _cfg.interactRange);
        if (!vendor)
            return "Nothing yet. There is no merchant nearby; you can sell as soon as you are close to one. Tell them.";
        if (cmd.kind == CompanionCommandKind::SellJunk)
        {
            uint32 n = 0;
            uint64 value = SellItems(bot, JunkItems(bot), &n);
            if (!n)
                return "Nothing. You have no junk to sell; tell them.";
            return "You sold " + std::to_string(n) + " junk items to " + vendor->GetName() + " for " + Money(value) + ".";
        }
        ItemGroup const* best = nullptr;
        int bestScore = 0;
        auto groups = GroupedBagItems(bot);
        for (ItemGroup const& g : groups)
        {
            int score = MatchScore(g.proto->Name1, cmd.arg);
            if (score > bestScore)
            {
                best = &g;
                bestScore = score;
            }
        }
        if (!best)
            return "Nothing. They asked you to sell \"" + cmd.arg + "\", but you have no such item; tell them.";
        if (!best->proto->SellPrice)
            return "Nothing. Merchants don't buy " + best->proto->Name1 + "; tell them.";
        std::vector<Item*> items;
        uint32 entry = best->proto->ItemId;
        ForEachBagItem(bot, [&](Item* item) { if (item->GetEntry() == entry) items.push_back(item); });
        uint32 n = 0, units = 0;
        uint64 value = 0;
        if (cmd.amount && cmd.amount < best->count)
        {
            // "sell 3 of your linen cloth": exactly that many
            value = SellUnits(bot, items, uint32(cmd.amount), &units);
            n = units ? 1 : 0;
        }
        else
            value = SellItems(bot, items, &n, &units);
        if (!n)
            return "Nothing. Your " + best->proto->Name1 + " could not be sold right now (it may be in a trade); tell them.";
        return "You sold " + (units > 1 ? std::to_string(units) + " of your " : std::string("your ")) + best->proto->Name1 + " to " +
               vendor->GetName() + " for " + Money(value) +
               (best->count > units ? " (you still have " + std::to_string(best->count - units) + ")." : ".");
    }

    std::string CompanionControl::Share(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        std::vector<Quest const*> toShare;
        std::vector<std::string> cannot;
        for (auto const& [q, slot] : QuestLog(bot))
        {
            if (cmd.kind == CompanionCommandKind::ShareQuest && MatchScore(q->GetTitle(), cmd.arg) == 0)
                continue;
            if (owner->FindQuestSlot(q->GetQuestId()) < MAX_QUEST_LOG_SIZE)
                continue;   // they already have it
            if (!q->HasFlag(QUEST_FLAGS_SHARABLE) || !owner->CanTakeQuest(q, false))
            {
                cannot.push_back(q->GetTitle());
                continue;
            }
            toShare.push_back(q);
        }
        if (toShare.empty())
        {
            if (cmd.kind == CompanionCommandKind::ShareQuest)
                return cannot.empty() ? "Nothing. You have no quest called \"" + cmd.arg + "\" that they don't have already; tell them."
                                      : "Nothing. They cannot take \"" + cannot.front() + "\" (level, class or it can't be shared); tell them.";
            return "Nothing. None of your quests are new to them" +
                   std::string(cannot.empty() ? "" : " (some can't be shared with them)") + "; tell them.";
        }
        // One share per second: quest dialogs replace each other on the client.
        time_t at = now;
        std::string names;
        for (Quest const* q : toShare)
        {
            _pending.push_back({ bot->GetGUID(), owner->GetGUID(), "share " + QuestLink(q), at, at + 30, false });
            ++at;
            names += (names.empty() ? "\"" : ", \"") + q->GetTitle() + "\"";
        }
        return "You shared " + std::to_string(toShare.size()) + (toShare.size() == 1 ? " quest" : " quests") + " with them: " + names + ".";
    }

    std::string CompanionControl::AttackAll(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        Group* group = bot->GetGroup();
        if (!group || !group->IsMember(owner->GetGUID()))
            return "Nothing. You can only pick fights together when you are in the same group; tell them.";
        Hunt& h = _hunts[owner->GetGUID()];
        bool fresh = h.bots.empty() || h.forLead || now > h.until;
        if (fresh)
        {
            ReleaseChasers(h);
            h = Hunt();
            h.owner = owner->GetGUID();
            h.filter = cmd.arg;
            h.creatureType = CreatureTypeFromWord(cmd.arg);
            h.radius = std::min(_cfg.commandRange, 45.0f);
            h.until = now + 300;
        }
        if (std::find(h.bots.begin(), h.bots.end(), bot->GetGUID()) == h.bots.end())
            h.bots.push_back(bot->GetGUID());
        if (!HuntStep(h, now))
        {
            ReleaseChasers(h);
            _hunts.erase(owner->GetGUID());
            return cmd.arg.empty() ? "Nothing. There are no enemies around here; tell them."
                                   : "Nothing. There are no \"" + cmd.arg + "\" around here; tell them.";
        }
        if (PlayerbotsBridge::IsHealer(bot))
            return "The group is clearing the area" + (cmd.arg.empty() ? std::string() : " of " + cmd.arg) +
                   ": you mark one enemy after another with a skull. You are the healer, so you don't attack yourself; "
                   "you keep everyone alive. Tell them.";
        return "You are clearing the area" + (cmd.arg.empty() ? std::string() : " of " + cmd.arg) +
               ": you mark one enemy after another with a skull and attack it, until none are left.";
    }

    std::string CompanionControl::StopAttack(Player* owner, Player* bot)
    {
        auto it = _hunts.find(owner->GetGUID());
        bool clearing = it != _hunts.end() && !it->second.forLead && it->second.only.IsEmpty();
        if (it != _hunts.end() && !it->second.forLead && !clearing)
        {
            ReleaseChasers(it->second);   // a single attack that was still walking closer
            _hunts.erase(it);
            it = _hunts.end();
        }
        if (clearing)
        {
            if (Group* group = bot->GetGroup())
                group->SetTargetIcon(kSkullIcon, owner->GetGUID(), ObjectGuid::Empty);
            ReleaseChasers(it->second);
            _hunts.erase(it);
        }
        PlayerbotsBridge::StopAttack(bot, owner);
        if (!_leads.count(bot->GetGUID()))
            PlayerbotsBridge::Command(bot, owner, "follow");   // back to the player (a leading companion keeps leading)
        return clearing ? "You stop clearing the area, drop your target and follow them again."
                        : "You stop attacking, drop your target and follow them again.";
    }

    bool CompanionControl::HuntStep(Hunt& h, time_t now)
    {
        Player* owner = ObjectAccessor::FindConnectedPlayer(h.owner);
        if (!owner || now > h.until)
            return false;
        std::vector<Player*> bots;
        for (ObjectGuid g : h.bots)
            if (Player* b = ObjectAccessor::FindConnectedPlayer(g))
                if (b->IsInWorld() && b->IsAlive() && b->GetMapId() == owner->GetMapId())
                    bots.push_back(b);
        if (bots.empty())
            return false;
        Group* group = bots.front()->GetGroup();
        if (!group)
            return false;

        Unit* current = h.current ? ObjectAccessor::GetUnit(*bots.front(), h.current) : nullptr;
        // Healers keep healing (Playerbots refuses attack orders for them); everyone else attacks.
        auto order = [&](Player* b, bool force)
        {
            if (PlayerbotsBridge::IsHealer(b))
                return;
            if (current && (b->GetVictim() == current || !b->IsWithinLOSInMap(current)))
                return;   // already on it, or Playerbots would answer "attack rti target: failed"
            bool engaged = b->IsInCombat() && current && (b->GetTarget() == current->GetGUID() || b->GetVictim() == current);
            time_t& last = h.lastOrder[b->GetGUID()];
            if (!force && (engaged || now - last < 5))
                return;
            last = now;
            if (force)
                PlayerbotsBridge::SetRti(b, "skull");
            PlayerbotsBridge::Command(b, owner, "d attack rti target");
        };
        if (current && current->IsAlive() && bots.front()->IsValidAttackTarget(current))
        {
            bool engaged = current->IsInCombat();
            for (Player* b : bots)
                engaged = engaged || (b->IsInCombat() && (b->GetTarget() == current->GetGUID() || b->GetVictim() == current));
            if (engaged || now - h.currentSince < 15)
            {
                for (Player* b : bots)
                    order(b, false);   // keep everyone on the marked enemy
                return true;
            }
            // nobody got to it (behind a wall, no path): leave it and take the next one
            h.skipped.insert(current->GetGUID());
            current = nullptr;
            h.current = ObjectGuid::Empty;
        }
        else if (h.current)
            ++h.kills;

        Player* anchor = bots.front();
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(anchor, anchor, h.radius);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(anchor, units, check);
        Cell::VisitObjects(anchor, searcher, h.radius);
        Unit* next = nullptr;
        float bestDist = 0;
        for (Unit* u : units)
        {
            Creature* c = u->ToCreature();
            if (!c || !c->IsAlive() || !anchor->IsValidAttackTarget(c) || c->IsCivilian())
                continue;
            if (c->hasLootRecipient() && !c->isTappedBy(owner))
                continue;   // someone else's fight
            if (h.skipped.count(c->GetGUID()))
                continue;
            if (!h.only.IsEmpty())
            {
                if (c->GetGUID() != h.only)
                    continue;
            }
            else if (!h.entries.empty())
            {
                if (!h.entries.count(c->GetEntry()))
                    continue;
            }
            else if (h.creatureType)
            {
                if (c->GetCreatureType() != h.creatureType)
                    continue;
            }
            else if (!h.filter.empty())
            {
                if (MatchScore(c->GetName(), h.filter) == 0)
                    continue;
            }
            else if (c->GetCreatureType() == CREATURE_TYPE_CRITTER)
                continue;
            // enemies she can see first; ones behind walls only when nothing else is left
            float d = anchor->GetDistance(c) + (anchor->IsWithinLOSInMap(c) ? 0.0f : 1000.0f);
            if (!next || d < bestDist)
            {
                next = c;
                bestDist = d;
            }
        }
        if (!next)
        {
            h.current = ObjectGuid::Empty;
            return false;
        }
        if (!anchor->IsWithinLOSInMap(next))
        {
            // Out of sight (behind a wall or a hill): walk closer first, give up on it after 20 seconds.
            if (h.chase != next->GetGUID())
            {
                h.chase = next->GetGUID();
                h.chaseSince = now;
                h.lastChase = 0;
            }
            if (now - h.chaseSince > 20)
            {
                h.skipped.insert(next->GetGUID());
                h.chase = ObjectGuid::Empty;
                return true;
            }
            if (now - h.lastChase >= 3)
            {
                h.lastChase = now;
                for (Player* b : bots)
                    if (!PlayerbotsBridge::IsHealer(b) && !b->IsInCombat() && PlayerbotsBridge::FollowState(b) != "stay")
                    {
                        // Playerbots' "follow" would pull her straight back to the player: off while she walks closer
                        if (PlayerbotsBridge::FollowState(b) == "follow" && PlayerbotsBridge::SetFollow(b, owner, false))
                            h.unfollowed.insert(b->GetGUID());
                        b->GetMotionMaster()->MovePoint(0, next->GetPositionX(), next->GetPositionY(), next->GetPositionZ());
                    }
            }
            return true;
        }
        h.chase = ObjectGuid::Empty;
        h.currentSince = now;
        h.current = next->GetGUID();
        current = next;
        group->SetTargetIcon(kSkullIcon, owner->GetGUID(), next->GetGUID());
        for (Player* b : bots)
            order(b, true);
        ReleaseChasers(h);
        return true;
    }

    std::string CompanionControl::AcceptQuests(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        bool classOnly = cmd.arg == "class";
        auto offeredMap = QuestNpcs(bot, _cfg.interactRange, false);
        // nearest quest giver first ("pick up a quest" takes the nearest one)
        std::vector<std::pair<uint32, Creature*>> offered(offeredMap.begin(), offeredMap.end());
        std::stable_sort(offered.begin(), offered.end(),
                         [bot](auto const& a, auto const& b) { return bot->GetDistance(a.second) < bot->GetDistance(b.second); });
        size_t limit = (cmd.all || !cmd.arg.empty()) ? 8 : 1;
        std::vector<Quest const*> take;
        std::vector<std::string> cannot;
        for (int pass = 0; pass < 2 && take.empty(); ++pass)
            for (auto const& [id, npc] : offered)
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(id);
                if (!q || bot->FindQuestSlot(id) < MAX_QUEST_LOG_SIZE || bot->GetQuestRewardStatus(id))
                    continue;
                if (classOnly ? !IsClassQuest(q) : (!cmd.arg.empty() && MatchScore(q->GetTitle(), cmd.arg) == 0))
                    continue;
                // first the quests they have taken themselves, then (if none) everything offered here
                if (cmd.arg.empty() && pass == 0 && owner->FindQuestSlot(id) >= MAX_QUEST_LOG_SIZE)
                    continue;
                if (!bot->CanTakeQuest(q, false) || !bot->CanAddQuest(q, false))
                {
                    if (pass == 0 || !cmd.arg.empty())
                        cannot.push_back(q->GetTitle());
                    continue;
                }
                take.push_back(q);
                if (take.size() >= limit)
                    break;
            }
        std::string names;
        for (Quest const* q : take)
        {
            bot->AddQuestAndCheckCompletion(q, nullptr);
            names += (names.empty() ? "\"" : ", \"") + q->GetTitle() + "\"";
        }
        std::string refused;
        for (auto const& t : cannot)
            refused += (refused.empty() ? "\"" : ", \"") + t + "\"";

        // "pick up all your quests": after the ones here, go on to the other quest givers around
        std::string onward;
        if (cmd.all || take.empty())
        {
            auto givers = AvailableGivers(bot, kAreaRadius, cmd.arg);
            if (!givers.empty() && _cfg.leadMode && PlayerbotsBridge::Available())
            {
                GiverInfo const& g = givers.front();
                if (BeginErrand(owner, bot, g.entry, g.name, g.x, g.y, g.z, false, cmd.arg, cmd.all, now))
                {
                    if (!take.empty())
                        onward = " Then you walk on to " + g.name + " (about " + std::to_string(int(g.dist)) + " yards " +
                                 Direction(bot, g.x, g.y) + ") for more: " + QuestList(g.quests) + ", and come back to them.";
                    else
                        return "You are going over to " + g.name + " (about " + std::to_string(int(g.dist)) + " yards " +
                               Direction(bot, g.x, g.y) + ") to pick up " + QuestList(g.quests) +
                               (cmd.all ? " and the other quests around" : "") + ", then you come back to them. Tell them.";
                }
            }
        }
        if (take.empty())
        {
            if (!refused.empty())
                return "Nothing. You cannot take " + refused + " (level, class or an earlier quest missing); tell them.";
            return classOnly ? "Nothing. There is no class quest for you around here right now; tell them."
                             : "Nothing. No quest giver around here has a quest you could take; tell them.";
        }
        return "You took " + names + " from the quest giver." + (refused.empty() ? "" : " You could not take " + refused + ".") + onward;
    }

    std::string CompanionControl::TurnInQuests(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        auto enders = QuestNpcs(bot, _cfg.interactRange, true);
        std::string done, notReady;
        for (auto const& [q, slot] : QuestLog(bot))
        {
            if (!cmd.arg.empty() && MatchScore(q->GetTitle(), cmd.arg) == 0)
                continue;
            auto it = enders.find(q->GetQuestId());
            if (it == enders.end())
                continue;
            if (bot->GetQuestStatus(q->GetQuestId()) != QUEST_STATUS_COMPLETE || !bot->CanRewardQuest(q, false))
            {
                notReady += (notReady.empty() ? "\"" : ", \"") + q->GetTitle() + "\"";
                continue;
            }
            bot->RewardQuest(q, BestRewardChoice(bot, q), it->second, false);
            done += (done.empty() ? "\"" : ", \"") + q->GetTitle() + "\"";
        }
        std::vector<Quest const*> ready;
        if (done.empty())
            for (auto const& [q, slot] : QuestLog(bot))
                if ((cmd.arg.empty() || MatchScore(q->GetTitle(), cmd.arg) > 0) &&
                    bot->GetQuestStatus(q->GetQuestId()) == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(q, false))
                    ready.push_back(q);
        if (done.empty() && ready.empty())
            return notReady.empty() ? "Nothing. None of your quests is finished yet; tell them."
                                    : "Nothing. " + notReady + " is not finished yet; tell them what is still missing.";
        if (done.empty())
        {
            // nobody here takes them: walk to the nearest quest ender in the area, turn in there, come back
            GiverInfo const* best = nullptr;
            auto enderList = QuestEnders(bot, kAreaRadius, ready);
            if (!enderList.empty())
                best = &enderList.front();
            if (!best)
                return "Nothing. " + QuestList(ready) + (ready.size() == 1 ? " is" : " are") + " finished, but the quest giver "
                       "is not around here; tell them where to go is unknown to you and suggest turning it in together later.";
            if (!_cfg.leadMode || !PlayerbotsBridge::Available())
                return "Nothing yet. You can turn in " + QuestList(best->quests) + " at " + best->name + " (about " +
                       std::to_string(int(best->dist)) + " yards " + Direction(bot, best->x, best->y) + "); tell them.";
            Lead lead;
            lead.owner = owner->GetGUID();
            lead.fetch = true;
            lead.turnIn = true;
            lead.fetchEntry = best->entry;
            lead.fetchFilter = cmd.arg;
            lead.title = best->name;
            lead.since = now;
            lead.map = bot->GetMapId();
            lead.x = best->x;
            lead.y = best->y;
            lead.z = best->z;
            lead.haveZ = true;
            lead.lastX = bot->GetPositionX();
            lead.lastY = bot->GetPositionY();
            if (!PlayerbotsBridge::StartLead(bot, owner))
                return "Nothing. You could not go over there right now; tell them.";
            _leads[bot->GetGUID()] = lead;
            return "You are going over to " + best->name + " (about " + std::to_string(int(best->dist)) + " yards " +
                   Direction(bot, best->x, best->y) + ") to turn in " + QuestList(best->quests) + ", then you come back to them. Tell them.";
        }
        return "You turned in " + done + "." + (notReady.empty() ? "" : " Not finished yet: " + notReady + ".");
    }

    std::string CompanionControl::SyncQuestAccept(Player* owner, Player* bot, uint32 questId)
    {
        if (!_cfg.controlEnabled || !_cfg.questSync || !Near(owner, bot, _cfg.interactRange + 10) || !bot->IsAlive())
            return "";
        Quest const* q = sObjectMgr->GetQuestTemplate(questId);
        if (!q || bot->FindQuestSlot(questId) < MAX_QUEST_LOG_SIZE || bot->GetQuestRewardStatus(questId))
            return "";
        if (!bot->CanTakeQuest(q, false) || !bot->CanAddQuest(q, false))
            return "";
        bot->AddQuestAndCheckCompletion(q, nullptr);
        return "You took the quest \"" + q->GetTitle() + "\" too, together with them.";
    }

    std::string CompanionControl::SyncQuestReward(Player* owner, Player* bot, uint32 questId)
    {
        if (!_cfg.controlEnabled || !_cfg.questSync || !Near(owner, bot, _cfg.interactRange + 10) || !bot->IsAlive())
            return "";
        Quest const* q = sObjectMgr->GetQuestTemplate(questId);
        if (!q || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE || !bot->CanRewardQuest(q, false))
            return "";
        auto enders = QuestNpcs(bot, _cfg.interactRange, true);
        auto it = enders.find(questId);
        if (it == enders.end())
            return "";
        bot->RewardQuest(q, BestRewardChoice(bot, q), it->second, false);
        return "";   // the quest-complete event already makes her comment
    }

    std::string CompanionControl::StartLead(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        if (!_cfg.leadMode || !PlayerbotsBridge::Available())
            return "Nothing. Leading the way is not possible here; you keep following them. Tell them.";
        if (auto cur = _leads.find(bot->GetGUID()); cur != _leads.end())
        {
            if (cur->second.fetch)
                return "Nothing yet. You are still on your way to " + cur->second.title + (cur->second.turnIn ? " to turn in quests" : " to pick up quests") +
                       "; you can lead them as soon as you are back. Tell them.";
            // "lead again" while a lead is paused: continue it
            if (cmd.arg.empty())
                return ResumeLead(owner, bot);
        }
        // one companion leads; the others follow the group
        for (auto const& [other, l] : _leads)
            if (other != bot->GetGUID() && l.owner == owner->GetGUID() && !l.fetch)
            {
                Player* leader = ObjectAccessor::FindConnectedPlayer(other);
                return "Nothing. " + (leader ? leader->GetName() : std::string("Another companion")) + " is already leading the way to \"" +
                       l.title + "\"; you follow along with the group. Say so in a few words.";
            }
        auto log = QuestLog(bot);
        Quest const* pick = nullptr;
        if (!cmd.arg.empty())
        {
            int bestScore = 0;
            for (auto const& [q, slot] : log)
            {
                int sc = MatchScore(q->GetTitle(), cmd.arg);
                if (sc > bestScore)
                {
                    pick = q;
                    bestScore = sc;
                }
            }
            if (!pick)
                return "Nothing. They want to do \"" + cmd.arg + "\", but that quest is not in your quest log; tell them and ask which one.";
        }
        else
        {
            // Prefer an unfinished quest you both have, then any unfinished one.
            for (int pass = 0; pass < 2 && !pick; ++pass)
                for (auto const& [q, slot] : log)
                {
                    if (bot->GetQuestStatus(q->GetQuestId()) != QUEST_STATUS_INCOMPLETE)
                        continue;
                    if (pass == 0 && owner->FindQuestSlot(q->GetQuestId()) >= MAX_QUEST_LOG_SIZE)
                        continue;
                    pick = q;
                    break;
                }
            if (!pick)
            {
                std::string list;
                for (auto const& [q, slot] : log)
                    list += (list.empty() ? "\"" : ", \"") + q->GetTitle() + "\"";
                return list.empty() ? "Nothing. You have no quest to lead them to; ask them what they would like to do."
                                    : "Nothing yet. All your quests (" + list + ") are done or ready to turn in; ask them what to do next.";
            }
        }
        if (bot->GetQuestStatus(pick->GetQuestId()) == QUEST_STATUS_COMPLETE)
            return "Nothing. \"" + pick->GetTitle() + "\" is already finished and only needs to be turned in; tell them.";
        if (auto cur = _leads.find(bot->GetGUID()); cur != _leads.end() && !cur->second.fetch && cur->second.quest == pick->GetQuestId())
            return ResumeLead(owner, bot);

        Lead lead;
        lead.owner = owner->GetGUID();
        lead.quest = pick->GetQuestId();
        lead.title = pick->GetTitle();
        lead.since = now;
        if (!QuestDestination(bot, pick, lead.map, lead.x, lead.y, lead.z, lead.haveZ))
            return "Nothing. You don't know where \"" + pick->GetTitle() + "\" takes place (it is not on this continent or has no "
                   "known location); tell them and suggest they lead.";
        if (!PlayerbotsBridge::StartLead(bot, owner))
            return "Nothing. You could not take the lead right now; tell them.";
        lead.lastX = bot->GetPositionX();
        lead.lastY = bot->GetPositionY();
        float dist = bot->GetDistance2d(lead.x, lead.y);
        _leads[bot->GetGUID()] = lead;
        LeadStep(owner, bot, _leads[bot->GetGUID()], now);
        std::string where = TargetEntries(bot, pick).empty() ? "the area of" : "where the targets of";
        return "You took the lead and are walking to " + where + " your quest \"" + pick->GetTitle() + "\" (about " +
               std::to_string(int(dist)) + " yards away); they follow you. You wait if they fall behind. Tell them where you are going.";
    }

    std::string CompanionControl::ResumeLead(Player* owner, Player* bot)
    {
        Lead& lead = _leads[bot->GetGUID()];
        bool wasPaused = lead.paused;
        lead.paused = false;
        lead.waiting = false;
        lead.stuck = 0;
        lead.lastMove = 0;
        lead.lastX = bot->GetPositionX();
        lead.lastY = bot->GetPositionY();
        PlayerbotsBridge::StartLead(bot, owner);   // drops a Playerbots "stay" that may have been set
        return wasPaused ? "You continue leading the way to \"" + lead.title + "\"."
                         : "You are already leading the way to \"" + lead.title + "\"; keep going.";
    }

    std::string CompanionControl::StopLead(Player* owner, Player* bot)
    {
        bool was = _leads.erase(bot->GetGUID()) > 0;
        auto h = _hunts.find(owner->GetGUID());
        if (h != _hunts.end() && h->second.forLead)
        {
            ReleaseChasers(h->second);
            _hunts.erase(h);
        }
        bot->GetMotionMaster()->Clear();
        PlayerbotsBridge::StopLead(bot, owner);
        return was ? "You stopped leading and follow them again." : "You follow them (you were not leading).";
    }

    bool CompanionControl::BeginErrand(Player* owner, Player* bot, uint32 entry, std::string const& name, float x, float y, float z,
                                       bool turnIn, std::string const& filter, bool all, time_t now)
    {
        if (!_cfg.leadMode || !PlayerbotsBridge::Available() || !PlayerbotsBridge::StartLead(bot, owner))
            return false;
        Lead lead;
        lead.owner = owner->GetGUID();
        lead.fetch = true;
        lead.turnIn = turnIn;
        lead.fetchAll = all;
        lead.fetchEntry = entry;
        lead.fetchFilter = filter;
        lead.title = name;
        lead.since = now;
        lead.map = bot->GetMapId();
        lead.x = x;
        lead.y = y;
        lead.z = z;
        lead.haveZ = true;
        lead.lastX = bot->GetPositionX();
        lead.lastY = bot->GetPositionY();
        _leads[bot->GetGUID()] = lead;
        return true;
    }

    void CompanionControl::FinishLead(Player* owner, Player* bot, std::string const& notice)
    {
        if (_cfg.debugLogging)
            LOG_INFO("module", "[mod-romance] {} stops leading: {}", bot->GetName(), notice.empty() ? "(no notice)" : notice);
        _leads.erase(bot->GetGUID());
        auto h = _hunts.find(owner->GetGUID());
        if (h != _hunts.end() && h->second.forLead)
        {
            ReleaseChasers(h->second);
            _hunts.erase(h);
        }
        bot->GetMotionMaster()->Clear();
        PlayerbotsBridge::StopLead(bot, owner);
        if (!notice.empty())
            _notices.push_back({ bot->GetGUID(), notice });
    }

    void CompanionControl::LeadStep(Player* owner, Player* bot, Lead& lead, time_t now)
    {
        if (lead.fetch)
        {
            if (bot->IsInCombat() || !bot->IsAlive())
                return;
            if (now - lead.since > 240)
            {
                FinishLead(owner, bot, "You could not reach " + lead.title + (lead.turnIn ? " to turn in your quests" : " to pick up quests") +
                                       " and come back to them; tell them.");
                return;
            }
            if (lead.turnIn && bot->GetDistance2d(lead.x, lead.y) < 5.0f)
            {
                std::vector<Quest const*> handed;
                for (auto const& [questId, npc] : QuestNpcs(bot, 30.0f, true))
                {
                    if (npc->GetEntry() != lead.fetchEntry)
                        continue;
                    Quest const* q = sObjectMgr->GetQuestTemplate(questId);
                    if (!q || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE || !bot->CanRewardQuest(q, false))
                        continue;
                    if (!lead.fetchFilter.empty() && MatchScore(q->GetTitle(), lead.fetchFilter) == 0)
                        continue;
                    bot->RewardQuest(q, BestRewardChoice(bot, q), npc, false);
                    handed.push_back(q);
                }
                FinishLead(owner, bot, handed.empty()
                    ? "You reached " + lead.title + " but could not turn anything in there. You are coming back to them; tell them."
                    : "You turned in " + QuestList(handed) + " at " + lead.title + " and are coming back to them. Tell them briefly.");
                return;
            }
            if (bot->GetDistance2d(lead.x, lead.y) < 5.0f)
            {
                std::vector<Quest const*> taken;
                size_t limit = (lead.fetchAll || !lead.fetchFilter.empty()) ? 99 : 1;
                for (GiverInfo const& g : AvailableGivers(bot, 30.0f, lead.fetchFilter))
                    if (g.entry == lead.fetchEntry)
                        for (Quest const* q : g.quests)
                            if (taken.size() < limit && CanTakeNow(bot, q))
                            {
                                bot->AddQuestAndCheckCompletion(q, nullptr);
                                taken.push_back(q);
                                lead.taken.push_back(q->GetTitle());
                            }
                lead.visited.insert(lead.fetchEntry);
                if (lead.fetchAll)
                {
                    // on to the next quest giver around, if any
                    for (GiverInfo const& g : AvailableGivers(bot, kAreaRadius, lead.fetchFilter))
                        if (!lead.visited.count(g.entry))
                        {
                            lead.fetchEntry = g.entry;
                            lead.title = g.name;
                            lead.x = g.x;
                            lead.y = g.y;
                            lead.z = g.z;
                            lead.since = now;
                            lead.lastMove = 0;
                            lead.stuck = 0;
                            return;
                        }
                }
                std::string all;
                for (std::string const& t : lead.taken)
                    all += (all.empty() ? "\"" : ", \"") + t + "\"";
                FinishLead(owner, bot, lead.taken.empty()
                    ? "You reached " + lead.title + " but there was no quest left for you. You are coming back to them; tell them."
                    : "You picked up " + all + " and are coming back to them. Tell them briefly.");
                return;
            }
        }
        Quest const* q = lead.fetch ? nullptr : sObjectMgr->GetQuestTemplate(lead.quest);
        if (!lead.fetch && (!q || bot->FindQuestSlot(lead.quest) >= MAX_QUEST_LOG_SIZE))
        {
            FinishLead(owner, bot, "");
            return;
        }
        if (!lead.fetch && bot->GetQuestStatus(lead.quest) == QUEST_STATUS_COMPLETE)
        {
            // walk to the quest ender (if one is around), turn the quest in and come back
            std::string title = lead.title;
            auto enders = QuestEnders(bot, kAreaRadius, { q });
            if (!enders.empty())
            {
                GiverInfo const& g = enders.front();
                auto hunt = _hunts.find(owner->GetGUID());
                if (hunt != _hunts.end() && hunt->second.forLead)
                {
                    ReleaseChasers(hunt->second);
                    _hunts.erase(hunt);
                }
                bot->GetMotionMaster()->Clear();
                if (BeginErrand(owner, bot, g.entry, g.name, g.x, g.y, g.z, true, title, false, now))
                {
                    _notices.push_back({ bot->GetGUID(), "You finished \"" + title + "\" while leading and walk to " + g.name +
                                                             " to turn it in; then you come back to them. Say it in a few words." });
                    return;
                }
            }
            FinishLead(owner, bot, "You finished the objectives of \"" + title + "\" while leading. You follow them again now; "
                                   "tell them it is done and suggest turning it in.");
            return;
        }
        if (bot->IsInCombat() || !bot->IsAlive())
            return;   // fighting is Playerbots' job
        // "wait" (ours) or a plain Playerbots "stay": hold position, keep the lead, never count as stuck
        if (lead.paused || PlayerbotsBridge::FollowState(bot) == "stay")
        {
            lead.paused = true;
            lead.stuck = 0;
            lead.lastMove = 0;
            return;
        }

        // never run away from them: wait when they fall behind
        // (fetching she goes alone and comes back, so she doesn't wait)
        bool ownerNear = lead.fetch || (owner->GetMapId() == bot->GetMapId() && bot->GetDistance(owner) < 40.0f);
        if (!ownerNear)
        {
            if (!lead.waiting)
            {
                lead.waiting = true;
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
            }
            return;
        }
        if (lead.waiting)
        {
            if (bot->GetDistance(owner) > 20.0f)
                return;
            lead.waiting = false;
            lead.lastMove = 0;
        }

        if (lead.hunting)
        {
            auto h = _hunts.find(owner->GetGUID());
            if (h == _hunts.end() || !h->second.forLead)
                FinishLead(owner, bot, "You reached the area of \"" + lead.title + "\" but found no more of the creatures you need "
                                       "right now. You follow them again; tell them and suggest waiting for them to come back.");
            return;
        }

        float dist = bot->GetDistance2d(lead.x, lead.y);
        if (!lead.fetch && dist < 25.0f)
        {
            std::set<uint32> entries = TargetEntries(bot, q);
            if (entries.empty())
            {
                std::string what;
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    if (!q->ObjectiveText[i].empty())
                        what += (what.empty() ? "" : "; ") + q->ObjectiveText[i];
                FinishLead(owner, bot, "You arrived at the area of \"" + lead.title + "\". " +
                                           (what.empty() ? std::string("") : "What still needs doing: " + what + ". ") +
                                           "You follow them again; tell them you are there and what to look for.");
                return;
            }
            Hunt h;
            h.owner = owner->GetGUID();
            h.bots = { bot->GetGUID() };
            // the other bots of the group fight along
            if (Group* g = owner->GetGroup())
                for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                    if (Player* m = ref->GetSource())
                        if (m != bot && m != owner && PlayerbotsBridge::MasterOf(m) == owner)
                            h.bots.push_back(m->GetGUID());
            h.entries = entries;
            h.radius = 60.0f;
            h.until = now + 900;
            h.forLead = true;
            _hunts[owner->GetGUID()] = h;
            lead.hunting = true;
            _notices.push_back({ bot->GetGUID(), "You arrived where the creatures for \"" + lead.title +
                                                     "\" are and start fighting them. Say it in a few words." });
            return;
        }

        // walk in steps of up to 60 yards (path finding works best on short legs)
        float moved = std::hypot(bot->GetPositionX() - lead.lastX, bot->GetPositionY() - lead.lastY);
        bool moving = bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE && moved > 0.5f;
        if (moving && now - lead.lastMove < 8)
        {
            lead.lastX = bot->GetPositionX();
            lead.lastY = bot->GetPositionY();
            return;
        }
        if (now - lead.lastMove < 2)
            return;
        lead.stuck = moved < 1.0f && lead.lastMove ? lead.stuck + 1 : 0;
        if (lead.stuck >= 8)
        {
            FinishLead(owner, bot, "You could not find a way to the area of \"" + lead.title +
                                       "\". You follow them again; tell them and ask them to lead instead.");
            return;
        }
        float step = std::min(dist, lead.stuck >= 3 ? 25.0f : 60.0f);
        float dx = (lead.x - bot->GetPositionX()) / dist, dy = (lead.y - bot->GetPositionY()) / dist;
        float nx = bot->GetPositionX() + dx * step, ny = bot->GetPositionY() + dy * step;
        float nz;
        if (lead.haveZ && step >= dist - 0.1f)
            nz = lead.z;
        else
        {
            nz = bot->GetMapHeight(nx, ny, bot->GetPositionZ() + 30.0f, true, 80.0f);
            if (nz <= INVALID_HEIGHT + 1.0f)
                nz = bot->GetPositionZ();
        }
        bot->GetMotionMaster()->MovePoint(0, nx, ny, nz);
        lead.lastMove = now;
        lead.lastX = bot->GetPositionX();
        lead.lastY = bot->GetPositionY();
    }

    // ------------------------------------------------------------------ background

    void CompanionControl::Update(time_t now)
    {
        for (auto it = _pending.begin(); it != _pending.end();)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(it->bot);
            Player* owner = ObjectAccessor::FindConnectedPlayer(it->owner);
            if (!bot || !owner || now > it->deadline)
            {
                it = _pending.erase(it);
                continue;
            }
            if (now < it->notBefore || (it->needsTrade && bot->GetTrader() != owner))
            {
                ++it;
                continue;
            }
            PlayerbotsBridge::Command(bot, owner, it->command);
            it = _pending.erase(it);
        }

        if (now == _lastStep)
            return;   // area clearing and lead mode step once per second
        _lastStep = now;
        for (auto it = _hunts.begin(); it != _hunts.end();)
        {
            if (HuntStep(it->second, now))
            {
                ++it;
                continue;
            }
            Hunt& h = it->second;
            ReleaseChasers(h);
            if (!h.forLead && h.only.IsEmpty() && !h.bots.empty())
                _notices.push_back({ h.bots.front(), h.kills
                    ? "You cleared the area (" + std::to_string(h.kills) + (h.kills == 1 ? " enemy" : " enemies") + "). Say it briefly."
                    : "" });
            if (Player* owner = ObjectAccessor::FindConnectedPlayer(h.owner))
                if (Group* g = owner->GetGroup())
                    g->SetTargetIcon(kSkullIcon, owner->GetGUID(), ObjectGuid::Empty);
            it = _hunts.erase(it);
        }
        for (auto it = _leads.begin(); it != _leads.end();)
        {
            ObjectGuid botGuid = it->first;
            Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
            Player* owner = ObjectAccessor::FindConnectedPlayer(it->second.owner);
            if (!bot || !owner || !bot->IsInWorld())
            {
                it = _leads.erase(it);
                continue;
            }
            ++it;   // LeadStep may erase this lead
            auto cur = _leads.find(botGuid);
            LeadStep(owner, bot, cur->second, now);
        }
        _notices.erase(std::remove_if(_notices.begin(), _notices.end(), [](auto const& n) { return n.second.empty(); }), _notices.end());
    }

    std::vector<std::pair<ObjectGuid, std::string>> CompanionControl::TakeNotices()
    {
        std::vector<std::pair<ObjectGuid, std::string>> out;
        out.swap(_notices);
        return out;
    }

    std::string CompanionControl::Poll(Player* owner, Player* bot, time_t now)
    {
        if (!_cfg.controlEnabled || !owner || !bot || !bot->IsAlive())
            return "";
        ObjectGuid g = bot->GetGUID();

        // bags nearly full
        uint32 freeSlots = bot->GetFreeInventorySpace();
        if (freeSlots <= _cfg.autoSellFreeSlots && !bot->IsInCombat())
        {
            Creature* vendor = _cfg.autoSellJunk ? NearbyVendor(bot, _cfg.interactRange) : nullptr;
            std::vector<Item*> junk = vendor ? JunkItems(bot) : std::vector<Item*>();
            if (vendor && !junk.empty() && now - _lastAutoSell[g] > kAutoSellCooldown)
            {
                _lastAutoSell[g] = now;
                uint32 n = 0;
                uint64 value = SellItems(bot, junk, &n);
                return "Your bags were almost full, so you sold " + std::to_string(n) + " junk items to " + vendor->GetName() +
                       " for " + Money(value) + ". Mention it briefly.";
            }
            if (now - _lastBagsFull[g] > kBagsFullCooldown)
            {
                _lastBagsFull[g] = now;
                return freeSlots == 0 ? "Your bags are completely full; you can't pick up anything else. Mention it and suggest visiting a merchant."
                                      : "Your bags are almost full (" + std::to_string(freeSlots) + " slots left). Mention it briefly.";
            }
        }
        return "";
    }

    void CompanionControl::Forget(ObjectGuid bot)
    {
        _leads.erase(bot);
        _lastAutoSell.erase(bot);
        _lastBagsFull.erase(bot);
        for (auto& [owner, h] : _hunts)
            h.bots.erase(std::remove(h.bots.begin(), h.bots.end(), bot), h.bots.end());
        _pending.erase(std::remove_if(_pending.begin(), _pending.end(), [&](Pending const& p) { return p.bot == bot; }), _pending.end());
    }

    // ------------------------------------------------------------------ character sheet

    Json CompanionControl::CharacterSheet(Player* owner, Player* bot) const
    {
        Json j = Json::MakeObject();
        j.Set("name", bot->GetName());
        j.Set("level", int64_t(bot->GetLevel()));
        j.Set("class", int64_t(bot->getClass()));
        j.Set("race", int64_t(bot->getRace()));
        j.Set("xp", int64_t(bot->GetUInt32Value(PLAYER_XP)));
        j.Set("xpNext", int64_t(bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)));
        j.Set("money", int64_t(bot->GetMoney()));
        j.Set("health", int64_t(bot->GetHealthPct()));
        j.Set("freeSlots", int64_t(bot->GetFreeInventorySpace()));
        j.Set("totalSlots", int64_t(TotalBagSlots(bot)));
        j.Set("follow", PlayerbotsBridge::FollowState(bot));
        auto lead = _leads.find(bot->GetGUID());
        if (lead != _leads.end())
            j.Set("leadQuest", lead->second.title);
        if (lead != _leads.end())
            j.Set("follow", std::string("lead"));

        Json items = Json::MakeArray();
        for (ItemGroup const& g : GroupedBagItems(bot))
        {
            Json it = Json::MakeObject();
            it.Set("entry", int64_t(g.proto->ItemId));
            it.Set("name", g.proto->Name1);
            it.Set("count", int64_t(g.count));
            it.Set("quality", int64_t(g.proto->Quality));
            it.Set("soulbound", g.soulbound);
            items.Push(it);
        }
        j.Set("items", items);

        Json gear = Json::MakeArray();
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (ItemTemplate const* proto = item->GetTemplate())
                {
                    Json it = Json::MakeObject();
                    it.Set("slot", int64_t(slot));
                    it.Set("entry", int64_t(proto->ItemId));
                    it.Set("name", proto->Name1);
                    it.Set("quality", int64_t(proto->Quality));
                    gear.Push(it);
                }
        j.Set("gear", gear);

        Json spells = Json::MakeArray();
        for (SpellInfo const* info : KnownSpells(bot))
        {
            Json s = Json::MakeObject();
            s.Set("id", int64_t(info->Id));
            s.Set("name", std::string(info->SpellName[0]));
            spells.Push(s);
        }
        j.Set("spells", spells);

        Json quests = Json::MakeArray();
        for (auto const& [q, slot] : QuestLog(bot))
        {
            Json qj = Json::MakeObject();
            qj.Set("id", int64_t(q->GetQuestId()));
            qj.Set("title", q->GetTitle());
            qj.Set("level", int64_t(q->GetQuestLevel()));
            qj.Set("progress", QuestProgress(bot, q, slot));
            qj.Set("complete", bot->GetQuestStatus(q->GetQuestId()) == QUEST_STATUS_COMPLETE);
            qj.Set("shared", owner && owner->FindQuestSlot(q->GetQuestId()) < MAX_QUEST_LOG_SIZE);
            quests.Push(qj);
        }
        j.Set("quests", quests);

        Json available = Json::MakeArray();
        for (GiverInfo const& g : AvailableGivers(bot, kAreaRadius, ""))
            for (Quest const* q : g.quests)
            {
                Json a = Json::MakeObject();
                a.Set("id", int64_t(q->GetQuestId()));
                a.Set("title", q->GetTitle());
                a.Set("level", int64_t(q->GetQuestLevel()));
                a.Set("giver", g.name);
                a.Set("distance", int64_t(g.dist));
                a.Set("direction", std::string(Direction(bot, g.x, g.y)));
                a.Set("classQuest", IsClassQuest(q));
                available.Push(a);
            }
        j.Set("available", available);
        return j;
    }
}
