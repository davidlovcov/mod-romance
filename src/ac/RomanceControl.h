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
#include <optional>
#include <string>
#include <vector>

class Player;

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

        void Forget(ObjectGuid bot);

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
        struct Lead
        {
            uint32 quest = 0;
            std::string title;
            time_t since = 0;
        };

        std::string GiveItem(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string GiveGold(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string Cast(Player* owner, Player* bot, CompanionCommand const& cmd);
        std::string Attack(Player* owner, Player* bot, CompanionCommand const& cmd);
        std::string Sell(Player* owner, Player* bot, CompanionCommand const& cmd);
        std::string Share(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string StartLead(Player* owner, Player* bot, CompanionCommand const& cmd, time_t now);
        std::string StopLead(Player* owner, Player* bot);
        void Trade(Player* owner, Player* bot, std::string const& command, time_t now);

        RomanceConfig const& _cfg;
        std::vector<Pending> _pending;
        std::map<ObjectGuid, Lead> _leads;
        std::map<ObjectGuid, time_t> _lastAutoSell;
        std::map<ObjectGuid, time_t> _lastBagsFull;
    };
}

#endif
