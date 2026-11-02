/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Thin bridge to mod-playerbots. Everything Playerbots-specific is isolated
 * here; the module compiles (with summoning disabled) when Playerbots is absent.
 * Combat, movement, spells and following stay entirely with the Playerbots AI:
 * this bridge only logs companions in/out and forwards validated chat commands.
 */

#ifndef MOD_ROMANCE_PLAYERBOTS_H
#define MOD_ROMANCE_PLAYERBOTS_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;

namespace romance::ac
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

        // Lead mode: Playerbots stops following so mod-romance can walk the companion itself
        // (see CompanionControl); StopLead switches back to following.
        static bool StartLead(Player* bot, Player* master);
        static bool StopLead(Player* bot, Player* master);
    };
}

#endif
