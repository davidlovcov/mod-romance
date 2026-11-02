/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Thin bridge to mod-playerbots. Everything Playerbots-specific is isolated
 * here; the module compiles (with summoning disabled) when Playerbots is absent.
 * Combat, movement, spells and following stay entirely with the Playerbots AI:
 * this bridge only logs companions in/out and forwards validated chat commands.
 */

#ifndef MOD_TRUE_COMPANION_PLAYERBOTS_H
#define MOD_TRUE_COMPANION_PLAYERBOTS_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;

namespace truecompanion::ac
{
    class PlayerbotsBridge
    {
    public:
        static bool Available();
        // Playerbots settings that disturb companions (empty = fine); logged at startup.
        static std::string ConfigWarnings();
        static bool IsBot(Player* player);
        static Player* MasterOf(Player* bot);
        static bool IsMyBot(Player* bot, Player* master);
        static bool IsAccountLinked(Player* master, uint32 accountId);
        // Accounts linked to `account` with ".playerbots account link" (candidates; verify with IsAccountLinked).
        static std::vector<uint32> LinkedAccounts(uint32 account);
        // Requests a login through PlayerbotHolder::AddPlayerBot (asynchronous). Returns "ok" or an error code.
        static std::string Summon(Player* master, ObjectGuid companion);
        static bool Dismiss(Player* master, ObjectGuid companion);
        // Routes a plain Playerbots chat command ("follow", "stay", ...) as if whispered by the master.
        static bool Command(Player* bot, Player* master, std::string const& command);
        static bool SayToParty(Player* bot, std::string const& text);
        // follow | stay | lead | unknown
        static std::string FollowState(Player* bot);
        // Bot runs a healing strategy: Playerbots refuses attack orders for healers ("attack rti target: useless").
        static bool IsHealer(Player* bot);
        // Sets the bot's raid target icon ("skull") directly, without the chat reply of the "rti" command.
        static bool SetRti(Player* bot, std::string const& icon);

        // Lead mode: Playerbots stops following so mod-true-companion can walk the companion itself
        // (see CompanionControl); StopLead switches back to following.
        static bool StartLead(Player* bot, Player* master);
        static bool StopLead(Player* bot, Player* master);
        // Drops the current target and stops auto-attacking (the bot is still in combat if enemies fight back).
        static bool StopAttack(Player* bot, Player* master);
        // Adds / removes Playerbots' non-combat "follow" strategy only (no movement, no "Following" reply).
        static bool SetFollow(Player* bot, Player* master, bool on);
        // Playerbots' non-combat "loot" strategy: whether she has it, and switching it on or off (no chat reply). Used
        // while her bags are full, so she doesn't walk to corpses she can't take anything from over and over.
        static bool IsLooting(Player* bot);
        static bool SetLooting(Player* bot, bool on);
        // Playerbots' own "go x;y;z" (its movement system, so its strategies don't cancel the walk).
        static bool MoveTo(Player* bot, Player* master, float x, float y, float z);
    };
}

#endif
