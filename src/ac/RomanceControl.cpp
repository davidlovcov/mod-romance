/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceControl.h"
#include "RomancePlayerbots.h"

#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
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
#include <cstdio>
#include <set>

namespace romance::ac
{
    namespace
    {
        constexpr uint8 kSkullIcon = 7;
        constexpr float kTradeDistance = 10.0f;
        constexpr float kVendorDistance = 6.0f;
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

        // Format Playerbots' ChatHelper::parseMoney understands: "10g5s3c" (no spaces).
        std::string MoneyCommand(uint64 copper)
        {
            std::string m = Money(copper);
            m.erase(std::remove(m.begin(), m.end(), ' '), m.end());
            return m;
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

        Creature* NearbyVendor(Player* bot)
        {
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(bot, kVendorDistance);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
            Cell::VisitObjects(bot, searcher, kVendorDistance);
            for (Unit* u : units)
                if (Creature* c = u->ToCreature())
                    if (c->IsVendor() && c->IsAlive() && !c->IsHostileTo(bot))
                        return c;
            return nullptr;
        }

        uint32 JunkCount(Player* bot, uint64* value)
        {
            uint32 n = 0;
            uint64 v = 0;
            ForEachBagItem(bot, [&](Item* item)
            {
                ItemTemplate const* proto = item->GetTemplate();
                if (proto && proto->Quality == ITEM_QUALITY_POOR && proto->SellPrice > 0)
                {
                    ++n;
                    v += uint64(proto->SellPrice) * item->GetCount();
                }
            });
            if (value)
                *value = v;
            return n;
        }
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
            return "Nothing. They asked you to hand over your belongings, but you don't know them well enough for that yet; "
                   "politely refuse.";
        switch (cmd.kind)
        {
            case CompanionCommandKind::GiveItem: return GiveItem(owner, bot, cmd, now);
            case CompanionCommandKind::GiveGold: return GiveGold(owner, bot, cmd, now);
            case CompanionCommandKind::Cast: return Cast(owner, bot, cmd);
            case CompanionCommandKind::AttackNamed:
            case CompanionCommandKind::AttackTarget: return Attack(owner, bot, cmd);
            case CompanionCommandKind::SellJunk:
            case CompanionCommandKind::SellItem: return Sell(owner, bot, cmd);
            case CompanionCommandKind::ShareQuests:
            case CompanionCommandKind::ShareQuest: return Share(owner, bot, cmd, now);
            case CompanionCommandKind::Lead: return StartLead(owner, bot, cmd, now);
            case CompanionCommandKind::StopLead: return StopLead(owner, bot);
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
        if (bot->GetMapId() != owner->GetMapId() || bot->GetDistance(owner) > kTradeDistance)
            return "Nothing yet. They asked for " + Money(amount) + ", but you are too far apart to trade; ask them to come closer.";
        if (owner->GetTrader() && owner->GetTrader() != bot)
            return "Nothing. They are trading with someone else right now.";
        Trade(owner, bot, "t " + MoneyCommand(amount), now);
        return "You opened a trade with them and put " + Money(amount) + " in it; they only need to accept.";
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

    std::string CompanionControl::Attack(Player* owner, Player* bot, CompanionCommand const& cmd)
    {
        if (cmd.kind == CompanionCommandKind::AttackTarget)
        {
            ObjectGuid tg = owner->GetTarget();
            Unit* u = tg ? ObjectAccessor::GetUnit(*owner, tg) : nullptr;
            if (!u || !bot->IsValidAttackTarget(u))
                return "Nothing. They told you to attack, but they have no enemy targeted; tell them.";
            PlayerbotsBridge::Command(bot, owner, "attack");
            return "You attack " + u->GetName() + ".";
        }

        uint32 type = CreatureTypeFromWord(cmd.arg);
        std::list<Unit*> units;
        float range = _cfg.commandRange;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        Unit* best = nullptr;
        float bestDist = 0;
        for (Unit* u : units)
        {
            if (!u->IsAlive() || !bot->IsValidAttackTarget(u) || u->ToPlayer())
                continue;
            Creature* c = u->ToCreature();
            bool match = type ? (c && c->GetCreatureType() == type) : MatchScore(u->GetName(), cmd.arg) > 0;
            if (!match)
                continue;
            float d = bot->GetDistance(u);
            if (!best || d < bestDist)
            {
                best = u;
                bestDist = d;
            }
        }
        if (!best)
            return "Nothing. They told you to attack \"" + cmd.arg + "\", but there is no such enemy nearby; tell them.";
        Group* group = bot->GetGroup();
        if (group && group->IsMember(owner->GetGUID()))
        {
            // Mark it with a skull so the player sees which one, then attack the marked target.
            group->SetTargetIcon(kSkullIcon, owner->GetGUID(), best->GetGUID());
            PlayerbotsBridge::Command(bot, owner, "rti skull");
            PlayerbotsBridge::Command(bot, owner, "d attack rti target");
            return "You marked " + DescribeUnit(best, bot) + " with a skull and attacked it.";
        }
        return "Nothing. You can only pick fights together when you are in the same group; tell them.";
    }

    std::string CompanionControl::Sell(Player* owner, Player* bot, CompanionCommand const& cmd)
    {
        Creature* vendor = NearbyVendor(bot);
        if (!vendor)
            return "Nothing yet. There is no merchant right next to you; you can only sell when you stand at a vendor. Tell them.";
        if (cmd.kind == CompanionCommandKind::SellJunk)
        {
            uint64 value = 0;
            uint32 n = JunkCount(bot, &value);
            if (!n)
                return "Nothing. You have no junk to sell; tell them.";
            PlayerbotsBridge::Command(bot, owner, "s gray");
            return "You sold " + std::to_string(n) + " junk items to " + vendor->GetName() + " for about " + Money(value) + ".";
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
        PlayerbotsBridge::Command(bot, owner, "s " + ItemLink(best->proto));
        return "You sold your " + best->proto->Name1 + " to " + vendor->GetName() + ".";
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

    std::string CompanionControl::StartLead(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now)
    {
        if (!_cfg.leadMode || !PlayerbotsBridge::Available())
            return "Nothing. Leading the way is not possible here; you keep following them. Tell them.";
        auto log = QuestLog(bot);
        Quest const* pick = nullptr;
        if (!cmd.arg.empty())
        {
            int bestScore = 0;
            for (auto const& [q, slot] : log)
            {
                int s = MatchScore(q->GetTitle(), cmd.arg);
                if (s > bestScore)
                {
                    pick = q;
                    bestScore = s;
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
        if (!PlayerbotsBridge::StartLead(bot, owner, pick->GetQuestId()))
            return "Nothing. You could not take the lead right now; tell them.";
        _leads[bot->GetGUID()] = { pick->GetQuestId(), pick->GetTitle(), now };
        return "You took the lead and are now walking to your quest \"" + pick->GetTitle() + "\"; they follow you. "
               "Tell them where you are going.";
    }

    std::string CompanionControl::StopLead(Player* owner, Player* bot)
    {
        bool was = _leads.erase(bot->GetGUID()) > 0 || PlayerbotsBridge::IsLeading(bot);
        PlayerbotsBridge::StopLead(bot, owner);
        return was ? "You stopped leading and follow them again." : "You follow them (you were not leading).";
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
    }

    std::string CompanionControl::Poll(Player* owner, Player* bot, time_t now)
    {
        if (!_cfg.controlEnabled || !owner || !bot || !bot->IsAlive())
            return "";
        ObjectGuid g = bot->GetGUID();

        // lead mode: finished or interrupted -> follow again and ask what's next
        auto lead = _leads.find(g);
        if (lead != _leads.end() && now - lead->second.since > 5)
        {
            bool done = bot->GetQuestStatus(lead->second.quest) == QUEST_STATUS_COMPLETE;
            bool stopped = PlayerbotsBridge::LeadQuest(bot) != lead->second.quest;
            if (done || stopped)
            {
                std::string title = lead->second.title;
                _leads.erase(lead);
                PlayerbotsBridge::StopLead(bot, owner);
                return done ? "You finished the objectives of \"" + title + "\" while leading. You follow them again now; "
                              "tell them and ask what they want to do next."
                            : "You stopped leading the way to \"" + title + "\" and follow them again; ask what they want to do next.";
            }
        }

        // bags nearly full
        uint32 freeSlots = bot->GetFreeInventorySpace();
        if (freeSlots <= _cfg.autoSellFreeSlots && !bot->IsInCombat())
        {
            Creature* vendor = _cfg.autoSellJunk ? NearbyVendor(bot) : nullptr;
            uint64 value = 0;
            uint32 junk = vendor ? JunkCount(bot, &value) : 0;
            if (vendor && junk && now - _lastAutoSell[g] > kAutoSellCooldown)
            {
                _lastAutoSell[g] = now;
                PlayerbotsBridge::Command(bot, owner, "s gray");
                return "Your bags were almost full, so you sold " + std::to_string(junk) + " junk items to " + vendor->GetName() +
                       " for about " + Money(value) + ". Mention it briefly.";
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
        return j;
    }
}
