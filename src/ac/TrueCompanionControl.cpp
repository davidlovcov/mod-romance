/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionControl.h"
#include "Log.h"
#include "TrueCompanionGift.h"
#include "TrueCompanionPlayerbots.h"

#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdio>
#include <set>
#include <unordered_map>

namespace truecompanion::ac
{
    namespace
    {
        constexpr uint8 kSkullIcon = 7;
        constexpr float kTradeDistance = 10.0f;
        constexpr float kHandOverDistance = 30.0f;   // gold is handed over directly, no trade window
        constexpr time_t kAutoSellCooldown = 60;
        constexpr time_t kBagsFullCooldown = 900;
        constexpr float kShareRange = 100.0f;   // quest sharing: how far from the holder a companion may be
        constexpr size_t kMaxKnowledgeItems = 30;
        constexpr size_t kMaxKnowledgeSpells = 40;
        constexpr size_t kMaxJournalQuests = 10;
        constexpr float kAssistMinHealthPct = 75.0f;   // healer assist: everyone near at least this healthy
        constexpr float kAssistMinManaPct = 30.0f;     // ... and she keeps this much mana for heals

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

        // "1 silver 68 copper": spelled out for the model (small models misread "1s 68c")
        std::string MoneyWords(uint64 copper)
        {
            uint64 g = copper / 10000, s = (copper / 100) % 100, c = copper % 100;
            std::string out;
            if (g)
                out += std::to_string(g) + " gold";
            if (s)
                out += (out.empty() ? "" : " ") + std::to_string(s) + " silver";
            if (c || out.empty())
                out += (out.empty() ? "" : " ") + std::to_string(c) + " copper";
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

        // ---- quest pickup and turn-in, the way the core's quest handler does them ----

        // Takes `q` from the quest giver `giver` (a creature or game object near her): the same checks and the same
        // AddQuestAndCheckCompletion call as the client's accept, so the giver's scripts run (escort starts, events).
        // For a shared quest `giver` is the group member who shares it (as in the core's quest sharing), or null.
        bool AcceptQuest(Player* p, Quest const* q, Object* giver)
        {
            if (!q || p->FindQuestSlot(q->GetQuestId()) < MAX_QUEST_LOG_SIZE || !p->CanTakeQuest(q, false) || !p->CanAddQuest(q, false))
                return false;
            p->AddQuestAndCheckCompletion(q, giver);
            return true;
        }

        // Turns `q` in at `ender` like the client's "complete quest": the chosen reward is validated (it must fit in
        // her bags, like for a player), then RewardQuest, then the quest ender's reward scripts (follow-up events).
        bool TurnInQuest(Player* p, Quest const* q, Creature* ender)
        {
            if (!q || !ender || p->GetQuestStatus(q->GetQuestId()) != QUEST_STATUS_COMPLETE)
                return false;
            uint32 reward = BestRewardChoice(p, q);
            if (!p->CanRewardQuest(q, reward, false))
                return false;
            p->RewardQuest(q, reward, ender, false);
            if (!sScriptMgr->OnQuestReward(p, ender, q, reward))
                ender->AI()->sQuestReward(p, q, reward);
            return true;
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

        // The living creature `entry` within `range` of `p` (the quest giver she stands at).
        Creature* CreatureNear(Player* p, uint32 entry, float range)
        {
            Creature* best = nullptr;
            for (Creature* c : NearbyCreatures(p, range))
                if (c->GetEntry() == entry && c->IsAlive() && (!best || p->GetDistance(c) < p->GetDistance(best)))
                    best = c;
            return best;
        }

        // A merchant within `range` of the companion (no need to target or walk up to him).
        Creature* NearbyVendor(Player* bot, float range)
        {
            for (Creature* c : NearbyCreatures(bot, range))
                if (c->IsVendor() && c->IsAlive() && !c->IsHostileTo(bot))
                    return c;
            return nullptr;
        }

        // Whether `p` can receive `copper` more, by the same limits the core's ModifyMoney applies (gold cap, trial cap).
        bool CanReceiveMoney(Player* p, uint64 copper)
        {
            if (copper == 0)
                return true;
            if (copper >= MAX_MONEY_AMOUNT || uint64(p->GetMoney()) + copper >= MAX_MONEY_AMOUNT)
                return false;
            if (uint32 trialCap = sWorld->getIntConfig(CONFIG_TRIAL_MONEY_CAP))
                if (p->GetSession() && p->GetSession()->IsTrialAccount() && uint64(p->GetMoney()) + copper > trialCap)
                    return false;
            return true;
        }

        // Whether a merchant buys this item stack now (the vendor window's checks that apply to her).
        bool Sellable(Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            return proto && proto->SellPrice && !item->IsInTrade() && !item->IsRefundable() && !item->m_lootGenerated;
        }

        struct SaleResult
        {
            uint64 credited = 0;   // copper she really got
            uint32 stacks = 0;     // stacks touched
            uint32 units = 0;      // items sold
            bool noRoom = false;   // she can't carry that much money: nothing was sold
        };

        // Sells `want` units of the given stacks (0 = all of them), like the vendor window: the price is worked out first,
        // the money is checked against her limits before anything leaves her bags, and only then items go and money comes in.
        // A part of a stack leaves the rest in the bag.
        SaleResult SellStacks(Player* bot, std::vector<Item*> const& items, uint32 want)
        {
            SaleResult r;
            struct Take { Item* item; uint32 count; };
            std::vector<Take> plan;
            uint64 price = 0;
            uint32 left = want;
            for (Item* item : items)
            {
                if (want && !left)
                    break;
                if (!Sellable(item))
                    continue;
                uint32 take = want ? std::min<uint32>(left, item->GetCount()) : item->GetCount();
                plan.push_back({ item, take });
                price += uint64(item->GetTemplate()->SellPrice) * take;
                if (want)
                    left -= take;
            }
            if (plan.empty())
                return r;
            if (!CanReceiveMoney(bot, price))
            {
                r.noRoom = true;
                return r;
            }
            uint32 before = bot->GetMoney();
            for (Take const& t : plan)
            {
                if (t.count >= t.item->GetCount())
                    bot->DestroyItem(t.item->GetBagSlot(), t.item->GetSlot(), true);
                else
                {
                    t.item->SetCount(t.item->GetCount() - t.count);
                    t.item->SetState(ITEM_CHANGED, bot);
                }
                ++r.stacks;
                r.units += t.count;
            }
            if (price && !bot->ModifyMoney(int32(price), false))
                LOG_ERROR("module", "[mod-true-companion] {} sold items for {} copper but the money was refused", bot->GetName(), price);
            r.credited = bot->GetMoney() > before ? uint64(bot->GetMoney() - before) : 0;
            return r;
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

        // Merchants (spawned creatures with the vendor flag), grouped by map; built once like QuestSpawns.
        std::vector<QuestSpawn> const& VendorSpawns(uint32 mapId)
        {
            static std::unordered_map<uint32, std::vector<QuestSpawn>> byMap;
            static size_t builtFor = std::numeric_limits<size_t>::max();
            auto const& all = sObjectMgr->GetAllCreatureData();
            if (builtFor != all.size())
            {
                byMap.clear();
                std::unordered_map<uint32, bool> isVendor;
                for (auto const& [spawnId, data] : all)
                {
                    auto it = isVendor.find(data.id);
                    if (it == isVendor.end())
                    {
                        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
                        it = isVendor.emplace(data.id, ct && (ct->npcflag & UNIT_NPC_FLAG_VENDOR)).first;
                    }
                    if (it->second)
                        byMap[data.mapid].push_back({ data.id, data.posX, data.posY, data.posZ });
                }
                builtFor = all.size();
            }
            static std::vector<QuestSpawn> const none;
            auto it = byMap.find(mapId);
            return it == byMap.end() ? none : it->second;
        }

        // The class trainers (creature entries) of a class. Only the entries are kept: their spells are read from the
        // trainer data on each use, so a ".reload" of trainers is seen right away and no trainer is dropped because
        // another one teaches the same spell (which trainers suit her is decided per character, at use).
        std::vector<uint32> const& ClassTrainers(uint8 classId)
        {
            static std::unordered_map<uint8, std::vector<uint32>> byClass;
            auto it = byClass.find(classId);
            if (it != byClass.end())
                return it->second;
            std::vector<uint32> list;
            if (CreatureTemplateContainer const* all = sObjectMgr->GetCreatureTemplates())
                for (auto const& [entry, ct] : *all)
                {
                    if (!(ct.npcflag & UNIT_NPC_FLAG_TRAINER))
                        continue;
                    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
                    if (trainer && trainer->GetTrainerType() == Trainer::Type::Class && trainer->GetTrainerRequirement() == classId)
                        list.push_back(entry);
                }
            std::sort(list.begin(), list.end());
            return byClass.emplace(classId, std::move(list)).first->second;
        }

        // Spawns of class trainers (any class) by map; built like the merchant index.
        std::vector<QuestSpawn> const& ClassTrainerSpawns(uint32 mapId)
        {
            static std::unordered_map<uint32, std::vector<QuestSpawn>> byMap;
            static size_t builtFor = std::numeric_limits<size_t>::max();
            auto const& all = sObjectMgr->GetAllCreatureData();
            if (builtFor != all.size())
            {
                byMap.clear();
                std::unordered_map<uint32, bool> isTrainer;
                for (auto const& [spawnId, data] : all)
                {
                    auto it = isTrainer.find(data.id);
                    if (it == isTrainer.end())
                    {
                        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
                        Trainer::Trainer* t = ct && (ct->npcflag & UNIT_NPC_FLAG_TRAINER) ? sObjectMgr->GetTrainer(data.id) : nullptr;
                        it = isTrainer.emplace(data.id, t && t->GetTrainerType() == Trainer::Type::Class).first;
                    }
                    if (it->second)
                        byMap[data.mapid].push_back({ data.id, data.posX, data.posY, data.posZ });
                }
                builtFor = all.size();
            }
            static std::vector<QuestSpawn> const none;
            auto it = byMap.find(mapId);
            return it == byMap.end() ? none : it->second;
        }

        // A trainer `p` can learn from: her class, and friendly.
        bool IsHerTrainer(Player* p, uint32 entry)
        {
            Trainer::Trainer* t = sObjectMgr->GetTrainer(entry);
            if (!t || t->GetTrainerType() != Trainer::Type::Class || !t->IsTrainerValidForPlayer(p))
                return false;
            CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry);
            FactionTemplateEntry const* mine = p->GetFactionTemplateEntry();
            if (ct && mine)
                if (FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(ct->faction))
                    if (mine->IsHostileTo(*ft))
                        return false;
            return true;
        }

        // Whether trainer `entry` suits her and has at least one spell for her right now.
        bool TeachesHerNow(Player* p, uint32 entry)
        {
            Trainer::Trainer* t = sObjectMgr->GetTrainer(entry);
            if (!t || !IsHerTrainer(p, entry))
                return false;
            for (Trainer::Spell const& sp : t->GetSpells())
                if (t->CanTeachSpell(p, &sp))
                    return true;
            return false;
        }

        // Spells any trainer of her class could teach her now, and what they cost together (without discounts). Each spell
        // counts once, from the cheapest trainer that suits her.
        uint32 TeachableNow(Player* p, std::vector<std::string>* names = nullptr)
        {
            std::map<uint32, uint32> cheapest;   // spell -> cost
            for (uint32 entry : ClassTrainers(p->getClass()))
            {
                Trainer::Trainer* t = sObjectMgr->GetTrainer(entry);
                if (!t || !IsHerTrainer(p, entry))
                    continue;
                for (Trainer::Spell const& sp : t->GetSpells())
                {
                    if (!t->CanTeachSpell(p, &sp))
                        continue;
                    auto [it, added] = cheapest.emplace(sp.SpellId, sp.MoneyCost);
                    if (!added)
                        it->second = std::min(it->second, sp.MoneyCost);
                }
            }
            uint32 cost = 0;
            for (auto const& [spellId, c] : cheapest)
            {
                cost += c;
                if (names)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                        names->push_back(info->SpellName[0]);
            }
            return cheapest.empty() ? 0 : std::max<uint32>(cost, 1);
        }

        // At the trainer: learns what she can, cheapest first, paying like Trainer::TeachSpell (reputation discount).
        // Returns the names learned; `paid` and `tooExpensive` report the rest.
        std::vector<std::string> LearnFrom(Player* p, Creature* npc, uint32& paid, uint32& tooExpensive)
        {
            std::vector<std::string> learned;
            paid = tooExpensive = 0;
            Trainer::Trainer* t = npc ? sObjectMgr->GetTrainer(npc->GetEntry()) : nullptr;
            if (!t || !t->IsTrainerValidForPlayer(p))
                return learned;
            float discount = p->GetReputationPriceDiscount(npc);
            // a new rank can become learnable after the one before it: a few passes
            for (int pass = 0; pass < 3; ++pass)
            {
                std::vector<Trainer::Spell const*> now;
                for (Trainer::Spell const& sp : t->GetSpells())
                    if (t->CanTeachSpell(p, &sp))
                        now.push_back(&sp);
                if (now.empty())
                    break;
                std::sort(now.begin(), now.end(), [](Trainer::Spell const* a, Trainer::Spell const* b) { return a->MoneyCost < b->MoneyCost; });
                bool any = false;
                tooExpensive = 0;
                for (Trainer::Spell const* sp : now)
                {
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(sp->SpellId);
                    if (!info)
                        continue;
                    uint32 cost = uint32(std::floor(sp->MoneyCost * discount));
                    if (p->GetMoney() < cost)
                    {
                        ++tooExpensive;
                        continue;
                    }
                    p->ModifyMoney(-int32(cost));
                    if (info->HasEffect(SPELL_EFFECT_LEARN_SPELL))
                        p->CastSpell(p, info->Id, true);
                    else
                        p->learnSpell(info->Id, false);
                    paid += cost;
                    learned.push_back(info->SpellName[0]);
                    any = true;
                }
                if (!any)
                    break;
            }
            return learned;
        }

        std::string NameList(std::vector<std::string> const& names, size_t max = 6)
        {
            std::string out;
            for (size_t i = 0; i < names.size() && i < max; ++i)
                out += (i ? ", " : "") + names[i];
            if (names.size() > max)
                out += " and " + std::to_string(names.size() - max) + " more";
            return out;
        }

        // what an errand is for ("to turn in quests", ...)
        char const* ErrandPurpose(bool sell, bool train, bool turnIn)
        {
            return sell ? " to sell junk" : train ? " to learn new spells" : turnIn ? " to turn in quests" : " to pick up quests";
        }

        // The nearest friendly merchant within `radius`.
        bool NearestVendor(Player* p, float radius, GiverInfo& out)
        {
            FactionTemplateEntry const* mine = p->GetFactionTemplateEntry();
            float best = -1;
            for (QuestSpawn const& v : VendorSpawns(p->GetMapId()))
            {
                float d = p->GetDistance2d(v.posX, v.posY);
                if (d > radius || (best >= 0 && d >= best))
                    continue;
                CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(v.id);
                if (!ct)
                    continue;
                if (FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(ct->faction))
                    if (mine && mine->IsHostileTo(*ft))
                        continue;
                best = d;
                out.entry = v.id;
                out.name = ct->Name;
                out.x = v.posX;
                out.y = v.posY;
                out.z = v.posZ;
                out.dist = d;
            }
            return best >= 0;
        }

        // Quest givers (spawned creatures) within `radius` that offer quests `p` can take right now; nearest first.
        std::vector<GiverInfo> AvailableGivers(Player* p, float radius, std::string const& filter, int maxLevel = 0,
                                               std::set<uint32> const* ignore = nullptr)
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
                    if (maxLevel > 0 && q->GetQuestLevel() > maxLevel)
                        continue;   // too hard for the group right now
                    if (ignore && ignore->count(q->GetQuestId()))
                        continue;   // the player doesn't want this one
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

        // Both really in the same place: in the world, same map AND same instance (dungeon copies share a map id),
        // same phase, and neither is in the middle of a teleport. Coordinates alone can't tell these apart.
        bool SameWorld(Player* a, Player* b)
        {
            return a && b && a->IsInWorld() && b->IsInWorld() && !a->IsBeingTeleported() && !b->IsBeingTeleported() &&
                   a->GetMapId() == b->GetMapId() && a->GetInstanceId() == b->GetInstanceId() && a->InSamePhase(b);
        }

        bool Near(Player* a, Player* b, float range)
        {
            return SameWorld(a, b) && a->GetDistance(b) <= range;
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
        bool QuestDestination(Player* bot, Quest const* q, uint32& map, float& x, float& y, float& z, bool& haveZ,
                              std::vector<std::pair<float, float>> const* avoid = nullptr, std::set<uint32> const* want = nullptr)
        {
            std::set<uint32> entries = want ? *want : TargetEntries(bot, q);
            float best = -1;
            if (!entries.empty())
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                {
                    if (data.mapid != bot->GetMapId() || !entries.count(data.id))
                        continue;
                    bool tried = false;
                    if (avoid)
                        for (auto const& [ax, ay] : *avoid)
                            if (std::hypot(data.posX - ax, data.posY - ay) < 50.0f)
                            {
                                tried = true;
                                break;
                            }
                    if (tried)
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
        VendorSpawns(0);
    }

    // ------------------------------------------------------------------ knowledge

    void CompanionControl::AddKnowledge(WorldContext& ctx, Player* owner, Player* bot, uint32 topics) const
    {
        if (!_cfg.controlEnabled || !owner || !bot || !topics)
            return;
        auto& k = ctx.knowledge;
        if (topics & (knowledge::Gold | knowledge::Inventory))
            k.push_back("Your money: " + MoneyWords(bot->GetMoney()) + ".");
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
            // her quests and their progress are in the journal (AddJournal), which every prompt has
            for (auto const& [q, slot] : QuestLog(bot))
                k.push_back("\"" + q->GetTitle() + "\" is a level " +
                            std::to_string(q->GetQuestLevel() > 0 ? q->GetQuestLevel() : int(bot->GetLevel())) + " quest.");
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

    void CompanionControl::AddJournal(WorldContext& ctx, Player* owner, Player* bot) const
    {
        if (!_cfg.controlEnabled || !owner || !bot)
            return;
        auto& j = ctx.journal;
        auto mine = QuestLog(bot);
        if (mine.empty())
            j.push_back("Your quest log is empty.");
        size_t n = 0;
        for (auto const& [q, slot] : mine)
        {
            if (++n > kMaxJournalQuests)
            {
                j.push_back("... and " + std::to_string(mine.size() - kMaxJournalQuests) + " more quests.");
                break;
            }
            // her own numbers first; theirs only as a comparison, clearly marked
            std::string line = "Your quest \"" + q->GetTitle() + "\": you have " + QuestProgress(bot, q, slot);
            uint16 theirSlot = owner->FindQuestSlot(q->GetQuestId());
            if (theirSlot < MAX_QUEST_LOG_SIZE)
                line += " (" + owner->GetName() + "'s own count, not yours: " + QuestProgress(owner, q, theirSlot) + ")";
            else
                line += " (" + owner->GetName() + " doesn't have it)";
            j.push_back(line + ".");
        }
        j.push_back("Your bags: " + std::to_string(bot->GetFreeInventorySpace()) + " of " + std::to_string(TotalBagSlots(bot)) +
                    " slots free. Your money: " + MoneyWords(bot->GetMoney()) + ".");
        if (auto l = _leads.find(bot->GetGUID()); l != _leads.end())
        {
            Lead const& lead = l->second;
            std::string task = lead.train  ? "walking to your class trainer " + lead.title + " to learn spells"
                               : lead.sell ? "leading them to the merchant " + lead.title + " to sell junk"
                               : lead.turnIn ? "going to " + lead.title + " to turn in quests"
                               : lead.fetch ? "going to " + lead.title + " to pick up quests"
                               : !lead.title.empty() ? "leading them to the quest \"" + lead.title + "\""
                                                     : "leading the way";
            j.push_back("What you are doing: " + task + (lead.paused ? " (paused, waiting)" : "") + ".");
        }
        else if (auto h = _hunts.find(owner->GetGUID()); h != _hunts.end() && !h->second.forLead &&
                 std::find(h->second.bots.begin(), h->second.bots.end(), bot->GetGUID()) != h->second.bots.end())
            j.push_back("What you are doing: clearing the area of enemies.");
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
            case CompanionCommandKind::AcceptQuests:
            case CompanionCommandKind::TurnInQuests:
            {
                auto l = _leads.find(bot->GetGUID());
                if (l != _leads.end() && (!l->second.fetch || l->second.group))
                    return "Nothing yet. You are leading them to " + l->second.title + " right now; that comes afterwards. Say so briefly.";
                return cmd.kind == CompanionCommandKind::AcceptQuests ? AcceptQuests(owner, bot, cmd, now) : TurnInQuests(owner, bot, cmd, now);
            }
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
            case CompanionCommandKind::LearnSpells: return LearnSpells(owner, bot, now);
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
        if (cmd.badAmount)
            return "Nothing. They asked for an amount that isn't a whole number of items (" + cmd.arg + "); ask them how many they mean.";
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
        if (!SameWorld(bot, owner) || bot->GetDistance(owner) > kTradeDistance)
            return "Nothing yet. They asked for your " + best->proto->Name1 + ", but you are too far apart to trade; ask them to come closer.";
        if (owner->GetTrader() && owner->GetTrader() != bot)
            return "Nothing. They are trading with someone else right now.";

        // Trades move whole stacks: offer enough stacks to cover the requested amount.
        uint32 wanted = cmd.amountGiven ? uint32(std::min<uint64>(cmd.amount, best->count)) : best->count;
        uint32 perStack = std::max<uint32>(1, best->proto->GetMaxStackSize());
        uint32 stacks = std::max<uint32>(1, std::min<uint32>(best->stacks, (wanted + perStack - 1) / perStack));
        stacks = std::min<uint32>(stacks, 6);
        Trade(owner, bot, "t " + ItemLink(best->proto) + " " + std::to_string(stacks), now);
        std::string what = best->proto->Name1 + (best->count > 1 ? " (you have " + std::to_string(best->count) + ")" : "");
        std::string note = cmd.amountGiven && cmd.amount > best->count ? " You had fewer than they asked for." : "";
        return "You opened a trade with them and put your " + what + " in it; they only need to accept." + note;
    }

    std::string CompanionControl::GiveGold(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        if (cmd.badAmount)
            return "Nothing. They asked for an amount of money that isn't a whole number (like \"2 gold 50 silver\"); you gave nothing. "
                   "Ask them to say it again.";
        uint64 amount = cmd.amount;
        bool all = cmd.arg == "all";
        if (all)
        {
            // "give me all your money": everything she has, but never more than MaxGoldPerRequest at once
            amount = bot->GetMoney();
            if (!amount)
                return "Nothing. You have no money at all; tell them.";
            if (_cfg.maxGoldPerRequest && amount > _cfg.maxGoldPerRequest)
                amount = _cfg.maxGoldPerRequest;
        }
        if (bot->GetMoney() < amount)
            return "Nothing. They asked for " + Money(amount) + ", but you only have " + Money(bot->GetMoney()) + "; tell them.";
        if (_cfg.maxGoldPerRequest && amount > _cfg.maxGoldPerRequest)
            return "Nothing. They asked for " + Money(amount) + ", which is more than you are willing to hand over at once (at most " +
                   Money(_cfg.maxGoldPerRequest) + "); tell them.";
        if (!SameWorld(bot, owner) || bot->GetDistance(owner) > kHandOverDistance)
            return "Nothing yet. They asked for " + Money(amount) + ", but you are too far apart; ask them to come closer.";
        if (amount == 0 || amount > uint64(std::numeric_limits<int32>::max()))
            return "Nothing. That is not an amount you can hand over at once; tell them.";
        uint64 before = owner->GetMoney();
        if (!CanReceiveMoney(owner, amount) || !owner->ModifyMoney(int32(amount), false))
            return "Nothing. They can't carry any more gold; tell them.";
        if (uint64(owner->GetMoney()) < before + amount)
        {
            LOG_WARN("module", "[mod-true-companion] gold handover failed: {} had {} copper, asked {}, has {} after", owner->GetName(), before,
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

    ObjectGuid CompanionControl::EscortedBy(ObjectGuid bot) const
    {
        for (auto const& [leader, l] : _leads)
            if (l.escort.count(bot))
                return leader;
        return ObjectGuid::Empty;
    }

    bool CompanionControl::IsBusy(ObjectGuid owner, ObjectGuid bot) const
    {
        if (_leads.count(bot))
            return true;
        auto h = _hunts.find(owner);
        return h != _hunts.end() && std::find(h->second.bots.begin(), h->second.bots.end(), bot) != h->second.bots.end();
    }

    void CompanionControl::OnActivated(Player* owner, Player* bot)
    {
        if (owner && bot && !_leads.count(bot->GetGUID()) && EscortedBy(bot->GetGUID()).IsEmpty())
            PlayerbotsBridge::SetFollow(bot, owner, true);
    }

    void CompanionControl::OnPlayerFollowOrder(Player* owner, Player* bot)
    {
        if (!owner || !bot)
            return;
        // walking behind a leading companion: from now on she follows the player instead
        for (auto& [leader, l] : _leads)
            if (l.escort.erase(bot->GetGUID()))
            {
                l.escortSkip.insert(bot->GetGUID());
                PlayerbotsBridge::SetFollow(bot, owner, true);
            }
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
            std::vector<Item*> junk = JunkItems(bot);
            if (junk.empty())
                return "Nothing. You have no junk to sell; tell them.";
            SaleResult sale = SellStacks(bot, junk, 0);
            if (sale.noRoom)
                return "Nothing. You can't carry any more money, so the merchant can't pay you; nothing was sold. Tell them.";
            if (!sale.stacks)
                return "Nothing. Your junk can't be sold right now (it may be in a trade); tell them.";
            return "You sold " + std::to_string(sale.stacks) + " junk items to " + vendor->GetName() + " for " + Money(sale.credited) + ".";
        }
        if (cmd.badAmount)
            return "Nothing. They asked you to sell an amount that isn't a whole number of items (" + cmd.arg +
                   "); nothing was sold. Ask them how many they mean.";
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
        // "sell 3 of your linen cloth": exactly that many; no count (or more than she has): all of them
        uint32 want = cmd.amountGiven && cmd.amount < best->count ? uint32(cmd.amount) : 0;
        SaleResult sale = SellStacks(bot, items, want);
        if (sale.noRoom)
            return "Nothing. You can't carry any more money, so the merchant can't pay you for your " + best->proto->Name1 +
                   "; nothing was sold. Tell them.";
        if (!sale.units)
            return "Nothing. Your " + best->proto->Name1 + " could not be sold right now (it may be in a trade); tell them.";
        std::string fewer = cmd.amountGiven && cmd.amount > best->count
                                ? " They asked for " + std::to_string(cmd.amount) + ", but you only had " + std::to_string(best->count) + "."
                                : "";
        return "You sold " + (sale.units > 1 ? std::to_string(sale.units) + " of your " : std::string("your ")) + best->proto->Name1 +
               " to " + vendor->GetName() + " for " + Money(sale.credited) +
               (best->count > sale.units ? " (you still have " + std::to_string(best->count - sale.units) + ")." : ".") + fewer;
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
                if (b->IsInWorld() && b->IsAlive() && SameWorld(b, owner))
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
        {
            ++h.kills;
            // a corpse of ours (tapped by the group) goes on the loot list
            if (Creature* dead = current ? current->ToCreature() : nullptr)
                if (dead->isTappedBy(owner))
                    h.toLoot[h.current] = now;
            h.current = ObjectGuid::Empty;
        }
        // Loot state: out of combat, the bots loot every corpse of ours (quest items like Lynx Collars are only in the
        // loot) before the next enemy is marked. A new fight ends it; looting goes on after it.
        if (!h.toLoot.empty())
        {
            bool fighting = false;
            for (Player* b : bots)
                fighting = fighting || b->IsInCombat();
            if (!fighting)
            {
                bool waitForPlayer = false;
                for (Player* b : bots)
                    waitForPlayer = waitForPlayer || _waitLoot.count(b->GetGUID());
                for (auto it = h.toLoot.begin(); it != h.toLoot.end();)
                {
                    Creature* c = ObjectAccessor::GetCreature(*bots.front(), it->first);
                    // without "wait for me to loot": the player's round-robin turn isn't waited for, and a corpse with
                    // only the player's share left is given up after a few seconds
                    bool playersTurn = c && c->loot.roundRobinPlayer == owner->GetGUID();
                    bool room = false;   // with full bags nobody can take anything (TendBags makes room or pauses looting)
                    for (Player* b : bots)
                        room = room || b->GetFreeInventorySpace() > 0;
                    bool done = !c || c->IsAlive() || c->loot.isLooted() || (!waitForPlayer && playersTurn) || (!room && !waitForPlayer) ||
                                now - it->second > (waitForPlayer ? 60 : 12);
                    it = done ? h.toLoot.erase(it) : std::next(it);
                }
                if (!h.toLoot.empty())
                {
                    if (now - h.lastLootCmd >= 3)
                    {
                        h.lastLootCmd = now;
                        for (Player* b : bots)
                            if (b->GetFreeInventorySpace() > 0)
                                PlayerbotsBridge::Command(b, owner, "add all loot");
                    }
                    return true;   // still looting
                }
            }
        }

        Player* anchor = bots.front();
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(anchor, anchor, h.radius);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(anchor, units, check);
        Cell::VisitObjects(anchor, searcher, h.radius);
        Unit* next = nullptr;
        float bestDist = 0;
        h.seen = h.tapped = h.other = 0;
        for (Unit* u : units)
        {
            Creature* c = u->ToCreature();
            if (!c || !c->IsAlive() || !anchor->IsValidAttackTarget(c))
                continue;
            // "Civilian" wildlife (e.g. Springpaw Cubs, which drop the Lynx Collars) counts as an enemy like with
            // Playerbots' grind; friendly townsfolk never pass IsValidAttackTarget anyway.
            ++h.seen;
            if (c->hasLootRecipient() && !c->isTappedBy(owner))
            {
                ++h.tapped;
                continue;   // someone else's fight
            }
            if (h.skipped.count(c->GetGUID()))
                continue;
            if (h.maxLevel && c->GetLevel() > h.maxLevel)
                continue;   // grinding: nothing far above the group
            if (!h.only.IsEmpty())
            {
                if (c->GetGUID() != h.only)
                    continue;
            }
            else if (!h.entries.empty())
            {
                if (!h.entries.count(c->GetEntry()))
                {
                    ++h.other;
                    continue;
                }
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
            LOG_INFO("module", "[mod-true-companion] {}: no target left ({} enemies around, {} tagged by others, {} not wanted, {} skipped, {} killed)",
                     anchor->GetName(), h.seen, h.tapped, h.other, h.skipped.size(), h.kills);
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
                        // through Playerbots' own movement (a plain MovePoint is cancelled by its strategies)
                        if (!PlayerbotsBridge::MoveTo(b, owner, next->GetPositionX(), next->GetPositionY(), next->GetPositionZ()))
                            b->GetMotionMaster()->MovePoint(0, next->GetPositionX(), next->GetPositionY(), next->GetPositionZ(),
                                                            FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
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
        std::vector<std::pair<Quest const*, Creature*>> take;
        std::vector<std::string> cannot;
        for (int pass = 0; pass < 2 && take.empty(); ++pass)
            for (auto const& [id, npc] : offered)
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(id);
                if (!q || bot->FindQuestSlot(id) < MAX_QUEST_LOG_SIZE || bot->GetQuestRewardStatus(id))
                    continue;
                if (cmd.arg.empty() && Ignored(owner) && Ignored(owner)->count(id))
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
                take.emplace_back(q, npc);
                if (take.size() >= limit)
                    break;
            }
        std::string names;
        for (auto const& [q, npc] : take)
            if (AcceptQuest(bot, q, npc))
                names += (names.empty() ? "\"" : ", \"") + q->GetTitle() + "\"";
        std::string refused;
        for (auto const& t : cannot)
            refused += (refused.empty() ? "\"" : ", \"") + t + "\"";

        // "pick up all your quests": after the ones here, go on to the other quest givers around
        std::string onward;
        if (cmd.all || take.empty())
        {
            auto givers = AvailableGivers(bot, kAreaRadius, cmd.arg, cmd.arg.empty() ? LevelCap(owner, bot) : 0, Ignored(owner));
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
            if (!TurnInQuest(bot, q, it->second))
            {
                notReady += (notReady.empty() ? "\"" : ", \"") + q->GetTitle() + "\" (no room in your bags for the reward)";
                continue;
            }
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
            lead.instance = bot->GetInstanceId();
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
        // like a shared quest: the player's accept already ran the quest giver's scripts once
        if (!AcceptQuest(bot, q, nullptr))
            return "";
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
        TurnInQuest(bot, q, it->second);
        return "";   // the quest-complete event already makes her comment
    }

    std::string CompanionControl::LearnSpells(Player* owner, Player* bot, time_t now)
    {
        if (auto cur = _leads.find(bot->GetGUID()); cur != _leads.end())
            return cur->second.fetch
                ? "Nothing yet. You are still on your way to " + cur->second.title + ErrandPurpose(cur->second.sell, cur->second.train,
                      cur->second.turnIn) + "; you can go to your trainer when you are back. Tell them."
                : "Nothing yet. You are leading them to \"" + cur->second.title + "\"; you can go to your trainer afterwards (or when "
                  "they say \"stop leading\"). Tell them.";
        std::vector<std::string> names;
        uint32 cost = TeachableNow(bot, &names);
        if (!cost)
            return "Nothing. Your class trainer has nothing new for you at your level; tell them.";
        // her trainer right here: learn now
        for (Creature* c : NearbyCreatures(bot, _cfg.interactRange + 5.0f))
            if (c->IsAlive() && TeachesHerNow(bot, c->GetEntry()))
            {
                uint32 paid = 0, tooExpensive = 0;
                std::vector<std::string> learned = LearnFrom(bot, c, paid, tooExpensive);
                if (learned.empty())
                    return "Nothing. You can't afford your new spells yet (about " + Money(cost) + ", you have " + Money(bot->GetMoney()) +
                           "); tell them.";
                return "You learned " + NameList(learned) + " from " + c->GetName() + " for " + Money(paid) + "." +
                       (tooExpensive ? " " + std::to_string(tooExpensive) + " more cost more than you have." : "") + " Tell them.";
            }
        if (bot->GetMoney() == 0)
            return "Nothing. You have no money to pay your trainer (" + NameList(names) + " cost about " + Money(cost) + "); tell them.";
        // the nearest trainer of her class around: walk there alone and come back
        QuestSpawn const* best = nullptr;
        float bestDist = 0;
        for (QuestSpawn const& sp : ClassTrainerSpawns(bot->GetMapId()))
        {
            float d = bot->GetDistance2d(sp.posX, sp.posY);
            if (d > 800.0f || (best && d >= bestDist) || !TeachesHerNow(bot, sp.id))
                continue;
            best = &sp;
            bestDist = d;
        }
        if (!best)
            return "Nothing. There is no trainer for your class around here (" + NameList(names) + " are waiting for you, about " +
                   Money(cost) + "); the big cities have one. Tell them.";
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(best->id);
        std::string trainerName = ct ? ct->Name : "your trainer";
        if (!BeginErrand(owner, bot, best->id, trainerName, best->posX, best->posY, best->posZ, false, "", false, now))
            return "Nothing. You can't walk over there right now; tell them.";
        _leads[bot->GetGUID()].train = true;
        LOG_INFO("module", "[mod-true-companion] {} goes to {} to learn {} spells", bot->GetName(), trainerName, names.size());
        return "You go to " + trainerName + " (about " + std::to_string(int(bestDist)) + " yards " + Direction(bot, best->posX, best->posY) +
               ") to learn " + NameList(names) + " (about " + Money(cost) + "), then you come back to them. Tell them.";
    }

    std::string CompanionControl::StartLead(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        if (!_cfg.leadMode || !PlayerbotsBridge::Available())
            return "Nothing. Leading the way is not possible here; you keep following them. Tell them.";
        if (auto cur = _leads.find(bot->GetGUID()); cur != _leads.end())
        {
            // on an errand: a lead to a named quest, or one told to her directly, takes over (the errand can wait;
            // finished quests are still turned in later). Only a plain "lead the way" to everyone waits for it.
            if (cur->second.fetch && cmd.arg.empty() && !cmd.direct)
                return "Nothing yet. You are still on your way to " + cur->second.title + ErrandPurpose(cur->second.sell, cur->second.train, cur->second.turnIn) +
                       "; you can lead them as soon as you are back. Tell them.";
            if (cur->second.fetch)
                LOG_INFO("module", "[mod-true-companion] {} drops the errand to {}: told to lead", bot->GetName(), cur->second.title);
            else if (cmd.arg.empty())
                return ResumeLead(owner, bot);   // "lead again" while a lead is paused: continue it
        }
        // one companion leads; the others follow the group. Told directly ("Myron, lead the way"), she takes over.
        std::vector<ObjectGuid> others;
        for (auto const& [other, l] : _leads)
            if (other != bot->GetGUID() && l.owner == owner->GetGUID() && (!l.fetch || l.group))
            {
                if (!cmd.direct)
                {
                    Player* leader = ObjectAccessor::FindConnectedPlayer(other);
                    return "Nothing. " + (leader ? leader->GetName() : std::string("Another companion")) + " is already leading the way to \"" +
                           l.title + "\"; you follow along with the group. Say so in a few words.";
                }
                others.push_back(other);
            }
        for (ObjectGuid other : others)
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(other))
                StopLead(owner, leader);
        Quest const* pick = nullptr;
        if (!cmd.arg.empty())
        {
            // her quests first, then the player's, then the other companions' (e.g. a quest only the player has)
            std::vector<Player*> logs = { bot, owner };
            for (Player* m : OtherCompanions(owner, bot))
                logs.push_back(m);
            int bestScore = 0;
            for (Player* p : logs)
                for (auto const& [q, slot] : QuestLog(p))
                {
                    int sc = MatchScore(q->GetTitle(), cmd.arg);
                    if (sc > bestScore)
                    {
                        pick = q;
                        bestScore = sc;
                    }
                }
            if (!pick)
                return "Nothing. They want to do \"" + cmd.arg + "\", but nobody in the group has that quest; tell them and ask which one.";
        }
        else
        {
            // a new "lead the way": nothing skipped or visited from an earlier round
            _roundSkip.erase(owner->GetGUID());
            _roundVisited.erase(owner->GetGUID());
            std::string said = LeadToNextQuest(owner, bot, now);
            if (!said.empty())
            {
                // "lead the way" is a round: quest after quest, then everything is turned in together
                if (auto r = _leads.find(bot->GetGUID()); r != _leads.end())
                    r->second.group = r->second.round = true;
                return said;
            }
            return QuestRound(owner, bot, now);   // nothing to do: turn in, pick some up around here, or grind
        }
        if (!PartyNeeds(owner, bot, pick))
        {
            // finished (by her, the player or another companion): lead them to turn it in. Companions who come
            // along turn in their copy there; the player turns in their own.
            std::vector<Player*> done;
            for (Player* m : std::vector<Player*>{ bot, owner })
                if (m->GetQuestStatus(pick->GetQuestId()) == QUEST_STATUS_COMPLETE)
                    done.push_back(m);
            for (Player* m : OtherCompanions(owner, bot))
                if (m->GetQuestStatus(pick->GetQuestId()) == QUEST_STATUS_COMPLETE)
                    done.push_back(m);
            if (!done.empty())
            {
                auto enders = QuestEnders(bot, kAreaRadius, { pick });
                if (!enders.empty())
                {
                    GiverInfo const& g = enders.front();
                    bool round = _leads.count(bot->GetGUID()) && _leads[bot->GetGUID()].round;
                    uint32 then = InterruptedQuest(bot->GetGUID());
                    if (BeginErrand(owner, bot, g.entry, g.name, g.x, g.y, g.z, true, "", false, now))
                    {
                        Lead& r = _leads[bot->GetGUID()];
                        r.group = true;
                        r.round = round;
                        r.thenQuest = then == pick->GetQuestId() ? 0 : then;
                        std::string who;
                        for (Player* m : done)
                            who += (who.empty() ? "" : ", ") + (m == bot ? std::string("you") : m == owner ? std::string("them") : m->GetName());
                        return "\"" + pick->GetTitle() + "\" is done for " + who + ", so you lead them to " + g.name + " (about " +
                               std::to_string(int(g.dist)) + " yards " + Direction(bot, g.x, g.y) + ") to turn it in. Tell them.";
                    }
                }
                return "Nothing. \"" + pick->GetTitle() + "\" is done, but the quest giver to turn it in is not around here; tell them.";
            }
            return "Nothing. \"" + pick->GetTitle() + "\" is already finished and only needs to be turned in; tell them.";
        }
        if (auto cur = _leads.find(bot->GetGUID()); cur != _leads.end() && !cur->second.fetch && cur->second.quest == pick->GetQuestId())
            return ResumeLead(owner, bot);

        return BeginQuestLead(owner, bot, pick, now);
    }

    uint32 CompanionControl::InterruptedQuest(ObjectGuid bot) const
    {
        auto it = _leads.find(bot);
        if (it == _leads.end())
            return 0;
        return it->second.fetch ? it->second.thenQuest : it->second.quest;
    }

    bool CompanionControl::BackToQuest(Player* owner, Player* bot, uint32 questId, std::string const& done, time_t now)
    {
        Quest const* q = sObjectMgr->GetQuestTemplate(questId);
        if (!q || !PartyNeeds(owner, bot, q))
            return false;
        LOG_INFO("module", "[mod-true-companion] {} goes back to the lead to \"{}\"", bot->GetName(), q->GetTitle());
        std::string said = BeginQuestLead(owner, bot, q, now);
        auto it = _leads.find(bot->GetGUID());
        if (it == _leads.end() || it->second.fetch || it->second.quest != questId)
            return false;   // could not lead there (the errand is still set: the caller finishes it)
        _notices.push_back({ bot->GetGUID(), done + "Now back to \"" + q->GetTitle() + "\". " + said });
        return true;
    }

    std::string CompanionControl::BeginQuestLead(Player* owner, Player* bot, Quest const* pick, time_t now)
    {
        Lead lead;
        lead.owner = owner->GetGUID();
        lead.quest = pick->GetQuestId();
        lead.title = pick->GetTitle();
        lead.since = now;
        std::set<uint32> want = PartyTargets(owner, bot, pick);
        if (!QuestDestination(bot, pick, lead.map, lead.x, lead.y, lead.z, lead.haveZ, nullptr, &want))
            return "Nothing. You don't know where \"" + pick->GetTitle() + "\" takes place (it is not on this continent or has no "
                   "known location); tell them and suggest they lead.";
        if (!PlayerbotsBridge::StartLead(bot, owner))
            return "Nothing. You could not take the lead right now; tell them.";
        lead.instance = bot->GetInstanceId();
        lead.lastX = bot->GetPositionX();
        lead.lastY = bot->GetPositionY();
        float dist = bot->GetDistance2d(lead.x, lead.y);
        // switching to another quest ("lead me to ..."): the fight for the old one ends
        if (auto old = _hunts.find(owner->GetGUID()); old != _hunts.end() && old->second.forLead)
        {
            ReleaseChasers(old->second);
            _hunts.erase(old);
        }
        if (auto prev = _leads.find(bot->GetGUID()); prev != _leads.end())
        {
            lead.escort = prev->second.escort;
            lead.escortSkip = prev->second.escortSkip;
            lead.round = prev->second.round;   // a "lead the way" round goes on after this quest
            lead.group = prev->second.group;
        }
        _roundVisited.erase(owner->GetGUID());   // a quest phase starts: later turn-ins may visit the same NPCs again
        _leads[bot->GetGUID()] = lead;
        LOG_INFO("module", "[mod-true-companion] {} leads to \"{}\" (level {}): {} yards, {} target kinds, {}", bot->GetName(), pick->GetTitle(),
                 pick->GetQuestLevel(), int(dist), want.size(), lead.haveZ ? "nearest spawn" : "quest map marker");
        LeadStep(owner, bot, _leads[bot->GetGUID()], now);
        std::string where = want.empty() ? "the area of" : "where the targets of";
        return "You took the lead and are walking to " + where + " your quest \"" + pick->GetTitle() + "\" (about " +
               std::to_string(int(dist)) + " yards away); they follow you. You wait if they fall behind. Tell them where you are going.";
    }

    std::set<uint32> const* CompanionControl::Ignored(Player* owner) const
    {
        auto it = owner ? _ignored.find(owner->GetGUID()) : _ignored.end();
        return it == _ignored.end() || it->second.empty() ? nullptr : &it->second;
    }

    void CompanionControl::SetQuestOrder(Player* owner, std::vector<uint32> order, bool apply, time_t now)
    {
        _questOrder[owner->GetGUID()] = std::move(order);
        if (!apply)
            return;
        // the companion leading right now switches to the new first quest
        for (auto& [leaderGuid, l] : _leads)
        {
            if (l.owner != owner->GetGUID() || l.fetch)
                continue;   // on the way to a quest giver or a merchant: the new order counts from the next quest on
            Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
            if (!leader)
                break;
            Quest const* next = NextQuest(owner, leader);
            if (next && l.quest != next->GetQuestId())
            {
                bool round = l.round;
                std::string said = BeginQuestLead(owner, leader, next, now);
                if (said.compare(0, 7, "Nothing") != 0)
                {
                    if (auto cur = _leads.find(leaderGuid); cur != _leads.end())
                    {
                        cur->second.round = round;
                        cur->second.group = true;
                    }
                    _notices.push_back({ leaderGuid, "They changed the quest order: " + said });
                }
            }
            break;   // one leader (the map may have changed: no further iteration)
        }
    }

    uint32 CompanionControl::AbandonForCompanions(Player* owner, uint32 questId)
    {
        uint32 n = 0;
        for (Player* m : OtherCompanions(owner, nullptr))
        {
            uint16 slot = m->FindQuestSlot(questId);
            if (slot >= MAX_QUEST_LOG_SIZE || !m->TakeQuestSourceItem(questId, true))
                continue;
            // the same steps as when a player drops a quest from the quest log
            if (Quest const* q = sObjectMgr->GetQuestTemplate(questId))
                if (q->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED))
                    m->RemoveTimedQuest(questId);
            m->AbandonQuest(questId);
            m->RemoveActiveQuest(questId);
            m->SetQuestSlot(slot, 0);
            ++n;
            // a lead to that quest ends
            if (auto l = _leads.find(m->GetGUID()); l != _leads.end() && !l->second.fetch && l->second.quest == questId &&
                Questers(owner, m, sObjectMgr->GetQuestTemplate(questId)).empty())
            {
                if (l->second.round)
                    _notices.push_back({ m->GetGUID(), "They dropped that quest. " + RoundNext(owner, m, time(nullptr)) });
                else
                    FinishLead(owner, m, "");
            }
        }
        LOG_INFO("module", "[mod-true-companion] {} abandoned quest {} for {} companions", owner->GetName(), questId, n);
        return n;
    }

    std::string CompanionControl::ShareQuest(Player* owner, uint32 questId, time_t now)
    {
        Quest const* q = sObjectMgr->GetQuestTemplate(questId);
        if (!q)
            return "Unknown quest.";
        if (!q->HasFlag(QUEST_FLAGS_SHARABLE))
            return "\"" + q->GetTitle() + "\" can't be shared.";
        // Like the core's quest sharing: someone must hold the quest (in the quest log, not failed) and be allowed to share
        // it; the others get it from that person. Without a holder nobody gets anything.
        auto holds = [&](Player* p)
        {
            uint16 slot = p->FindQuestSlot(questId);
            QuestStatus st = p->GetQuestStatus(questId);
            return slot < MAX_QUEST_LOG_SIZE && (st == QUEST_STATUS_INCOMPLETE || st == QUEST_STATUS_COMPLETE) && p->CanShareQuest(questId);
        };
        std::vector<Player*> companions = OtherCompanions(owner, nullptr);
        Player* holder = holds(owner) ? owner : nullptr;
        for (Player* m : companions)
            if (!holder && m->IsAlive() && SameWorld(m, owner) && holds(m))
                holder = m;
        if (!holder)
            return "Nobody in your group has \"" + q->GetTitle() + "\" to share; take it from its quest giver first.";
        std::string got, cannot, away;
        for (Player* m : companions)
        {
            if (m == holder || m->FindQuestSlot(questId) < MAX_QUEST_LOG_SIZE)
                continue;
            // the core shares only with group members on the holder's map; she must also be around
            if (!SameWorld(m, holder) || !m->IsInMap(holder) || m->GetDistance(holder) > kShareRange)
            {
                away += (away.empty() ? "" : ", ") + m->GetName();
                continue;
            }
            if (m->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE || !CanTakeNow(m, q) || !m->SatisfyQuestStatus(q, false))
            {
                cannot += (cannot.empty() ? "" : ", ") + m->GetName();
                continue;
            }
            // shared by the holder, no quest giver (timed quests start their own timer, as with the core's sharing)
            if (AcceptQuest(m, q, holder))
                got += (got.empty() ? "" : ", ") + m->GetName();
            else
                cannot += (cannot.empty() ? "" : ", ") + m->GetName();
        }
        bool toPlayer = holder != owner && owner->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE && owner->CanTakeQuest(q, false);
        if (toPlayer)
            _pending.push_back({ holder->GetGUID(), owner->GetGUID(), "share " + QuestLink(q), now, now + 30, false });
        LOG_INFO("module", "[mod-true-companion] {} shares \"{}\" (holder {}): to {} / you: {} / can't: {} / too far: {}", owner->GetName(),
                 q->GetTitle(), holder->GetName(), got, toPlayer ? "yes" : "no", cannot, away);
        std::string out = "\"" + q->GetTitle() + "\": ";
        if (!got.empty())
            out += "now also with " + got + ". ";
        if (toPlayer)
            out += holder->GetName() + " shares it with you (accept window). ";
        if (!cannot.empty())
            out += cannot + " can't take it (level, class, an earlier quest, full quest log or already done). ";
        if (!away.empty())
            out += away + " too far away to share with. ";
        if (got.empty() && !toPlayer && cannot.empty() && away.empty())
            out += "everyone who can take it has it already.";
        return out;
    }

    std::string CompanionControl::TakeQuest(Player* owner, uint32 questId, ObjectGuid preferred, time_t now)
    {
        Quest const* q = sObjectMgr->GetQuestTemplate(questId);
        if (!q)
            return "Unknown quest.";
        std::vector<Player*> companions = OtherCompanions(owner, nullptr);
        std::vector<Player*> takers;
        for (Player* m : companions)
            if (CanTakeNow(m, q))
                takers.push_back(m);
        if (takers.empty())
            return "None of your companions can take \"" + q->GetTitle() + "\" right now" +
                   (owner->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE && owner->CanTakeQuest(q, false) ? " (you can)." : ".");
        // the quest giver offering it, nearest to one of them
        GiverInfo giver;
        bool found = false;
        for (Player* m : takers)
            for (GiverInfo const& g : AvailableGivers(m, kAreaRadius, ""))
                if (std::find(g.quests.begin(), g.quests.end(), q) != g.quests.end() && (!found || g.dist < giver.dist))
                {
                    giver = g;
                    found = true;
                }
        if (!found)
            return "No quest giver around here offers \"" + q->GetTitle() + "\".";
        // taken on purpose: no longer ignored (the addon drops it from its list too)
        if (auto ig = _ignored.find(owner->GetGUID()); ig != _ignored.end())
            ig->second.erase(questId);
        // who goes: the companion leading now, else the one selected in the addon, else the first who can take it
        Player* actor = nullptr;
        bool round = false;
        for (auto const& [g, l] : _leads)
            if (l.owner == owner->GetGUID() && (!l.fetch || l.group))
                if (Player* p = ObjectAccessor::FindConnectedPlayer(g))
                {
                    actor = p;
                    round = l.round;
                }
        if (!actor)
            for (Player* m : companions)
                if (m->GetGUID() == preferred)
                    actor = m;
        if (!actor)
            actor = takers.front();
        // right here: everyone takes it at once
        if (actor->GetDistance2d(giver.x, giver.y) < 25.0f)
        {
            PickUpAt(owner, actor, giver.entry, "", 1, true, now, questId);
            std::string who;
            for (Player* m : takers)
                if (m->FindQuestSlot(questId) < MAX_QUEST_LOG_SIZE)
                    who += (who.empty() ? "" : ", ") + m->GetName();
            return who.empty() ? "They couldn't take \"" + q->GetTitle() + "\" from " + giver.name + "."
                               : who + " took \"" + q->GetTitle() + "\".";
        }
        uint32 then = InterruptedQuest(actor->GetGUID());
        if (!BeginErrand(owner, actor, giver.entry, giver.name, giver.x, giver.y, giver.z, false, "", false, now))
            return "Leading is not possible right now.";
        Lead& l = _leads[actor->GetGUID()];
        l.group = true;   // the others come along and take it too
        l.round = round;
        l.onlyQuest = questId;
        l.thenQuest = round ? 0 : then;   // a round decides on its own what comes next
        _notices.push_back({ actor->GetGUID(), "They want you all to take \"" + q->GetTitle() + "\" from " + giver.name + " (about " +
                                                   std::to_string(int(giver.dist)) + " yards " + Direction(actor, giver.x, giver.y) +
                                                   "); you lead them there. Say it in a few words." });
        LOG_INFO("module", "[mod-true-companion] {} leads to {} to take \"{}\"", actor->GetName(), giver.name, q->GetTitle());
        return actor->GetName() + " leads you to " + giver.name + " to take \"" + q->GetTitle() + "\".";
    }

    Json CompanionControl::LeadState(Player* owner) const
    {
        auto kindOf = [](Lead const& l) -> std::string
        {
            if (!l.fetch)
                return l.quest ? "quest" : "grind";
            if (l.sell)
                return "merchant";
            if (l.train)
                return "trainer";
            if (l.turnIn)
                return "turn_in";
            return l.onlyQuest ? "take" : "pick_up";
        };
        Json j = Json::MakeObject();
        Json errands = Json::MakeArray();
        bool found = false;
        for (auto const& [guid, l] : _leads)
        {
            if (l.owner != owner->GetGUID())
                continue;
            Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
            if (!bot)
                continue;
            if (l.fetch && !l.group)
            {
                // alone to a quest giver or merchant: not leading the player
                Json e = Json::MakeObject();
                e.Set("name", bot->GetName());
                e.Set("kind", kindOf(l));
                e.Set("target", l.title);
                errands.Push(e);
                continue;
            }
            if (found)
                continue;
            found = true;
            j.Set("leader", bot->GetName());
            j.Set("leaderGuid", int64_t(guid.GetCounter()));
            j.Set("kind", kindOf(l));
            j.Set("target", l.title);
            bool here = bot->GetMapId() == l.map;
            j.Set("distance", int64_t(here ? bot->GetDistance2d(l.x, l.y) : -1));
            j.Set("direction", here ? std::string(Direction(bot, l.x, l.y)) : std::string());
            j.Set("paused", l.paused);
        }
        // nobody leads: the companions follow the player (and maybe clear the area with them: "attack all here")
        j.Set("you", !found);
        if (auto h = _hunts.find(owner->GetGUID()); h != _hunts.end() && !h->second.forLead && !h->second.bots.empty())
            j.Set("fighting", true);
        j.Set("errands", errands);
        return j;
    }

    Json CompanionControl::GroupQuests(Player* owner) const
    {
        std::vector<Player*> members = { owner };
        for (Player* m : OtherCompanions(owner, nullptr))
            members.push_back(m);
        std::set<uint32> const* ignored = Ignored(owner);
        std::vector<uint32> order;
        if (auto it = _questOrder.find(owner->GetGUID()); it != _questOrder.end())
            order = it->second;
        // the party's quests: the player's order first, then as found (player, then companions)
        std::vector<uint32> ids = order;
        std::map<uint32, Json> rows;
        for (Player* m : members)
            for (auto const& [q, slot] : QuestLog(m))
            {
                uint32 id = q->GetQuestId();
                if (!rows.count(id))
                {
                    Json r = Json::MakeObject();
                    r.Set("id", int64_t(id));
                    r.Set("title", q->GetTitle());
                    r.Set("level", int64_t(q->GetQuestLevel() > 0 ? q->GetQuestLevel() : int(owner->GetLevel())));
                    r.Set("ignored", ignored && ignored->count(id));
                    r.Set("sharable", q->HasFlag(QUEST_FLAGS_SHARABLE));
                    // who lacks it and could get it shared
                    Json missing = Json::MakeArray();
                    for (Player* other : members)
                        if (other->FindQuestSlot(id) >= MAX_QUEST_LOG_SIZE && !other->GetQuestRewardStatus(id) &&
                            other->CanTakeQuest(q, false) && other->CanAddQuest(q, false))
                            missing.Push(other == owner ? std::string("you") : other->GetName());
                    r.Set("missing", missing);
                    r.Set("who", Json::MakeArray());
                    rows[id] = r;
                    if (std::find(ids.begin(), ids.end(), id) == ids.end())
                        ids.push_back(id);
                }
                Json w = Json::MakeObject();
                w.Set("name", m->GetName());
                w.Set("done", m->GetQuestStatus(id) == QUEST_STATUS_COMPLETE);
                w.Set("player", m == owner);
                rows[id].MutObject()["who"].Push(w);
            }
        Json quests = Json::MakeArray();
        for (uint32 id : ids)
            if (auto it = rows.find(id); it != rows.end())
                quests.Push(it->second);
        // quests the companions could pick up around here
        Json available = Json::MakeArray();
        std::set<uint32> seen;
        int cap = members.size() > 1 ? LevelCap(owner, members[1]) : 0;
        for (size_t i = 1; i < members.size(); ++i)
            for (GiverInfo const& g : AvailableGivers(members[i], kAreaRadius, ""))
                for (Quest const* q : g.quests)
                    if (seen.insert(q->GetQuestId()).second)
                    {
                        Json a = Json::MakeObject();
                        a.Set("id", int64_t(q->GetQuestId()));
                        a.Set("title", q->GetTitle());
                        a.Set("level", int64_t(q->GetQuestLevel() > 0 ? q->GetQuestLevel() : int(members[i]->GetLevel())));
                        a.Set("giver", g.name);
                        a.Set("distance", int64_t(g.dist));
                        a.Set("direction", std::string(Direction(members[i], g.x, g.y)));
                        a.Set("classQuest", IsClassQuest(q));
                        a.Set("tooHigh", cap > 0 && q->GetQuestLevel() > cap);
                        a.Set("ignored", ignored && ignored->count(q->GetQuestId()));
                        Json takers = Json::MakeArray();
                        for (size_t k = 1; k < members.size(); ++k)
                            if (CanTakeNow(members[k], q))
                                takers.Push(members[k]->GetName());
                        a.Set("takers", takers);
                        a.Set("you", owner->FindQuestSlot(q->GetQuestId()) >= MAX_QUEST_LOG_SIZE &&
                                         !owner->GetQuestRewardStatus(q->GetQuestId()) && owner->CanTakeQuest(q, false));
                        available.Push(a);
                    }
        Json j = Json::MakeObject();
        j.Set("quests", quests);
        j.Set("available", available);
        j.Set("levelCap", int64_t(cap));
        j.Set("companions", int64_t(members.size() - 1));
        return j;
    }

    std::vector<Player*> CompanionControl::Questers(Player* owner, Player* bot, Quest const* q) const
    {
        std::vector<Player*> out;
        auto has = [q](Player* p) { return p && p->IsInWorld() && p->FindQuestSlot(q->GetQuestId()) < MAX_QUEST_LOG_SIZE; };
        if (has(bot))
            out.push_back(bot);
        if (has(owner))
            out.push_back(owner);
        for (Player* m : OtherCompanions(owner, bot))
            if (has(m))
                out.push_back(m);
        return out;
    }

    std::vector<Player*> CompanionControl::OtherCompanions(Player* owner, Player* bot) const
    {
        std::vector<Player*> out;
        if (Group* g = owner->GetGroup())
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    if (m != bot && m != owner && m->IsInWorld() && PlayerbotsBridge::MasterOf(m) == owner)
                        out.push_back(m);
        return out;
    }

    // The quest to lead to: one they share that she can fight for, then any she can fight for, then any unfinished one.
    // Every quest she could lead to now, best first: the quests of the whole group (hers, the player's, the other
    // companions'), in the group's quest order (else her quest log, then the player's, ...). Left out: quests nobody
    // needs any more, ignored ones, ones skipped in this round, ones with nothing to lead to, and quests far above the
    // group unless the player put them in the order.
    std::vector<Quest const*> CompanionControl::QuestCandidates(Player* owner, Player* bot) const
    {
        std::vector<Quest const*> list;
        std::set<uint32> seen;
        auto add = [&](Player* p)
        {
            for (auto const& [q, slot] : QuestLog(p))
                if (seen.insert(q->GetQuestId()).second)
                    list.push_back(q);
        };
        add(bot);
        add(owner);
        for (Player* m : OtherCompanions(owner, bot))
            add(m);
        std::vector<uint32> const* order = nullptr;
        if (auto it = _questOrder.find(owner->GetGUID()); it != _questOrder.end() && !it->second.empty())
            order = &it->second;
        auto rank = [&](Quest const* q)
        {
            if (!order)
                return size_t(0);
            auto at = std::find(order->begin(), order->end(), q->GetQuestId());
            return at == order->end() ? order->size() : size_t(at - order->begin());
        };
        std::stable_sort(list.begin(), list.end(), [&](Quest const* a, Quest const* b) { return rank(a) < rank(b); });
        std::set<uint32> const* ignored = Ignored(owner);
        std::set<uint32> const* skipped = nullptr;
        if (auto it = _roundSkip.find(owner->GetGUID()); it != _roundSkip.end())
            skipped = &it->second;
        int cap = LevelCap(owner, bot);
        std::vector<Quest const*> out;
        for (Quest const* q : list)
        {
            uint32 id = q->GetQuestId();
            if (!PartyNeeds(owner, bot, q) || (ignored && ignored->count(id)) || (skipped && skipped->count(id)))
                continue;
            bool chosen = order && rank(q) < order->size();
            if (!chosen && q->GetQuestLevel() > cap)
                continue;
            if (PartyTargets(owner, bot, q).empty() && !sObjectMgr->GetQuestPOIVector(id))
                continue;   // nothing she could lead them to
            out.push_back(q);
        }
        return out;
    }

    Quest const* CompanionControl::NextQuest(Player* owner, Player* bot) const
    {
        auto list = QuestCandidates(owner, bot);
        return list.empty() ? nullptr : list.front();
    }

    std::string CompanionControl::LeadToNextQuest(Player* owner, Player* bot, time_t now)
    {
        for (Quest const* q : QuestCandidates(owner, bot))
        {
            std::string said = BeginQuestLead(owner, bot, q, now);
            if (said.compare(0, 7, "Nothing") != 0)
                return said;
            _roundSkip[owner->GetGUID()].insert(q->GetQuestId());   // no known place on this continent: not again
        }
        return "";
    }

    std::string CompanionControl::RoundSkip(Player* owner, Player* bot, uint32 quest, time_t now)
    {
        _roundSkip[owner->GetGUID()].insert(quest);
        auto hunt = _hunts.find(owner->GetGUID());
        if (hunt != _hunts.end() && hunt->second.forLead)
        {
            ReleaseChasers(hunt->second);
            _hunts.erase(hunt);
        }
        bot->GetMotionMaster()->Clear();
        return RoundNext(owner, bot, now);
    }

    std::set<uint32> CompanionControl::PartyTargets(Player* owner, Player* bot, Quest const* q) const
    {
        std::set<uint32> out;
        for (Player* p : Questers(owner, bot, q))
            if (p->GetQuestStatus(q->GetQuestId()) == QUEST_STATUS_INCOMPLETE)
                for (uint32 e : TargetEntries(p, q))
                    out.insert(e);
        return out;
    }

    bool CompanionControl::PartyNeeds(Player* owner, Player* bot, Quest const* q) const
    {
        for (Player* p : Questers(owner, bot, q))
            if (p->GetQuestStatus(q->GetQuestId()) == QUEST_STATUS_INCOMPLETE)
                return true;
        return false;
    }

    int CompanionControl::LevelCap(Player* owner, Player* bot) const
    {
        int sum = int(bot->GetLevel()) + int(owner->GetLevel()), n = 2;
        for (Player* m : OtherCompanions(owner, bot))
        {
            sum += int(m->GetLevel());
            ++n;
        }
        int bonus = n >= 5 ? 4 : n >= 3 ? 3 : 2;   // she and you: +2; three of you: +3; five: +4
        return (sum + n / 2) / n + bonus;
    }

    // "Lead the way" without a quest: to the quest givers around (everyone picks up), then to the quests; else grind.
    std::string CompanionControl::QuestRound(Player* owner, Player* bot, time_t now)
    {
        return RoundNext(owner, bot, now);
    }

    bool CompanionControl::NeedsSelling(Player* owner, Player* bot, time_t now) const
    {
        if (auto last = _lastSellTrip.find(bot->GetGUID()); last != _lastSellTrip.end() && now - last->second < 180)
            return false;   // just was at a merchant
        std::vector<Player*> all = OtherCompanions(owner, bot);
        all.push_back(bot);
        for (Player* p : all)
        {
            size_t junk = JunkItems(p).size();
            if (junk && (p->GetFreeInventorySpace() <= 4 || junk >= 8))
                return true;
        }
        return false;
    }

    // The next step of a "lead the way" round, in this order: a merchant when bags are full of junk; the next quest
    // someone still needs (in the player's order); when every quest is done, turning them in (and taking the new ones
    // at those quest givers); new quests from the givers around; grinding.
    std::string CompanionControl::RoundNext(Player* owner, Player* bot, time_t now)
    {
        auto setRound = [&]()
        {
            Lead& r = _leads[bot->GetGUID()];
            r.group = r.round = true;
        };
        int cap = LevelCap(owner, bot);
        if (NeedsSelling(owner, bot, now))
        {
            GiverInfo v;
            if (NearestVendor(bot, kAreaRadius, v) && BeginErrand(owner, bot, v.entry, v.name, v.x, v.y, v.z, false, "", false, now))
            {
                setRound();
                _leads[bot->GetGUID()].sell = true;
                _lastSellTrip[bot->GetGUID()] = now;
                LOG_INFO("module", "[mod-true-companion] {} leads to merchant {} ({} yards) to sell junk", bot->GetName(), v.name, int(v.dist));
                return "Bags are getting full of junk, so you lead them to the merchant " + v.name + " (about " + std::to_string(int(v.dist)) +
                       " yards " + Direction(bot, v.x, v.y) + ") to sell it first. Say it in a few words.";
            }
        }
        {
            std::string said = LeadToNextQuest(owner, bot, now);
            if (!said.empty())
            {
                setRound();
                return said;
            }
        }
        // (after the quest step: starting a quest lead clears this list)
        std::set<uint32>& visited = _roundVisited[owner->GetGUID()];
        {
            // every quest done: turn in what she and the other companions have finished
            std::vector<Player*> team = OtherCompanions(owner, bot);
            team.push_back(bot);
            std::vector<Quest const*> ready;
            std::set<uint32> seenReady;
            for (Player* p : team)
                for (auto const& [q, slot] : QuestLog(p))
                    if (p->GetQuestStatus(q->GetQuestId()) == QUEST_STATUS_COMPLETE && p->CanRewardQuest(q, false) &&
                        seenReady.insert(q->GetQuestId()).second)
                        ready.push_back(q);
            if (!ready.empty())
            {
                auto enders = QuestEnders(bot, kAreaRadius, ready);
                enders.erase(std::remove_if(enders.begin(), enders.end(), [&](GiverInfo const& g) { return visited.count(g.entry); }),
                             enders.end());
                if (!enders.empty())
                {
                    GiverInfo const& g = enders.front();
                    if (BeginErrand(owner, bot, g.entry, g.name, g.x, g.y, g.z, true, "", false, now))
                    {
                        setRound();
                        LOG_INFO("module", "[mod-true-companion] {} leads to {} ({} yards) to turn in", bot->GetName(), g.name, int(g.dist));
                        return "Every quest is done, so you lead them to " + g.name + " (about " + std::to_string(int(g.dist)) + " yards " +
                               Direction(bot, g.x, g.y) + ") to turn in " + QuestList(g.quests) + " (and take any new quests there). Tell them.";
                    }
                }
            }
        }
        auto givers = AvailableGivers(bot, kAreaRadius, "", cap, Ignored(owner));
        givers.erase(std::remove_if(givers.begin(), givers.end(), [&](GiverInfo const& g) { return visited.count(g.entry); }), givers.end());
        if (!givers.empty())
        {
            GiverInfo const& g = givers.front();
            if (BeginErrand(owner, bot, g.entry, g.name, g.x, g.y, g.z, false, "", true, now))
            {
                setRound();
                LOG_INFO("module", "[mod-true-companion] {} leads to quest giver {} ({} yards) to pick up quests", bot->GetName(), g.name, int(g.dist));
                return "You lead them to " + g.name + " (about " + std::to_string(int(g.dist)) + " yards " + Direction(bot, g.x, g.y) +
                       ") to pick up quests (" + QuestList(g.quests) + "), then to the other quest givers around, and then to the quests. "
                       "They follow you. Tell them.";
            }
        }
        if (_leads.count(bot->GetGUID()))
            FinishLead(owner, bot, "");
        return Grind(owner, bot, now);
    }

    std::vector<Quest const*> CompanionControl::PickUpAt(Player* owner, Player* bot, uint32 entry, std::string const& filter, size_t limit,
                                                         bool group, time_t now, uint32 onlyQuest)
    {
        std::vector<Quest const*> taken;
        // one chosen quest (Take): no level cap, not hidden by the ignore list
        bool chosen = onlyQuest != 0 || !filter.empty();
        int cap = chosen ? 0 : LevelCap(owner, bot);
        Creature* giver = CreatureNear(bot, entry, 30.0f);
        if (!giver)
            return taken;   // not there (despawned, dead): nothing can be taken from it
        for (GiverInfo const& g : AvailableGivers(bot, 30.0f, filter, cap, chosen ? nullptr : Ignored(owner)))
            if (g.entry == entry)
                for (Quest const* q : g.quests)
                    if (taken.size() < limit && (!onlyQuest || q->GetQuestId() == onlyQuest) && CanTakeNow(bot, q) &&
                        AcceptQuest(bot, q, giver))
                        taken.push_back(q);
        if (!group)
            return taken;
        // the other companions (they came along) take what they can from this quest giver too
        QuestRelationBounds b = sObjectMgr->GetCreatureQuestRelationBounds(entry);
        for (Player* m : OtherCompanions(owner, bot))
        {
            if (!m->IsAlive() || !SameWorld(m, bot) || m->GetDistance(bot) > 100.0f)
                continue;
            for (auto it = b.first; it != b.second; ++it)
                if (Quest const* oq = sObjectMgr->GetQuestTemplate(it->second))
                    if ((!onlyQuest || oq->GetQuestId() == onlyQuest) && CanTakeNow(m, oq) && (cap <= 0 || oq->GetQuestLevel() <= cap) &&
                        !(!chosen && Ignored(owner) && Ignored(owner)->count(oq->GetQuestId())))
                        AcceptQuest(m, oq, giver);   // from the same quest giver (they came along)
        }
        // the player gets the new quests shared (if they can take them; if not, she just goes on)
        time_t at = now;
        for (Quest const* tq : taken)
            if (owner->FindQuestSlot(tq->GetQuestId()) >= MAX_QUEST_LOG_SIZE && tq->HasFlag(QUEST_FLAGS_SHARABLE) && owner->CanTakeQuest(tq, false))
                _pending.push_back({ bot->GetGUID(), owner->GetGUID(), "share " + QuestLink(tq), at++, now + 30, false });
        return taken;
    }

    // While she leads, the other companions walk a few yards behind her (Playerbots can only follow the player).
    void CompanionControl::Escort(Player* owner, Player* bot, Lead& lead, time_t now)
    {
        if (lead.fetch && !lead.group)
        {
            ReleaseEscort(lead, owner);   // an errand she runs alone
            return;
        }
        if (now - lead.lastEscort < 3)
            return;
        lead.lastEscort = now;
        for (Player* m : OtherCompanions(owner, bot))
        {
            if (_leads.count(m->GetGUID()) || lead.escortSkip.count(m->GetGUID()) || !m->IsAlive() || m->IsInCombat() ||
                !SameWorld(m, bot) || PlayerbotsBridge::FollowState(m) == "stay")
                continue;
            // not yet behind her, or back from her own errand with Playerbots' follow on again
            if ((!lead.escort.count(m->GetGUID()) || PlayerbotsBridge::FollowState(m) == "follow") &&
                PlayerbotsBridge::SetFollow(m, owner, false))
                lead.escort.insert(m->GetGUID());
            if (lead.escort.count(m->GetGUID()) && m->GetDistance(bot) > 8.0f)
            {
                float behind = bot->GetOrientation() + 3.14159265f;
                PlayerbotsBridge::MoveTo(m, owner, bot->GetPositionX() + 3.0f * std::cos(behind),
                                         bot->GetPositionY() + 3.0f * std::sin(behind), bot->GetPositionZ());
            }
        }
    }

    void CompanionControl::ReleaseEscort(Lead& lead, Player* owner)
    {
        for (ObjectGuid g : lead.escort)
            if (Player* m = ObjectAccessor::FindConnectedPlayer(g))
                if (!_leads.count(g))
                    PlayerbotsBridge::SetFollow(m, owner, true);
        lead.escort.clear();
    }

    std::string CompanionControl::Grind(Player* owner, Player* bot, time_t now)
    {
        Group* group = bot->GetGroup();
        if (!group || !group->IsMember(owner->GetGUID()))
            return "Nothing. There is no quest for you around here, and you can only fight together when you are in the same group; tell them.";
        if (auto old = _hunts.find(owner->GetGUID()); old != _hunts.end())
        {
            ReleaseChasers(old->second);
            _hunts.erase(old);
        }
        Hunt h;
        h.owner = owner->GetGUID();
        h.bots = { bot->GetGUID() };
        for (Player* m : OtherCompanions(owner, bot))
            h.bots.push_back(m->GetGUID());
        h.radius = 50.0f;
        h.until = now + 600;
        h.maxLevel = uint8(std::min(255, LevelCap(owner, bot)));
        _hunts[owner->GetGUID()] = h;
        if (!HuntStep(_hunts[owner->GetGUID()], now))
        {
            ReleaseChasers(_hunts[owner->GetGUID()]);
            _hunts.erase(owner->GetGUID());
            return "Nothing. There is no quest for you around here and no enemy to fight either; tell them and suggest moving on to a new area.";
        }
        LOG_INFO("module", "[mod-true-companion] {}: no quests around, grinding", bot->GetName());
        return "There is no quest for you around here, so you fight the creatures nearby (grinding) until the area is clear. Tell them.";
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
        if (auto cur = _leads.find(bot->GetGUID()); cur != _leads.end())
            ReleaseEscort(cur->second, owner);
        bool was = _leads.erase(bot->GetGUID()) > 0;
        if (was)
            LOG_INFO("module", "[mod-true-companion] {} stops leading: told to (stop leading / follow me)", bot->GetName());
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
        lead.instance = bot->GetInstanceId();
        lead.x = x;
        lead.y = y;
        lead.z = z;
        lead.haveZ = true;
        lead.lastX = bot->GetPositionX();
        lead.lastY = bot->GetPositionY();
        if (auto prev = _leads.find(bot->GetGUID()); prev != _leads.end())
            lead.escort = prev->second.escort;
        // she goes on her own errand: no longer part of an area clearing (it would keep ordering her to attack)
        for (auto& [o, h] : _hunts)
            h.bots.erase(std::remove(h.bots.begin(), h.bots.end(), bot->GetGUID()), h.bots.end());
        _leads[bot->GetGUID()] = lead;
        return true;
    }

    void CompanionControl::FinishLead(Player* owner, Player* bot, std::string const& notice)
    {
        LOG_INFO("module", "[mod-true-companion] {} stops leading: {}", bot->GetName(), notice.empty() ? "(quest gone from her log)" : notice);
        if (auto cur = _leads.find(bot->GetGUID()); cur != _leads.end())
            ReleaseEscort(cur->second, owner);
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
        // hearthstone, teleport, an instance: the lead ends and Playerbots brings her along again
        if (bot->GetMapId() != lead.map || bot->GetInstanceId() != lead.instance || !SameWorld(owner, bot))
        {
            FinishLead(owner, bot, "You got separated from them (they went somewhere else), so you stop leading and follow them "
                                   "again. Say it briefly.");
            return;
        }
        if (lead.fetch)
        {
            if (bot->IsInCombat() || !bot->IsAlive())
                return;
            if (now - lead.since > (lead.group ? 600 : 240))
            {
                FinishLead(owner, bot, "You could not reach " + lead.title + ErrandPurpose(lead.sell, lead.train, lead.turnIn) +
                                       " and come back to them; tell them.");
                return;
            }
            if (lead.train && bot->GetDistance2d(lead.x, lead.y) < 12.0f)
            {
                Creature* npc = nullptr;
                for (Creature* c : NearbyCreatures(bot, 25.0f))
                    if (c->GetEntry() == lead.fetchEntry && c->IsAlive())
                        npc = c;
                uint32 paid = 0, tooExpensive = 0;
                std::vector<std::string> learned = LearnFrom(bot, npc, paid, tooExpensive);
                LOG_INFO("module", "[mod-true-companion] {} learned {} spells at {} for {}", bot->GetName(), learned.size(), lead.title, Money(paid));
                std::string said = !npc ? "You reached the place of " + lead.title + ", but the trainer isn't there right now."
                                 : learned.empty() ? "You reached " + lead.title + " but couldn't learn anything" +
                                                         (tooExpensive ? " (you can't afford it yet)." : ".")
                                                   : "You learned " + NameList(learned) + " from " + lead.title + " for " + Money(paid) + "." +
                                                         (tooExpensive ? " " + std::to_string(tooExpensive) + " more cost more than you have." : "");
                FinishLead(owner, bot, said + " You are coming back to them. Tell them briefly.");
                return;
            }
            if (lead.sell && bot->GetDistance2d(lead.x, lead.y) < 12.0f)
            {
                // everyone who came along sells their junk here
                uint32 items = 0;
                uint64 value = 0;
                if (NearbyVendor(bot, _cfg.interactRange))
                {
                    std::vector<Player*> sellers = { bot };
                    for (Player* m : OtherCompanions(owner, bot))
                        if (m->IsAlive() && SameWorld(m, bot) && m->GetDistance(bot) < 40.0f)
                            sellers.push_back(m);
                    for (Player* p2 : sellers)
                    {
                        SaleResult sale = SellStacks(p2, JunkItems(p2), 0);
                        value += sale.credited;
                        items += sale.stacks;
                    }
                }
                LOG_INFO("module", "[mod-true-companion] {}: sold {} junk items at {}", bot->GetName(), items, lead.title);
                std::string done = items ? "You all sold your junk (" + std::to_string(items) + " items, " + Money(value) + ") at " +
                                               lead.title + ". "
                                         : "";
                _notices.push_back({ bot->GetGUID(), done + RoundNext(owner, bot, now) });
                return;
            }
            if (lead.turnIn && bot->GetDistance2d(lead.x, lead.y) < 12.0f)
            {
                if (lead.round)
                    _roundVisited[owner->GetGUID()].insert(lead.fetchEntry);
                std::vector<Quest const*> handed;
                std::string playerTurnIn;
                for (auto const& [questId, npc] : QuestNpcs(bot, 30.0f, true))
                {
                    if (npc->GetEntry() != lead.fetchEntry)
                        continue;
                    Quest const* q = sObjectMgr->GetQuestTemplate(questId);
                    if (!q || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE || !bot->CanRewardQuest(q, false))
                        continue;
                    if (!lead.fetchFilter.empty() && MatchScore(q->GetTitle(), lead.fetchFilter) == 0)
                        continue;
                    if (TurnInQuest(bot, q, npc))
                        handed.push_back(q);
                }
                if (lead.group)
                {
                    // the companions who came along turn in their finished quests here too
                    Creature* ender = nullptr;
                    for (auto const& [questId, npc] : QuestNpcs(bot, 30.0f, true))
                        if (npc->GetEntry() == lead.fetchEntry)
                        {
                            ender = npc;
                            break;
                        }
                    QuestRelationBounds ends = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(lead.fetchEntry);
                    if (ender)
                        for (Player* m : OtherCompanions(owner, bot))
                        {
                            if (!m->IsAlive() || !SameWorld(m, bot) || m->GetDistance(bot) > 100.0f)
                                continue;
                            for (auto it = ends.first; it != ends.second; ++it)
                                if (Quest const* oq = sObjectMgr->GetQuestTemplate(it->second))
                                    if (m->GetQuestStatus(oq->GetQuestId()) == QUEST_STATUS_COMPLETE && TurnInQuest(m, oq, ender))
                                        if (std::find(handed.begin(), handed.end(), oq) == handed.end())
                                            handed.push_back(oq);
                        }
                    // the player turns in their own; say what they can hand in here
                    std::vector<Quest const*> theirs;
                    for (auto it = ends.first; it != ends.second; ++it)
                        if (Quest const* oq = sObjectMgr->GetQuestTemplate(it->second))
                            if (owner->GetQuestStatus(oq->GetQuestId()) == QUEST_STATUS_COMPLETE)
                                theirs.push_back(oq);
                    if (!theirs.empty())
                        playerTurnIn = " They can turn in " + QuestList(theirs) + " here too.";
                }
                if (lead.round)
                {
                    // turned in first; then whatever this quest giver has new for them
                    std::vector<Quest const*> fresh = PickUpAt(owner, bot, lead.fetchEntry, "", 99, true, now);
                    std::string done = (handed.empty() ? "" : "You turned in " + QuestList(handed) + ". ") +
                                       (fresh.empty() ? "" : "You took " + QuestList(fresh) + " there. ") +
                                       (playerTurnIn.empty() ? "" : playerTurnIn.substr(1) + " ");
                    std::string next = RoundNext(owner, bot, now);
                    _notices.push_back({ bot->GetGUID(), done + next });
                    return;
                }
                if (lead.thenQuest && BackToQuest(owner, bot, lead.thenQuest,
                        (handed.empty() ? "" : "You turned in " + QuestList(handed) + " at " + lead.title + ".") + playerTurnIn + " ", now))
                    return;
                FinishLead(owner, bot, handed.empty() && playerTurnIn.empty()
                    ? "You reached " + lead.title + " but could not turn anything in there. You are coming back to them; tell them."
                    : handed.empty() ? "You reached " + lead.title + "." + playerTurnIn + " Tell them."
                    : "You turned in " + QuestList(handed) + " at " + lead.title + " and are coming back to them." + playerTurnIn + " Tell them briefly.");
                return;
            }
            if (bot->GetDistance2d(lead.x, lead.y) < 12.0f)
            {
                size_t limit = lead.onlyQuest ? 1 : (lead.fetchAll || !lead.fetchFilter.empty()) ? 99 : 1;
                for (Quest const* tq : PickUpAt(owner, bot, lead.fetchEntry, lead.fetchFilter, limit, lead.group, now, lead.onlyQuest))
                    lead.taken.push_back(tq->GetTitle());
                if (lead.onlyQuest && lead.taken.empty())
                {
                    // the chosen quest: whoever could take it has it now (maybe only the others), or it is gone
                    Quest const* oq = sObjectMgr->GetQuestTemplate(lead.onlyQuest);
                    if (oq && !Questers(owner, bot, oq).empty())
                        lead.taken.push_back(oq->GetTitle());
                }
                lead.visited.insert(lead.fetchEntry);
                if (lead.round)
                    _roundVisited[owner->GetGUID()].insert(lead.fetchEntry);
                if (lead.fetchAll)
                {
                    // on to the next quest giver around, if any
                    for (GiverInfo const& g : AvailableGivers(bot, kAreaRadius, lead.fetchFilter,
                                                              lead.fetchFilter.empty() ? LevelCap(owner, bot) : 0,
                                                              lead.fetchFilter.empty() ? Ignored(owner) : nullptr))
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
                if (lead.round)
                {
                    // all quests picked up: on with the round (the quests, in the player's order)
                    std::string said = RoundNext(owner, bot, now);
                    _notices.push_back({ bot->GetGUID(), (all.empty() ? std::string("") : "You picked up " + all + ". ") + said });
                    return;
                }
                if (lead.thenQuest && BackToQuest(owner, bot, lead.thenQuest,
                        all.empty() ? "" : "You picked up " + all + ". ", now))
                    return;
                FinishLead(owner, bot, lead.taken.empty()
                    ? "You reached " + lead.title + " but there was no quest left for you. You are coming back to them; tell them."
                    : "You picked up " + all + " and are coming back to them. Tell them briefly.");
                return;
            }
        }
        Quest const* q = lead.fetch ? nullptr : sObjectMgr->GetQuestTemplate(lead.quest);
        if (!lead.fetch && (!q || Questers(owner, bot, q).empty()))
        {
            // nobody has it any more (turned in, abandoned): in a round on to the next step
            if (lead.round)
                _notices.push_back({ bot->GetGUID(), RoundNext(owner, bot, now) });
            else
                FinishLead(owner, bot, "");
            return;
        }
        // the quest is done when everyone in the party who has it is done (they keep questing together until then)
        bool partyDone = !lead.fetch;
        std::set<uint32> partyTargets;
        if (!lead.fetch)
            for (Player* p : Questers(owner, bot, q))
                if (p->GetQuestStatus(lead.quest) == QUEST_STATUS_INCOMPLETE)
                {
                    partyDone = false;
                    for (uint32 e : TargetEntries(p, q))
                        partyTargets.insert(e);
                }
        if (!lead.fetch && partyDone)
        {
            // walk to the quest ender (if one is around), turn the quest in and come back
            std::string title = lead.title;
            bool round = lead.round;
            if (round)
            {
                auto hunt = _hunts.find(owner->GetGUID());
                if (hunt != _hunts.end() && hunt->second.forLead)
                {
                    ReleaseChasers(hunt->second);
                    _hunts.erase(hunt);
                }
                bot->GetMotionMaster()->Clear();
                std::string next = RoundNext(owner, bot, now);
                _notices.push_back({ bot->GetGUID(), "You finished \"" + title + "\". " + next });
                return;
            }
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
                if (BeginErrand(owner, bot, g.entry, g.name, g.x, g.y, g.z, true, "", false, now))
                {
                    _leads[bot->GetGUID()].group = true;   // the others come along and turn theirs in too
                    _leads[bot->GetGUID()].round = round;  // in a round: then the next quests
                    _notices.push_back({ bot->GetGUID(), "You finished \"" + title + "\" while leading and lead them to " + g.name +
                                                             " to turn it in. Say it in a few words." });
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
        bool ownerNear = (lead.fetch && !lead.group) || (SameWorld(owner, bot) && bot->GetDistance(owner) < 40.0f);
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
            {
                // nothing free here (tagged by other players / bots, or killed): try another spawn spot nearby first
                lead.tried.push_back({ bot->GetPositionX(), bot->GetPositionY() });
                uint32 map = 0;
                float x = 0, y = 0, z = 0;
                bool haveZ = false;
                if (lead.moves < 4 && QuestDestination(bot, q, map, x, y, z, haveZ, &lead.tried, &partyTargets) && haveZ &&
                    bot->GetDistance2d(x, y) < 400.0f)
                {
                    ++lead.moves;
                    lead.hunting = false;
                    lead.x = x;
                    lead.y = y;
                    lead.z = z;
                    lead.haveZ = true;
                    lead.lastMove = 0;
                    lead.stuck = 0;
                    lead.lastX = bot->GetPositionX();
                    lead.lastY = bot->GetPositionY();
                    LOG_INFO("module", "[mod-true-companion] {}: no free quest mobs here, moving on ({} yards, try {})", bot->GetName(),
                             int(bot->GetDistance2d(x, y)), lead.moves);
                    _notices.push_back({ bot->GetGUID(), "There are no free creatures for \"" + lead.title + "\" here right now; you "
                                                             "lead them on to another spot nearby. Say it in a few words." });
                    return;
                }
                if (lead.round)
                {
                    std::string title = lead.title;
                    _notices.push_back({ bot->GetGUID(), "There are no free creatures for \"" + title + "\" around here right now, so you "
                                                             "leave it for later. " + RoundSkip(owner, bot, lead.quest, now) });
                    return;
                }
                FinishLead(owner, bot, "You reached the area of \"" + lead.title + "\" but found no more of the creatures you need "
                                       "right now. You follow them again; tell them and suggest waiting for them to come back.");
            }
            return;
        }

        float dist = bot->GetDistance2d(lead.x, lead.y);
        if (!lead.fetch && dist < 25.0f)
        {
            std::set<uint32> entries = partyTargets;   // what she and everyone else still needs
            if (entries.empty())
            {
                std::string what;
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    if (!q->ObjectiveText[i].empty())
                        what += (what.empty() ? "" : "; ") + q->ObjectiveText[i];
                if (lead.round)
                {
                    // nothing to fight here (objects to use, someone to talk to): that part is up to the player
                    std::string title = lead.title;
                    _notices.push_back({ bot->GetGUID(), "You arrived at the area of \"" + title + "\". " +
                                                             (what.empty() ? std::string("") : "What still needs doing: " + what + ". ") +
                                                             "That part is up to them, so you go on: " +
                                                             RoundSkip(owner, bot, lead.quest, now) });
                    return;
                }
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
            if (lead.round && !lead.fetch)
            {
                std::string title = lead.title;
                _notices.push_back({ bot->GetGUID(), "You could not find a way to \"" + title + "\", so you leave it for later. " +
                                                         RoundSkip(owner, bot, lead.quest, now) });
                return;
            }
            FinishLead(owner, bot, "You could not find a way to the area of \"" + lead.title +
                                       "\". You follow them again; tell them and ask them to lead instead.");
            return;
        }
        float step = std::min(dist, lead.stuck >= 3 ? 25.0f : 60.0f);
        float dx = (lead.x - bot->GetPositionX()) / dist, dy = (lead.y - bot->GetPositionY()) / dist;
        float nx = bot->GetPositionX() + dx * step, ny = bot->GetPositionY() + dy * step;
        // The ground near her own height: searching from far above found roofs (and a quest giver up on a building
        // made her fly up there). With path finding she walks to the closest reachable point of it.
        float nz = bot->GetMapHeight(nx, ny, bot->GetPositionZ() + 6.0f, true, 60.0f);
        if (nz <= INVALID_HEIGHT + 1.0f)
            nz = bot->GetPositionZ();
        // with a path only: when there is none (a quest giver up on a building) she must not fly there in a straight
        // line; she goes as close as she can get
        bot->GetMotionMaster()->MovePoint(0, nx, ny, nz, FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
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
            if (auto again = _leads.find(botGuid); again != _leads.end())
                Escort(owner, bot, again->second, now);
        }
        _notices.erase(std::remove_if(_notices.begin(), _notices.end(), [](auto const& n) { return n.second.empty(); }), _notices.end());
    }

    void CompanionControl::Tend(Player* owner, Player* bot, time_t now)
    {
        if (!_cfg.controlEnabled || !owner || !bot || !bot->IsInWorld() || !bot->IsAlive() || !SameWorld(owner, bot))
            return;
        HealerAssist(owner, bot, now);
        TendBags(owner, bot);
    }

    // Playerbots lets a healer attack only with a lot of mana (and not at all on many maps), so a priest companion often
    // just watched the player fight. While everyone near is healthy she casts her damage spells at the player's target;
    // healing stays with Playerbots and takes over as soon as someone is hurt.
    void CompanionControl::HealerAssist(Player* owner, Player* bot, time_t now)
    {
        if (!_cfg.healerAssist || !PlayerbotsBridge::IsHealer(bot) || _leads.count(bot->GetGUID()))
            return;
        if (!owner->IsInCombat() || bot->IsNonMeleeSpellCast(false) || now - _lastAssist[bot->GetGUID()] < 2)
            return;
        ObjectGuid tg = owner->GetTarget();
        Unit* target = tg ? ObjectAccessor::GetUnit(*owner, tg) : nullptr;
        if (!target || !target->IsAlive())
            target = owner->GetVictim();
        if (!target || !target->IsAlive() || !target->IsInCombat() || !bot->IsValidAttackTarget(target) || !bot->IsWithinLOSInMap(target))
            return;
        if (bot->getPowerType() == POWER_MANA && bot->GetPowerPct(POWER_MANA) < kAssistMinManaPct)
            return;
        if (Group* g = bot->GetGroup())
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    if (m->IsInWorld() && m->IsAlive() && SameWorld(m, bot) && m->GetDistance(bot) < 40.0f && m->GetHealthPct() < kAssistMinHealthPct)
                        return;   // someone needs her heals
        // her damage spells, best first: a damage-over-time that isn't on the target yet, then direct damage
        static char const* const spells[] = { "Shadow Word: Pain", "Moonfire", "Flame Shock", "Holy Fire", "Mind Blast", "Smite", "Wrath",
                                              "Lightning Bolt" };
        std::map<std::string, SpellInfo const*> known;
        for (SpellInfo const* info : KnownSpells(bot))
            known[info->SpellName[0]] = info;
        float dist = bot->GetDistance(target);
        for (char const* name : spells)
        {
            auto it = known.find(name);
            if (it == known.end())
                continue;
            SpellInfo const* info = it->second;
            if (bot->HasSpellCooldown(info->Id) || dist > info->GetMaxRange(false, bot))
                continue;
            if (info->HasAura(SPELL_AURA_PERIODIC_DAMAGE) && target->HasAura(info->Id, bot->GetGUID()))
                continue;
            if (info->PowerType == POWER_MANA && info->CalcPowerCost(bot, info->GetSchoolMask()) > int32(bot->GetPower(POWER_MANA)))
                continue;
            bot->SetFacingToObject(target);
            if (bot->CastSpell(target, info->Id, false) == SPELL_CAST_OK)
            {
                _lastAssist[bot->GetGUID()] = now;
                return;
            }
        }
    }

    // Full bags made her run to corpses again and again (Playerbots finds loot she can't take). Now: when a corpse of the
    // group holds something worth more than her cheapest junk (or a quest item she needs), that junk is dropped so she
    // can loot it; otherwise Playerbots' looting is paused until she has room again.
    void CompanionControl::TendBags(Player* owner, Player* bot)
    {
        ObjectGuid g = bot->GetGUID();
        if (bot->GetFreeInventorySpace() > 0)
        {
            if (_lootPaused.erase(g))
                PlayerbotsBridge::SetLooting(bot, true);
            return;
        }
        if (bot->IsInCombat())
            return;
        // the most valuable thing on the group's corpses around that she may loot and can't fit
        uint64 wanted = 0;
        std::string wantedName;
        for (Creature* c : NearbyCreatures(bot, 30.0f))
        {
            if (c->IsAlive() || c->loot.isLooted() || !c->isTappedBy(owner))
                continue;
            auto consider = [&](LootItem const& li, bool quest)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(li.itemid);
                if (li.is_looted || !proto || !li.AllowedForPlayer(bot, c->GetGUID()))
                    return;
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, li.itemid, li.count) == EQUIP_ERR_OK)
                    return;   // fits on a stack she has
                uint64 value = quest || li.needs_quest ? std::numeric_limits<uint64>::max() / 2
                                                       : uint64(proto->SellPrice) * li.count + (proto->Quality >= ITEM_QUALITY_UNCOMMON ? 1000000 : 0);
                if (value > wanted)
                {
                    wanted = value;
                    wantedName = proto->Name1;
                }
            };
            for (LootItem const& li : c->loot.items)
                consider(li, false);
            for (LootItem const& li : c->loot.quest_items)
                consider(li, true);
        }
        Item* cheapest = nullptr;
        uint64 cheapestValue = 0;
        for (Item* item : JunkItems(bot))
        {
            uint64 v = uint64(item->GetTemplate()->SellPrice) * item->GetCount();
            if (!item->IsInTrade() && (!cheapest || v < cheapestValue))
            {
                cheapest = item;
                cheapestValue = v;
            }
        }
        if (_cfg.dropJunkForLoot && wanted && cheapest && wanted > cheapestValue)
        {
            std::string dropped = cheapest->GetTemplate()->Name1 + (cheapest->GetCount() > 1 ? " x" + std::to_string(cheapest->GetCount()) : "");
            bot->DestroyItem(cheapest->GetBagSlot(), cheapest->GetSlot(), true);
            LOG_INFO("module", "[mod-true-companion] {}: bags full, dropped {} ({} copper) for {}", bot->GetName(), dropped, cheapestValue, wantedName);
            _notices.push_back({ g, "Your bags were full, so you threw away " + dropped + " (junk worth " + MoneyWords(cheapestValue) +
                                    ") to make room for " + wantedName + ". Mention it in a few words." });
            if (_lootPaused.erase(g))
                PlayerbotsBridge::SetLooting(bot, true);
            return;
        }
        if (!_lootPaused.count(g) && PlayerbotsBridge::IsLooting(bot) && PlayerbotsBridge::SetLooting(bot, false))
        {
            _lootPaused.insert(g);
            LOG_INFO("module", "[mod-true-companion] {}: bags full, looting paused until there is room", bot->GetName());
        }
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
        if (freeSlots <= std::max<uint32>(_cfg.autoSellFreeSlots, 4) && !bot->IsInCombat())
        {
            Creature* vendor = _cfg.autoSellJunk ? NearbyVendor(bot, _cfg.interactRange) : nullptr;
            std::vector<Item*> junk = vendor ? JunkItems(bot) : std::vector<Item*>();
            if (vendor && !junk.empty() && freeSlots <= _cfg.autoSellFreeSlots && now - _lastAutoSell[g] > kAutoSellCooldown)
            {
                _lastAutoSell[g] = now;
                SaleResult sale = SellStacks(bot, junk, 0);
                if (sale.stacks)
                    return "Your bags were almost full, so you sold " + std::to_string(sale.stacks) + " junk items to " +
                           vendor->GetName() + " for " + Money(sale.credited) + ". Mention it briefly.";
            }
            // while a round is going she takes them to a merchant herself; otherwise she says so
            bool leading = false;
            for (auto const& [leader, l] : _leads)
                leading = leading || (l.owner == owner->GetGUID() && l.round);
            if (!leading && now - _lastBagsFull[g] > kBagsFullCooldown)
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
        // She is no longer a companion (dismissed; she may stay as a plain Playerbot): everything companion control
        // started for her ends the way a lead ends (FinishLead), and Playerbots' own follow is back.
        Player* b = ObjectAccessor::FindConnectedPlayer(bot);
        Player* owner = nullptr;
        if (auto l = _leads.find(bot); l != _leads.end())
        {
            owner = ObjectAccessor::FindConnectedPlayer(l->second.owner);
            if (owner)
            {
                ReleaseEscort(l->second, owner);   // the ones walking behind her follow the player again
                if (auto h = _hunts.find(owner->GetGUID()); h != _hunts.end() && h->second.forLead)
                {
                    ReleaseChasers(h->second);      // her lead's fight ends with her lead
                    _hunts.erase(h);
                }
            }
            _leads.erase(l);
        }
        // walking behind another companion: her follow was switched off for that
        for (auto& [leader, l] : _leads)
        {
            bool escorted = l.escort.erase(bot) > 0;
            l.escortSkip.erase(bot);
            if (escorted && !owner)
                owner = ObjectAccessor::FindConnectedPlayer(l.owner);
        }
        // part of an area clearing: out of it; if she was walking closer to an enemy, her follow comes back
        bool unfollowed = false;
        for (auto it = _hunts.begin(); it != _hunts.end();)
        {
            Hunt& h = it->second;
            h.bots.erase(std::remove(h.bots.begin(), h.bots.end(), bot), h.bots.end());
            if (h.unfollowed.erase(bot))
            {
                unfollowed = true;
                if (!owner)
                    owner = ObjectAccessor::FindConnectedPlayer(h.owner);
            }
            if (h.bots.empty())
            {
                ReleaseChasers(h);
                it = _hunts.erase(it);
            }
            else
                ++it;
        }
        _lastAutoSell.erase(bot);
        _lastBagsFull.erase(bot);
        _lastAssist.erase(bot);
        if (_lootPaused.erase(bot))
            if (Player* p = ObjectAccessor::FindConnectedPlayer(bot))
                PlayerbotsBridge::SetLooting(p, true);   // a plain Playerbot loots again
        _pending.erase(std::remove_if(_pending.begin(), _pending.end(), [&](Pending const& p) { return p.bot == bot; }), _pending.end());
        if (b && b->IsInWorld())
        {
            if (!owner)
                owner = PlayerbotsBridge::MasterOf(b);
            b->GetMotionMaster()->Clear();
            if (owner)
            {
                PlayerbotsBridge::StopLead(b, owner);   // Playerbots' follow and its own movement again
                if (unfollowed)
                    PlayerbotsBridge::SetFollow(b, owner, true);
            }
        }
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
        else if (ObjectGuid leader = EscortedBy(bot->GetGUID()); !leader.IsEmpty())
        {
            Player* l = ObjectAccessor::FindConnectedPlayer(leader);
            j.Set("follow", std::string("behind"));
            j.Set("leader", l ? l->GetName() : std::string("?"));
        }

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

        // spells her class trainer can teach her now, and the ones coming in the next two levels
        Json learnable = Json::MakeArray();
        Json soon = Json::MakeArray();
        uint32 learnCost = 0;
        std::set<uint32> learnNow;
        std::vector<std::pair<Trainer::Spell const*, Json>> later;
        std::set<uint32> listed;   // a spell several of her trainers teach is listed once
        std::vector<std::pair<Trainer::Trainer*, Trainer::Spell const*>> offers;
        for (uint32 entry : ClassTrainers(bot->getClass()))
            if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry); trainer && IsHerTrainer(bot, entry))
                for (Trainer::Spell const& sp : trainer->GetSpells())
                    offers.emplace_back(trainer, &sp);
        // the ones she can learn now first, so a spell is listed with a trainer that can teach it to her
        std::stable_sort(offers.begin(), offers.end(), [bot](auto const& a, auto const& b)
                         { return a.first->CanTeachSpell(bot, a.second) && !b.first->CanTeachSpell(bot, b.second); });
        for (auto const& [trainer, sp] : offers)
        {
            SpellInfo const* teaches = sSpellMgr->GetSpellInfo(sp->SpellId);
            if (!teaches || !listed.insert(sp->SpellId).second)
                continue;
            // a trainer spell usually casts "learn X": show X, the spell she gets
            SpellInfo const* learned = teaches;
            for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                if (teaches->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && teaches->Effects[e].TriggerSpell)
                    if (SpellInfo const* t = sSpellMgr->GetSpellInfo(teaches->Effects[e].TriggerSpell))
                        learned = t;
            if (bot->HasSpell(learned->Id) || bot->HasSpell(sp->SpellId))
                continue;
            Json s = Json::MakeObject();
            s.Set("id", int64_t(learned->Id));
            s.Set("name", std::string(learned->SpellName[0]));
            s.Set("rank", std::string(learned->Rank[0] ? learned->Rank[0] : ""));
            s.Set("cost", int64_t(sp->MoneyCost));
            s.Set("level", int64_t(sp->ReqLevel));
            if (trainer->CanTeachSpell(bot, sp))
            {
                learnCost += sp->MoneyCost;
                learnable.Push(s);
                learnNow.insert(sp->SpellId);
            }
            else if (sp->ReqLevel > bot->GetLevel() && sp->ReqLevel <= bot->GetLevel() + 2)
                later.emplace_back(sp, s);
        }
        // coming soon: only what she will really be able to learn (not talent spells she lacks, nor ranks after
        // one she can't get)
        for (auto const& [sp, s] : later)
        {
            bool ok = true;
            for (uint32 req : sp->ReqAbility)
                if (req && !bot->HasSpell(req) && !learnNow.count(req))
                    ok = false;
            if (ok && soon.AsArray().size() < 12)
                soon.Push(s);
        }
        j.Set("learnable", learnable);
        j.Set("learnableCost", int64_t(learnCost));
        j.Set("learnSoon", soon);

        Json quests = Json::MakeArray();
        auto log = QuestLog(bot);
        if (auto order = owner ? _questOrder.find(owner->GetGUID()) : _questOrder.end(); order != _questOrder.end())
        {
            auto rank = [&](Quest const* q)
            {
                auto at = std::find(order->second.begin(), order->second.end(), q->GetQuestId());
                return at == order->second.end() ? order->second.size() : size_t(at - order->second.begin());
            };
            std::stable_sort(log.begin(), log.end(), [&](auto const& a, auto const& b) { return rank(a.first) < rank(b.first); });
        }
        if (owner)
            j.Set("levelCap", int64_t(LevelCap(owner, bot)));
        for (auto const& [q, slot] : log)
        {
            Json qj = Json::MakeObject();
            qj.Set("id", int64_t(q->GetQuestId()));
            qj.Set("title", q->GetTitle());
            qj.Set("level", int64_t(q->GetQuestLevel() > 0 ? q->GetQuestLevel() : int(bot->GetLevel())));   // -1 = scales with her level
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
