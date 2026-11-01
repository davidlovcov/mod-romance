/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * RomanceManager glues the engine-independent RomanceService to AzerothCore.
 *
 * Threading
 *  - World thread: chat/addon hooks (CMSG_MESSAGECHAT is PROCESS_THREADUNSAFE),
 *    CMSG_ACCEPT_TRADE (PROCESS_THREADUNSAFE), WorldScript::OnUpdate (runs after
 *    MapMgr::Update has finished, so players can be read safely). All service
 *    calls happen here.
 *  - Any thread (map updates): login/logout/kill/quest/loot hooks and outgoing
 *    trade status packets only push small records into a mutex-protected inbox
 *    that Update() drains on the world thread.
 */

#ifndef MOD_ROMANCE_MANAGER_H
#define MOD_ROMANCE_MANAGER_H

#include "RomanceConfig.h"
#include "RomanceControl.h"
#include "RomanceProtocol.h"
#include "RomanceService.h"

#include "Define.h"
#include "ObjectGuid.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Item;
class Player;
class WorldSession;
class WorldPacket;

namespace romance::ac
{
    class RomanceSqlRepository;

    class RomanceManager
    {
    public:
        static RomanceManager& Instance();

        // lifecycle (world thread)
        void OnConfigLoad(bool reload);
        void OnStartup();
        void OnShutdown();
        void Update(uint32 diff);
        bool Enabled() const { return _enabled; }

        // chat & addon (world thread). Return false to swallow the original message.
        bool OnWhisper(Player* sender, Player* receiver, uint32 lang, std::string& msg);
        bool OnGroupChat(Player* sender, uint32 type, uint32 lang, std::string& msg);

        // inbox (any thread)
        void QueueLogin(ObjectGuid guid);
        void QueueLogout(ObjectGuid guid);
        void QueueKill(ObjectGuid killer, std::string const& creatureName, bool notable);
        void QueueQuestComplete(ObjectGuid player, std::string const& questTitle, uint32 questId = 0);
        void QueueQuestAccept(ObjectGuid player, uint32 questId);
        void QueueLoot(ObjectGuid player, std::string const& itemName);

        // trade detection
        void OnTradeAcceptPacket(WorldSession* session);              // world thread, before the handler runs
        void OnTradeStatusPacket(WorldSession* session, uint32 status); // any thread

        // .romance chat commands
        std::string CommandStatus(Player* player);
        std::string CommandSummon(Player* player, std::string const& name);
        std::string CommandDismiss(Player* player);
        std::string CommandToggleAi(Player* player, bool on);

    private:
        struct InboxEvent
        {
            enum class Kind : uint8 { Login, Logout, Kill, Quest, Loot, TradeDone, QuestAccept };
            Kind kind = Kind::Login;
            uint32 guid = 0;
            std::string text;
            bool flag = false;
            uint32 id = 0;
        };

        struct TradeItemSnapshot
        {
            GiftItem item;
            uint32 companionCountBefore = 0;
        };

        struct TradeSnapshot
        {
            uint32 companion = 0;
            std::vector<TradeItemSnapshot> items;
            uint32 money = 0;
            uint32 companionMoneyBefore = 0;
            bool completed = false;
            time_t at = 0;
        };

        struct Tracker
        {
            bool inCombat = false;
            bool nearDeath = false;
            uint32 kills = 0;
            std::string lastKill;
            uint32 zone = 0;
            uint8 ownerLevel = 0;
            uint8 companionLevel = 0;
            bool ownerAlive = true;
            bool companionAlive = true;
            bool initialized = false;
        };

        struct Peer
        {
            AddonReassembler reassembler;
            RateLimiter limiter{ 15.0, 60.0 };
            uint32 nextMessageId = 1;
            uint32 rejected = 0;
        };

        struct PendingSummon
        {
            uint32 owner = 0;
            time_t at = 0;
        };

        RomanceManager() = default;

        void Rebuild();
        time_t Now() const;
        int64 NowMs() const;

        // addon
        void OnAddonMessage(Player* player, std::string const& body);
        void HandleAddonOp(Player* player, Json const& msg);
        void SendAddon(Player* player, Json const& payload);
        void Reply(Player* player, Json const& req, Json payload);
        void ReplyError(Player* player, Json const& req, std::string const& code, std::string const& message = "");

        // ownership & characters
        bool ResolveOwnedCompanion(Player* owner, uint32 guidLow, std::string& name, std::string& error);
        Json BuildCharacterList(Player* owner);
        Json PresetListJson(uint32 account);
        CharacterStatus StatusFor(Player* owner, uint32 guidLow, std::string& reason);

        // companion lifecycle
        std::string Summon(Player* owner, uint32 guidLow);
        std::string Dismiss(Player* owner, uint32 guidLow, bool logout);
        bool ActivateCompanion(Player* owner, Player* bot, bool announce);
        void ProcessPendingLogins();
        // Activates an online bot of `owner` on demand (whisper / addon chat). Returns true if active afterwards.
        bool EnsureActive(Player* owner, Player* bot, bool requireConfigured);

        // world
        std::optional<WorldContext> BuildContext(CompanionKey const& key);
        void PollTrackers();
        void ProcessInbox();
        void ProcessTrades();
        void Execute(std::vector<OutgoingAction> const& actions);
        // Game knowledge + natural-language companion commands for one incoming message.
        void ApplyControl(Player* owner, Player* bot, CompanionKey const& key, std::string const& text, WorldContext& ctx);
        void PollControl();
        int64 EstrangedHours(Player* owner, uint32 guidLow);
        GiftItem DescribeItem(Item* item, Player* receiver) const;

        RomanceConfig _cfg;
        bool _enabled = false;
        bool _schemaOk = false;
        std::unique_ptr<RomanceSqlRepository> _repo;
        std::unique_ptr<RomanceService> _service;
        std::unique_ptr<CompanionControl> _control;
        uint32 _controlTimer = 0;
        std::map<CompanionKey, time_t> _pendingLeaves;   // estranged companions about to leave

        std::mutex _inboxMutex;
        std::vector<InboxEvent> _inbox;
        std::mutex _tradeMutex;
        std::map<uint32, TradeSnapshot> _trades;    // by owner guid

        std::map<uint32, Peer> _peers;              // by owner guid (world thread only)
        std::map<CompanionKey, Tracker> _trackers;
        std::map<uint32, PendingSummon> _pendingSummons; // by companion guid
        std::map<uint32, time_t> _loginWatch;            // bots that logged in but whose Playerbots AI may not be ready yet
        uint32 _tickTimer = 0;
        uint32 _pollTimer = 0;
    };
}

#define sRomanceMgr romance::ac::RomanceManager::Instance()

#endif
