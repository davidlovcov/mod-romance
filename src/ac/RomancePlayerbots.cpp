/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomancePlayerbots.h"

#include "Player.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#if __has_include("Playerbots.h") && __has_include("PlayerbotMgr.h")
#  define ROMANCE_WITH_PLAYERBOTS 1
#  include "PlayerbotAI.h"
#  include "PlayerbotMgr.h"
#  include "Playerbots.h"
#  include "PlayerbotsDatabase.h"
#else
#  define ROMANCE_WITH_PLAYERBOTS 0
#endif

namespace romance::ac
{
#if ROMANCE_WITH_PLAYERBOTS
    bool PlayerbotsBridge::Available() { return true; }

    bool PlayerbotsBridge::IsBot(Player* player)
    {
        if (!player)
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        return ai && ai->IsBotAI();
    }

    Player* PlayerbotsBridge::MasterOf(Player* bot)
    {
        if (!bot)
            return nullptr;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        return (ai && ai->IsBotAI()) ? ai->GetMaster() : nullptr;
    }

    bool PlayerbotsBridge::IsMyBot(Player* bot, Player* master)
    {
        if (!bot || !master)
            return false;
        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master);
        return mgr && mgr->GetPlayerBot(bot->GetGUID()) == bot;
    }

    bool PlayerbotsBridge::IsAccountLinked(Player* master, uint32 accountId)
    {
        if (!master)
            return false;
        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master);
        return mgr && mgr->IsAccountLinked(accountId, master->GetSession()->GetAccountId());
    }

    std::vector<uint32> PlayerbotsBridge::LinkedAccounts(uint32 account)
    {
        std::vector<uint32> out;
        PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_ACCOUNT_LINKS_BY_ACCOUNT);
        stmt->SetData(0, account);
        if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
        {
            do
                out.push_back(result->Fetch()[0].Get<uint32>());
            while (result->NextRow());
        }
        return out;
    }

    std::string PlayerbotsBridge::Summon(Player* master, ObjectGuid companion)
    {
        if (!master)
            return "no_master";
        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master);
        if (!mgr)
            return "playerbots_unavailable";
        // AddPlayerBot performs Playerbots' own checks again (account/linked/MaxAddedBots) and ignores
        // duplicate requests for a character that is already loading or online.
        mgr->AddPlayerBot(companion, master->GetSession()->GetAccountId());
        return "ok";
    }

    bool PlayerbotsBridge::Dismiss(Player* master, ObjectGuid companion)
    {
        if (!master)
            return false;
        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master);
        if (!mgr || !mgr->GetPlayerBot(companion))
            return false;
        mgr->LogoutPlayerBot(companion);
        return true;
    }

    bool PlayerbotsBridge::Command(Player* bot, Player* master, std::string const& command)
    {
        if (!bot || !master)
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai || !ai->IsBotAI() || ai->GetMaster() != master)
            return false;
        // Same entry point as a whispered command, including Playerbots' security checks.
        ai->HandleCommand(CHAT_MSG_WHISPER, command, master);
        return true;
    }

    bool PlayerbotsBridge::SayToParty(Player* bot, std::string const& text)
    {
        PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        return ai && ai->SayToParty(text);
    }

    std::string PlayerbotsBridge::FollowState(Player* bot)
    {
        PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai)
            return "unknown";
        if (ai->HasStrategy("stay", BOT_STATE_NON_COMBAT))
            return "stay";
        if (ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            return "follow";
        return "unknown";
    }
#else
    bool PlayerbotsBridge::Available() { return false; }
    bool PlayerbotsBridge::IsBot(Player*) { return false; }
    Player* PlayerbotsBridge::MasterOf(Player*) { return nullptr; }
    bool PlayerbotsBridge::IsMyBot(Player*, Player*) { return false; }
    bool PlayerbotsBridge::IsAccountLinked(Player*, uint32) { return false; }
    std::vector<uint32> PlayerbotsBridge::LinkedAccounts(uint32) { return {}; }
    std::string PlayerbotsBridge::Summon(Player*, ObjectGuid) { return "playerbots_not_installed"; }
    bool PlayerbotsBridge::Dismiss(Player*, ObjectGuid) { return false; }
    bool PlayerbotsBridge::Command(Player*, Player*, std::string const&) { return false; }
    bool PlayerbotsBridge::SayToParty(Player*, std::string const&) { return false; }
    std::string PlayerbotsBridge::FollowState(Player*) { return "unknown"; }
#endif
}
