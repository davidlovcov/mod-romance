/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Script hooks. Only the hooks listed in each constructor are registered, so
 * the per-packet hooks cost nothing for other opcodes.
 */

#include "RomanceManager.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "Group.h"
#include "GroupScript.h"
#include "Item.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;

namespace
{
    class RomanceWorldScript : public WorldScript
    {
    public:
        RomanceWorldScript()
            : WorldScript("mod_romance_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_SHUTDOWN, WORLDHOOK_ON_UPDATE }) { }

        void OnAfterConfigLoad(bool reload) override { sRomanceMgr.OnConfigLoad(reload); }
        void OnStartup() override { sRomanceMgr.OnStartup(); }
        void OnShutdown() override { sRomanceMgr.OnShutdown(); }
        // Runs after MapMgr::Update() has completed, on the world thread.
        void OnUpdate(uint32 diff) override { sRomanceMgr.Update(diff); }
    };

    class RomancePlayerScript : public PlayerScript
    {
    public:
        RomancePlayerScript()
            : PlayerScript("mod_romance_player", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
                                                   PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_CREATURE_KILL,
                                                   PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_STORE_NEW_ITEM,
                                                   PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT }) { }

        // Whispers (and our addon messages, which are whispers to oneself in LANG_ADDON).
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER)
                return true;
            return sRomanceMgr.OnWhisper(player, receiver, language, msg);
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Group* /*group*/) override
        {
            return sRomanceMgr.OnGroupChat(player, type, language, msg);
        }

        void OnPlayerLogin(Player* player) override { sRomanceMgr.QueueLogin(player->GetGUID()); }
        void OnPlayerLogout(Player* player) override { sRomanceMgr.QueueLogout(player->GetGUID()); }

        void OnPlayerCreatureKill(Player* killer, Creature* killed) override
        {
            if (!sRomanceMgr.Enabled() || !killer || !killed)
                return;
            bool notable = killed->isElite() || killed->isWorldBoss();
            sRomanceMgr.QueueKill(killer->GetGUID(), killed->GetName(), notable);
        }

        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            if (sRomanceMgr.Enabled() && player && quest)
                sRomanceMgr.QueueQuestComplete(player->GetGUID(), quest->GetTitle(), quest->GetQuestId());
        }

        void OnPlayerQuestAccept(Player* player, Quest const* quest) override
        {
            if (sRomanceMgr.Enabled() && player && quest)
                sRomanceMgr.QueueQuestAccept(player->GetGUID(), quest->GetQuestId());
        }

        void OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/) override
        {
            if (!sRomanceMgr.Enabled() || !player || !item || !item->GetTemplate())
                return;
            if (item->GetTemplate()->Quality >= ITEM_QUALITY_EPIC)
                sRomanceMgr.QueueLoot(player->GetGUID(), item->GetTemplate()->Name1);
        }
    };

    class RomanceServerScript : public ServerScript
    {
    public:
        RomanceServerScript() : ServerScript("mod_romance_server", { SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_CAN_PACKET_SEND }) { }

        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            // CMSG_ACCEPT_TRADE is PROCESS_THREADUNSAFE: this runs on the world thread before the handler.
            if (packet.GetOpcode() == CMSG_ACCEPT_TRADE && session)
                sRomanceMgr.OnTradeAcceptPacket(session);
            return true;
        }

        bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
        {
            if (packet.GetOpcode() == SMSG_TRADE_STATUS && session && packet.size() >= 4)
                sRomanceMgr.OnTradeStatusPacket(session, packet.read<uint32>(0));
            return true;
        }
    };

    class RomanceCommandScript : public CommandScript
    {
    public:
        RomanceCommandScript() : CommandScript("mod_romance_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable romanceTable = {
                { "status", HandleStatus, SEC_PLAYER, Console::No },
                { "summon", HandleSummon, SEC_PLAYER, Console::No },
                { "dismiss", HandleDismiss, SEC_PLAYER, Console::No },
                { "on", HandleOn, SEC_PLAYER, Console::No },
                { "off", HandleOff, SEC_PLAYER, Console::No },
            };
            static ChatCommandTable commandTable = {
                { "romance", romanceTable },
            };
            return commandTable;
        }

        static Player* Self(ChatHandler* handler) { return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr; }

        static bool HandleStatus(ChatHandler* handler)
        {
            handler->SendSysMessage(sRomanceMgr.CommandStatus(Self(handler)));
            return true;
        }

        static bool HandleSummon(ChatHandler* handler, Tail name)
        {
            Player* p = Self(handler);
            if (!p || name.empty())
            {
                handler->SendSysMessage("Usage: .romance summon <character name>");
                return true;
            }
            handler->SendSysMessage(sRomanceMgr.CommandSummon(p, std::string(name)));
            return true;
        }

        static bool HandleDismiss(ChatHandler* handler)
        {
            if (Player* p = Self(handler))
                handler->SendSysMessage(sRomanceMgr.CommandDismiss(p));
            return true;
        }

        static bool HandleOn(ChatHandler* handler)
        {
            if (Player* p = Self(handler))
                handler->SendSysMessage(sRomanceMgr.CommandToggleAi(p, true));
            return true;
        }

        static bool HandleOff(ChatHandler* handler)
        {
            if (Player* p = Self(handler))
                handler->SendSysMessage(sRomanceMgr.CommandToggleAi(p, false));
            return true;
        }
    };
}

namespace
{
    // Kicking a companion from the party (or disbanding it) dismisses her; before she stayed and kept following.
    class RomanceGroupScript : public GroupScript
    {
    public:
        RomanceGroupScript() : GroupScript("mod_romance_group", { GROUPHOOK_ON_REMOVE_MEMBER, GROUPHOOK_ON_DISBAND }) { }

        void OnRemoveMember(Group* /*group*/, ObjectGuid guid, RemoveMethod /*method*/, ObjectGuid /*kicker*/, char const* /*reason*/) override
        {
            if (sRomanceMgr.Enabled())
                sRomanceMgr.QueueGroupRemoval(guid);
        }

        void OnDisband(Group* group) override
        {
            if (!sRomanceMgr.Enabled() || !group)
                return;
            for (auto const& slot : group->GetMemberSlots())
                sRomanceMgr.QueueGroupRemoval(slot.guid);
        }
    };
}

void Addmod_romanceScripts()
{
    new RomanceGroupScript();
    new RomanceWorldScript();
    new RomancePlayerScript();
    new RomanceServerScript();
    new RomanceCommandScript();
}
