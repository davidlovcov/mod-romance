/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Companion control on the game thread:
 *  - game knowledge for the prompt (bags, gear, quests, spells, gold, XP, the player's target);
 *  - natural-language commands parsed by romance::ParseCompanionCommand, executed through
 *    Playerbots chat commands ("t", "cast", "s", "share", "d attack rti target", lead mode)
 *    with an honest outcome sentence for the prompt;
 *  - background jobs: two-step trades, paced quest shares, auto-selling junk, lead mode;
 *  - the character sheet shown in the addon.
 * World thread only.
 */

#ifndef MOD_ROMANCE_CONTROL_H
#define MOD_ROMANCE_CONTROL_H

#include "RomanceCommands.h"
#include "RomanceConfig.h"
#include "RomanceJson.h"
#include "RomanceTypes.h"

#include "Define.h"
#include "ObjectGuid.h"

#include <ctime>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>

class Player;
class Unit;
class Quest;

namespace romance::ac
{
    class CompanionControl
    {
    public:
        explicit CompanionControl(RomanceConfig const& cfg) : _cfg(cfg) {}

        // Adds the requested knowledge lines (see romance::knowledge) to ctx.knowledge.
        void AddKnowledge(WorldContext& ctx, Player* owner, Player* bot, uint32 topics) const;

        // Carries out `cmd`; returns the outcome sentence for the prompt (second person: "You ...").
        std::string Execute(Player* owner, Player* bot, CompanionCommand const& cmd, std::optional<Stage> stage, time_t now);

        // Every world tick: re-issues trade commands once the trade window is open, paces quest shares.
        void Update(time_t now);

        // Periodic per active companion: auto-sell junk, bags-full and lead-finished notices.
        // Returns a situation text for a CompanionNotice event (empty = nothing to say).
        std::string Poll(Player* owner, Player* bot, time_t now);

        Json CharacterSheet(Player* owner, Player* bot) const;

        // Quest sync: the owner took / turned in a quest; a companion nearby does the same if she can.
        // Returns a short situation text (empty = nothing happened).
        std::string SyncQuestAccept(Player* owner, Player* bot, uint32 questId);
        std::string SyncQuestReward(Player* owner, Player* bot, uint32 questId);

        // Notices produced by background jobs (area cleared, lead finished): (companion, situation text).
        std::vector<std::pair<ObjectGuid, std::string>> TakeNotices();

        bool IsLeading(ObjectGuid bot) const { return _leads.count(bot) > 0; }

        // The player's quest order for the group (quest ids, first = next). `apply`: a companion leading right now
        // switches to the new first quest.
        void SetQuestOrder(Player* owner, std::vector<uint32> order, bool apply, time_t now);
        // Quests the player never wants done: not picked up, not led to.
        void SetIgnoredQuests(ObjectGuid owner, std::set<uint32> ids) { _ignored[owner] = std::move(ids); }
        // Abandons the quest for every companion in the player's group; returns how many dropped it.
        uint32 AbandonForCompanions(Player* owner, uint32 questId);
        // Everything for the addon's group quest tab: the party's quests (who has which), quests available around.
        Json GroupQuests(Player* owner) const;
        // "Wait for me to loot" (companion setting): after a fight she also waits for the player's loot.
        void SetWaitForPlayerLoot(ObjectGuid bot, bool on) { if (on) _waitLoot.insert(bot); else _waitLoot.erase(bot); }

        // The companion she walks behind while that one leads (empty if none).
        ObjectGuid EscortedBy(ObjectGuid bot) const;

        // True while she is leading, on an errand or clearing an area: then the AI's own movement actions
        // (follow / stay / attack / flee in a reply) must not interfere.
        bool IsBusy(ObjectGuid owner, ObjectGuid bot) const;

        // The player said "follow me": a lead or an area clearing ends properly (not just Playerbots' follow).
        void OnPlayerFollowOrder(Player* owner, Player* bot);

        void Forget(ObjectGuid bot);

        // A companion was called: Playerbots' follow back on (a lead before a restart may have left it off).
        void OnActivated(Player* owner, Player* bot);

        // Builds the quest-giver and kill-credit indexes now (at startup) instead of on the first request.
        void WarmUp() const;

    private:
        struct Pending
        {
            ObjectGuid bot;
            ObjectGuid owner;
            std::string command;
            time_t notBefore = 0;
            time_t deadline = 0;
            bool needsTrade = false;   // wait until the trade window with the owner is open
        };
        // Clearing an area (or the quest mobs at a lead destination): one marked enemy after another.
        struct Hunt
        {
            ObjectGuid owner;
            std::vector<ObjectGuid> bots;
            std::string filter;            // name filter (empty = any enemy)
            uint32 creatureType = 0;       // creature type filter
            std::set<uint32> entries;      // creature entries (lead mode: quest mobs)
            float radius = 40.0f;
            time_t until = 0;
            ObjectGuid current;
            uint32 kills = 0;
            bool forLead = false;
            std::map<ObjectGuid, time_t> lastOrder;   // attack orders are re-sent at most every few seconds
            time_t currentSince = 0;                  // when the current target was marked
            std::set<ObjectGuid> skipped;             // targets nobody could reach (behind walls, ...)
            ObjectGuid chase;                         // out of sight: walk closer first (Playerbots won't attack what it can't see)
            std::set<ObjectGuid> unfollowed;          // bots whose Playerbots "follow" is off while they walk closer
            ObjectGuid only;                          // a single attack: just this enemy
            uint8 maxLevel = 0;                       // grinding: leave enemies far above the group alone
            uint32 seen = 0, tapped = 0, other = 0;   // last search (for the log): enemies around, tagged by others, not wanted
            std::map<ObjectGuid, time_t> toLoot;      // our kills still to loot (quest items!), and since when
            time_t lastLootCmd = 0;
            time_t chaseSince = 0, lastChase = 0;
        };
        // Lead mode: she walks to the quest area (you follow), then hunts the quest mobs there.
        struct Lead
        {
            ObjectGuid owner;
            uint32 quest = 0;
            std::string title;
            time_t since = 0;
            uint32 map = 0;
            float x = 0, y = 0, z = 0;
            bool haveZ = false;
            bool hunting = false;
            bool waiting = false;
            time_t lastMove = 0;
            float lastX = 0, lastY = 0;
            uint32 stuck = 0;
            bool paused = false;           // "wait": she stays where she is and keeps the lead
            // fetch mode: walk alone to a quest giver, pick up quests there, come back
            bool fetch = false;
            uint32 fetchEntry = 0;
            std::string fetchFilter;       // quest title words, "class" = class quests only
            bool turnIn = false;           // fetch mode, but turn finished quests in at fetchEntry
            bool fetchAll = false;         // pick up from every giver around (else only one quest)
            bool group = false;            // "lead the way" without a quest: the player and the other companions come
                                           // along, everyone picks up quests, then she leads to them (or grinds)
            bool round = false;            // a "lead the way" round: after a giver / turn-in she goes on to the next
            bool sell = false;             // errand to a merchant: everyone sells their junk there
            std::set<ObjectGuid> escort;   // other companions walking behind her (their Playerbots follow is off)
            std::set<ObjectGuid> escortSkip;   // told "follow me" by the player: they follow the player, not her
            time_t lastEscort = 0;
            std::set<uint32> visited;      // givers already visited on this errand
            std::vector<std::string> taken;   // quests picked up so far on this errand
            uint32 moves = 0;              // lead: spots tried after one had no free quest mobs
            std::vector<std::pair<float, float>> tried;
        };

        std::string GiveItem(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string GiveGold(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string Cast(Player* owner, Player* bot, CompanionCommand const& cmd);
        std::string Attack(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string ChaseAttack(Player* owner, Player* bot, Unit* target, time_t now);
        void ReleaseChasers(Hunt& h);
        std::string Sell(Player* owner, Player* bot, CompanionCommand const& cmd);
        std::string Share(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string AttackAll(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string StopAttack(Player* owner, Player* bot);
        std::string AcceptQuests(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string TurnInQuests(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        bool HuntStep(Hunt& h, time_t now);           // false = finished
        void LeadStep(Player* owner, Player* bot, Lead& lead, time_t now);
        void FinishLead(Player* owner, Player* bot, std::string const& notice);
        bool BeginErrand(Player* owner, Player* bot, uint32 entry, std::string const& name, float x, float y, float z, bool turnIn,
                         std::string const& filter, bool all, time_t now);
        std::string StartLead(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string StopLead(Player* owner, Player* bot);
        std::string ResumeLead(Player* owner, Player* bot);
        std::string BeginQuestLead(Player* owner, Player* bot, Quest const* pick, time_t now);
        std::string QuestRound(Player* owner, Player* bot, time_t now);
        std::string Grind(Player* owner, Player* bot, time_t now);
        Quest const* NextQuest(Player* owner, Player* bot) const;
        // Every quest she could lead to now, best first (the group's quest order, the group's quest logs).
        std::vector<Quest const*> QuestCandidates(Player* owner, Player* bot) const;
        // Starts a lead to the first candidate that has a known place; "" if none.
        std::string LeadToNextQuest(Player* owner, Player* bot, time_t now);
        // In a round: leave `quest` for now (she can't do it / can't get there) and go on with the next step.
        std::string RoundSkip(Player* owner, Player* bot, uint32 quest, time_t now);
        std::vector<Player*> OtherCompanions(Player* owner, Player* bot) const;
        // Everyone in the party with `q` in the quest log: the leader, the player, the other companions.
        std::vector<Player*> Questers(Player* owner, Player* bot, Quest const* q) const;
        std::set<uint32> PartyTargets(Player* owner, Player* bot, Quest const* q) const;
        bool PartyNeeds(Player* owner, Player* bot, Quest const* q) const;   // someone still has it unfinished
        // Highest quest level worth doing: the group's average level plus 1..4 (more people, a little higher).
        int LevelCap(Player* owner, Player* bot) const;
        std::string RoundNext(Player* owner, Player* bot, time_t now);
        // Quests from quest giver `entry` (within talking range) for her, and with `group` for the companions too and
        // shared with the player.
        std::vector<Quest const*> PickUpAt(Player* owner, Player* bot, uint32 entry, std::string const& filter, size_t limit, bool group,
                                           time_t now);
        bool NeedsSelling(Player* owner, Player* bot, time_t now) const;
        void Escort(Player* owner, Player* bot, Lead& lead, time_t now);
        void ReleaseEscort(Lead& lead, Player* owner);
        void Trade(Player* owner, Player* bot, std::string const& command, time_t now);

        RomanceConfig const& _cfg;
        std::vector<Pending> _pending;
        std::map<ObjectGuid, Lead> _leads;               // by companion
        std::map<ObjectGuid, Hunt> _hunts;               // by owner
        std::vector<std::pair<ObjectGuid, std::string>> _notices;
        std::map<ObjectGuid, std::vector<uint32>> _questOrder;   // by player
        std::map<ObjectGuid, std::set<uint32>> _ignored;          // by player
        std::set<uint32> const* Ignored(Player* owner) const;
        std::set<ObjectGuid> _waitLoot;
        std::map<ObjectGuid, time_t> _lastSellTrip;
        std::map<ObjectGuid, std::set<uint32>> _roundSkip;      // by player: quests left out of this round
        std::map<ObjectGuid, std::set<uint32>> _roundVisited;   // by player: NPCs already visited in this phase of the round
        time_t _lastStep = 0;
        std::map<ObjectGuid, time_t> _lastAutoSell;
        std::map<ObjectGuid, time_t> _lastBagsFull;
    };
}

#endif
