/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceManager.h"
#include "RomanceAcConfig.h"
#include "RomancePlayerbots.h"
#include "RomancePresets.h"
#include "RomanceSqlRepository.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "TradeData.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

namespace romance::ac
{
    namespace
    {
        constexpr uint32 kTickIntervalMs = 1000;
        constexpr uint32 kPollIntervalMs = 500;
        constexpr uint32 kControlIntervalMs = 5000;
        constexpr time_t kLeaveDelaySec = 4;
        constexpr time_t kSummonTimeout = 60;
        constexpr time_t kTradeSnapshotTtl = 120;
        constexpr float kNearDeathPct = 25.0f;
        constexpr char const* kNoticePrefix = "|cffff99cc[Romance]|r ";

        ObjectGuid PlayerGuid(uint32 low) { return ObjectGuid::Create<HighGuid::Player>(low); }

        std::string ClassName(uint8 cls)
        {
            ChrClassesEntry const* e = sChrClassesStore.LookupEntry(cls);
            return e && e->name[0] ? e->name[0] : "adventurer";
        }

        std::string RaceName(uint8 race)
        {
            ChrRacesEntry const* e = sChrRacesStore.LookupEntry(race);
            return e && e->name[0] ? e->name[0] : "";
        }

        std::string AreaName(uint32 id)
        {
            AreaTableEntry const* e = sAreaTableStore.LookupEntry(id);
            if (!e)
                return "";
            char const* n = e->area_name[sWorld->GetDefaultDbcLocale()];
            if (!n || !*n)
                n = e->area_name[0];
            return n ? n : "";
        }

        std::string TimeOfDay(time_t now)
        {
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            int h = local.tm_hour;
            if (h >= 5 && h < 12) return "morning";
            if (h >= 12 && h < 18) return "afternoon";
            if (h >= 18 && h < 22) return "evening";
            return "night";
        }

        uint32 JsonGuid(Json const& msg)
        {
            int64_t g = msg.GetInt("guid", 0);
            return (g > 0 && g <= int64_t(0xFFFFFFFF)) ? uint32(g) : 0;
        }

        // Companions follow the server's faction rules: no Horde companion for an Alliance player
        // (and the other way round) unless AllowTwoSide.Interaction.Group is on.
        bool OtherFaction(Player* owner, uint8 race)
        {
            return !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) &&
                   Player::TeamIdForRace(race) != owner->GetTeamId(true);
        }

        // Player-facing text for error codes (shown by the addon and chat commands).
        std::string HumanError(std::string const& code, uint32 limit, int64 hoursLeft = -1)
        {
            if (code == "estranged")
                return hoursLeft > 0 ? "They left you and don't want to see you for about " + std::to_string(hoursLeft) + " more hours."
                                     : "They left you and don't want to see you right now.";
            if (code == "limit_reached")
                return "You already have the maximum number of AI companions (" + std::to_string(limit) + "). Send one home first.";
            if (code == "server_limit_reached") return "The server has reached its limit of AI companions. Try again later.";
            if (code == "online_as_player") return "This character is logged in by a player.";
            if (code == "controlled_elsewhere") return "This character is controlled by someone else.";
            if (code == "already_loading") return "This companion is already on the way.";
            if (code == "other_faction") return "This character belongs to the other faction. Companions have to be of your faction.";
            if (code == "not_owner") return "That character does not belong to you.";
            if (code == "playerbots_not_installed" || code == "playerbots_unavailable") return "mod-playerbots is not available on this server.";
            if (code == "companion_in_use") return "This companion is already active for another of your characters.";
            if (code == "module_disabled") return "The romance module is disabled.";
            if (code == "unknown_preset") return "That preset does not exist (anymore).";
            if (code == "not_online") return "This companion is not in the world right now. Call them first.";
            if (code == "import_incomplete") return "Not every part of the import arrived; nothing was changed. Try again.";
            if (code == "import_too_large") return "That import is too large.";
            if (code == "import_not_started" || code == "import_bad_part") return "The import was interrupted; nothing was changed. Try again.";
            if (code == "invalid_json") return "That is not a valid mod-romance export.";
            if (code == "preset_label_required") return "Give the preset a name first.";
            if (code == "preset_limit_reached") return "You have reached the maximum number of custom presets. Delete one first.";
            if (code == "activation_failed") return "The companion joined, but the AI could not be activated.";
            return code;
        }

        void Notice(Player* player, std::string const& text)
        {
            if (player && player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage(kNoticePrefix + text);
        }
    }

    RomanceManager& RomanceManager::Instance()
    {
        static RomanceManager instance;
        return instance;
    }

    time_t RomanceManager::Now() const { return std::time(nullptr); }

    int64 RomanceManager::NowMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ================================================================ lifecycle

    void RomanceManager::OnConfigLoad(bool reload)
    {
        _cfg = LoadConfigFromWorldserver();
        if (reload && _service)
        {
            _service->Reconfigure(std::make_shared<OllamaBackend>(_cfg.ollamaUrl));
            _enabled = _cfg.enabled && _schemaOk;
            LOG_INFO("module", "[mod-romance] configuration reloaded (model {}, {})", _cfg.model, _cfg.ollamaUrl);
        }
    }

    void RomanceManager::OnStartup()
    {
        _repo = std::make_unique<RomanceSqlRepository>();
        _schemaOk = _repo->CheckSchema();
        _enabled = _cfg.enabled && _schemaOk;
        Rebuild();
        _control->WarmUp();   // index quest givers now, not on the first Character tab click
        LOG_INFO("module", "[mod-romance] v{} {} | Ollama {} at {} | model {} | Playerbots {}", kModuleVersion,
                 _enabled ? "enabled" : "DISABLED", _cfg.ollamaEnabled ? "on" : "off", _cfg.ollamaUrl, _cfg.model,
                 PlayerbotsBridge::Available() ? "found" : "NOT found (summoning disabled)");
        if (std::string warn = PlayerbotsBridge::ConfigWarnings(); !warn.empty())
            LOG_WARN("module", "[mod-romance] {}", warn);
    }

    void RomanceManager::Rebuild()
    {
        if (_service)
            _service->Shutdown();
        _service = std::make_unique<RomanceService>(_cfg, *_repo, std::make_shared<OllamaBackend>(_cfg.ollamaUrl));
        _control = std::make_unique<CompanionControl>(_cfg);
    }

    void RomanceManager::OnShutdown()
    {
        if (_service)
        {
            _service->Shutdown();   // cancels in-flight HTTP requests, joins workers, persists sessions
            _service.reset();
        }
        LOG_INFO("module", "[mod-romance] shut down");
    }

    void RomanceManager::Update(uint32 diff)
    {
        if (!_enabled || !_service)
            return;
        _pollTimer += diff;
        _tickTimer += diff;
        _controlTimer += diff;
        ProcessInbox();
        ProcessPendingLogins();
        ProcessTrades();
        _control->Update(Now());
        for (auto it = _pendingLeaves.begin(); it != _pendingLeaves.end();)
        {
            if (Now() < it->second)
            {
                ++it;
                continue;
            }
            if (Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(it->first.owner))))
            {
                Dismiss(owner, uint32(it->first.companion), true);
                Json p = Json::MakeObject();
                p.Set("op", "chars_changed");
                SendAddon(owner, p);
            }
            else
                _service->Deactivate(it->first, Now());
            it = _pendingLeaves.erase(it);
        }
        if (_controlTimer >= kControlIntervalMs)
        {
            _controlTimer = 0;
            PollControl();
        }
        if (_pollTimer >= kPollIntervalMs)
        {
            _pollTimer = 0;
            PollTrackers();
        }
        if (_tickTimer >= kTickIntervalMs)
        {
            _tickTimer = 0;
            time_t now = Now();
            for (auto it = _pendingSummons.begin(); it != _pendingSummons.end();)
                it = now - it->second.at > kSummonTimeout ? _pendingSummons.erase(it) : std::next(it);
            _service->Tick(now, [this](CompanionKey const& k) { return BuildContext(k); });
        }
        Execute(_service->TakeOutgoing());
    }

    // ================================================================ inbox

    void RomanceManager::QueueLogin(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> g(_inboxMutex);
        _inbox.push_back({ InboxEvent::Kind::Login, guid.GetCounter(), "", false });
    }

    void RomanceManager::QueueLogout(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> g(_inboxMutex);
        _inbox.push_back({ InboxEvent::Kind::Logout, guid.GetCounter(), "", false });
    }

    void RomanceManager::QueueKill(ObjectGuid killer, std::string const& creatureName, bool notable)
    {
        std::lock_guard<std::mutex> g(_inboxMutex);
        _inbox.push_back({ InboxEvent::Kind::Kill, killer.GetCounter(), creatureName, notable });
    }

    void RomanceManager::QueueQuestComplete(ObjectGuid player, std::string const& questTitle, uint32 questId)
    {
        std::lock_guard<std::mutex> g(_inboxMutex);
        _inbox.push_back({ InboxEvent::Kind::Quest, player.GetCounter(), questTitle, false, questId });
    }

    void RomanceManager::QueueQuestAccept(ObjectGuid player, uint32 questId)
    {
        std::lock_guard<std::mutex> g(_inboxMutex);
        _inbox.push_back({ InboxEvent::Kind::QuestAccept, player.GetCounter(), "", false, questId });
    }

    void RomanceManager::QueueLoot(ObjectGuid player, std::string const& itemName)
    {
        std::lock_guard<std::mutex> g(_inboxMutex);
        _inbox.push_back({ InboxEvent::Kind::Loot, player.GetCounter(), itemName, false });
    }

    void RomanceManager::ProcessInbox()
    {
        std::vector<InboxEvent> events;
        {
            std::lock_guard<std::mutex> g(_inboxMutex);
            events.swap(_inbox);
        }
        time_t now = Now();
        for (auto const& e : events)
        {
            switch (e.kind)
            {
                case InboxEvent::Kind::Login:
                    // Playerbots fires OnPlayerLogin before it attaches the bot's AI and master
                    // (OnBotLoginOperation is queued), so readiness is polled in ProcessPendingLogins().
                    _loginWatch[e.guid] = now;
                    break;
                case InboxEvent::Kind::Logout:
                {
                    for (auto const& k : _service->ActiveForOwner(e.guid))
                    {
                        _service->Deactivate(k, now);
                        _trackers.erase(k);
                    }
                    if (auto k = _service->ActiveByCompanion(e.guid))
                    {
                        _service->Deactivate(*k, now);
                        _trackers.erase(*k);
                        if (Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(k->owner))))
                        {
                            Json p = Json::MakeObject();
                            p.Set("op", "chars_changed");
                            SendAddon(owner, p);
                        }
                    }
                    _peers.erase(e.guid);
                    {
                        std::lock_guard<std::mutex> g(_tradeMutex);
                        _trades.erase(e.guid);
                    }
                    break;
                }
                case InboxEvent::Kind::Kill:
                {
                    for (auto& [key, tr] : _trackers)
                        if (key.owner == e.guid || key.companion == e.guid)
                        {
                            ++tr.kills;
                            if (e.flag || tr.lastKill.empty())
                                tr.lastKill = e.text;
                        }
                    break;
                }
                case InboxEvent::Kind::Quest:
                    for (auto const& k : _service->ActiveForOwner(e.guid))
                    {
                        // turn it in too when she is done with it and the quest ender is near
                        if (e.id)
                        {
                            Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(e.guid));
                            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(k.companion)));
                            if (owner && bot)
                                _control->SyncQuestReward(owner, bot, e.id);
                        }
                        _service->OnGameEvent(k, { GameEventType::QuestComplete, e.text, 0, now }, now);
                    }
                    break;
                case InboxEvent::Kind::QuestAccept:
                    for (auto const& k : _service->ActiveForOwner(e.guid))
                    {
                        Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(e.guid));
                        Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(k.companion)));
                        if (!owner || !bot)
                            continue;
                        std::string note = _control->SyncQuestAccept(owner, bot, e.id);
                        if (!note.empty())
                            _service->OnGameEvent(k, { GameEventType::CompanionNotice, note, 1, now }, now);
                    }
                    break;
                case InboxEvent::Kind::Loot:
                    for (auto const& k : _service->ActiveForOwner(e.guid))
                        _service->OnGameEvent(k, { GameEventType::NotableLoot, e.text, 0, now }, now);
                    break;
                case InboxEvent::Kind::TradeDone:
                    break; // handled through _trades
            }
        }
    }

    void RomanceManager::ProcessPendingLogins()
    {
        time_t now = Now();
        for (auto it = _loginWatch.begin(); it != _loginWatch.end();)
        {
            uint32 guid = it->first;
            Player* p = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guid));
            if (!p || now - it->second > kSummonTimeout)
            {
                it = _loginWatch.erase(it);
                continue;
            }
            if (!p->GetSession() || p->GetSession()->PlayerLoading() || !p->IsInWorld())
            {
                ++it;
                continue;
            }
            if (!PlayerbotsBridge::IsBot(p))
            {
                // a real player, or a bot whose AI is not attached yet
                if (now - it->second > 5 && !_pendingSummons.count(guid))
                    it = _loginWatch.erase(it);
                else
                    ++it;
                continue;
            }
            Player* master = PlayerbotsBridge::MasterOf(p);
            if (!master || !PlayerbotsBridge::IsMyBot(p, master))
            {
                ++it;   // master not attached yet (or a random bot: expires via the timeout)
                continue;
            }
            auto pending = _pendingSummons.find(guid);
            bool requested = pending != _pendingSummons.end() && master->GetGUID().GetCounter() == pending->second.owner;
            if (pending != _pendingSummons.end())
                _pendingSummons.erase(pending);
            // Companions added through ".playerbots bot add" also come alive if the owner configured them.
            CompanionRecord rec = _service->GetOrCreateRecord({ master->GetGUID().GetCounter(), guid },
                                                              master->GetSession()->GetAccountId(), p->GetName(), now);
            if (requested || (rec.configured && rec.settings.aiEnabled))
                ActivateCompanion(master, p, true);
            it = _loginWatch.erase(it);
        }
    }

    bool RomanceManager::EnsureActive(Player* owner, Player* bot, bool requireConfigured)
    {
        if (!owner || !bot)
            return false;
        CompanionKey key{ owner->GetGUID().GetCounter(), bot->GetGUID().GetCounter() };
        if (_service->IsActive(key))
            return true;
        if (!PlayerbotsBridge::IsMyBot(bot, owner))
            return false;
        CompanionRecord rec = _service->GetOrCreateRecord(key, owner->GetSession()->GetAccountId(), bot->GetName(), Now());
        if (!rec.settings.aiEnabled || (requireConfigured && !rec.configured))
            return false;
        _loginWatch.erase(key.companion);
        _pendingSummons.erase(uint32(key.companion));
        return ActivateCompanion(owner, bot, false);
    }

    // ================================================================ trades

    GiftItem RomanceManager::DescribeItem(Item* item, Player* receiver) const
    {
        GiftItem g;
        ItemTemplate const* proto = item->GetTemplate();
        g.itemGuid = item->GetGUID().GetCounter();
        g.entry = item->GetEntry();
        g.count = item->GetCount();
        if (!proto)
            return g;
        g.name = proto->Name1;
        g.quality = int(proto->Quality);
        g.itemClass = int(proto->Class);
        g.itemSubClass = int(proto->SubClass);
        g.inventoryType = int(proto->InventoryType);
        g.sellPrice = proto->SellPrice;
        g.itemLevel = int(proto->ItemLevel);
        g.requiredLevel = int(proto->RequiredLevel);
        g.isEquippable = proto->InventoryType != INVTYPE_NON_EQUIP;
        if (receiver && g.isEquippable)
        {
            g.usableByCompanion = receiver->CanUseItem(proto) == EQUIP_ERR_OK;
            uint8 slot = receiver->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot < EQUIPMENT_SLOT_END)
            {
                Item* equipped = receiver->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                g.equippedItemLevel = equipped && equipped->GetTemplate() ? int(equipped->GetTemplate()->ItemLevel) : -1;
            }
        }
        return g;
    }

    void RomanceManager::OnTradeAcceptPacket(WorldSession* session)
    {
        if (!_enabled || !_service || !session)
            return;
        Player* owner = session->GetPlayer();
        if (!owner)
            return;
        TradeData* mine = owner->GetTradeData();
        Player* trader = mine ? mine->GetTrader() : nullptr;
        if (!trader)
            return;
        CompanionKey key{ owner->GetGUID().GetCounter(), trader->GetGUID().GetCounter() };
        if (!_service->IsActive(key))
            return;

        TradeSnapshot snap;
        snap.companion = uint32(key.companion);
        snap.money = mine->GetMoney();
        snap.companionMoneyBefore = trader->GetMoney();
        snap.at = Now();
        for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
            if (Item* item = mine->GetItem(TradeSlots(slot)))
            {
                TradeItemSnapshot s;
                s.item = DescribeItem(item, trader);
                s.companionCountBefore = trader->GetItemCount(item->GetEntry(), true);
                snap.items.push_back(s);
            }
        std::lock_guard<std::mutex> g(_tradeMutex);
        _trades[uint32(key.owner)] = std::move(snap);   // re-accepting replaces an older snapshot
    }

    void RomanceManager::OnTradeStatusPacket(WorldSession* session, uint32 status)
    {
        if (!session)
            return;
        Player* owner = session->GetPlayer();
        if (!owner)
            return;
        uint32 ownerGuid = owner->GetGUID().GetCounter();
        std::lock_guard<std::mutex> g(_tradeMutex);
        auto it = _trades.find(ownerGuid);
        if (it == _trades.end())
            return;
        // TRADE_STATUS_TRADE_COMPLETE is sent only after WorldSession::moveItems() has run.
        if (status == TRADE_STATUS_TRADE_COMPLETE)
            it->second.completed = true;
        else if (status == TRADE_STATUS_TRADE_CANCELED || status == TRADE_STATUS_CLOSE_WINDOW)
            _trades.erase(it);
    }

    void RomanceManager::ProcessTrades()
    {
        std::vector<std::pair<uint32, TradeSnapshot>> done;
        {
            std::lock_guard<std::mutex> g(_tradeMutex);
            time_t now = Now();
            for (auto it = _trades.begin(); it != _trades.end();)
            {
                if (it->second.completed)
                {
                    done.emplace_back(it->first, std::move(it->second));
                    it = _trades.erase(it);
                }
                else if (now - it->second.at > kTradeSnapshotTtl)
                    it = _trades.erase(it);
                else
                    ++it;
            }
        }
        for (auto& [ownerGuid, snap] : done)
        {
            Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(ownerGuid));
            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(snap.companion));
            CompanionKey key{ ownerGuid, snap.companion };
            if (!owner || !bot || !_service->IsActive(key))
                continue;
            auto ctx = BuildContext(key).value_or(WorldContext{});
            time_t now = Now();
            for (auto const& s : snap.items)
            {
                // Verify the item really is in the companion's possession now.
                bool verified = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(uint32(s.item.itemGuid))) != nullptr ||
                                bot->GetItemCount(s.item.entry, true) >= s.companionCountBefore + s.item.count;
                if (!verified)
                {
                    LOG_DEBUG("module", "[mod-romance] trade item {} not found on {} after trade; no reward", s.item.entry, bot->GetName());
                    continue;
                }
                auto outcome = _service->OnGift(key, s.item, ctx, now);
                if (_cfg.debugLogging)
                    LOG_INFO("module", "[mod-romance] gift {} -> {}: {} ({})", s.item.name, bot->GetName(),
                             outcome.rewarded ? "rewarded" : "not rewarded", outcome.reason);
            }
            if (snap.money >= 10000 && bot->GetMoney() >= snap.companionMoneyBefore + snap.money)
            {
                GiftItem gold;
                gold.money = snap.money;
                gold.name = "gold";
                _service->OnGift(key, gold, ctx, now);
            }
        }
    }

    // ================================================================ world snapshots

    std::optional<WorldContext> RomanceManager::BuildContext(CompanionKey const& key)
    {
        Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.owner)));
        Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.companion)));
        if (!owner || !owner->IsInWorld())
            return std::nullopt;
        WorldContext c;
        c.playerName = owner->GetName();
        c.playerClass = ClassName(owner->getClass());
        c.playerRace = RaceName(owner->getRace());
        c.playerLevel = owner->GetLevel();
        c.playerHealthPct = int(owner->GetHealthPct());
        c.playerInCombat = owner->IsInCombat();
        c.playerAfk = owner->isAFK();
        c.zone = AreaName(owner->GetZoneId());
        c.area = AreaName(owner->GetAreaId());
        if (Map* map = owner->GetMap())
        {
            c.inDungeon = map->IsDungeon() && !map->IsRaid();
            c.inRaid = map->IsRaid();
            c.inBattleground = map->IsBattlegroundOrArena();
        }
        c.timeOfDay = TimeOfDay(Now());
        if (bot && bot->IsInWorld())
        {
            c.companionInWorld = true;
            c.companionClass = ClassName(bot->getClass());
            c.companionRace = RaceName(bot->getRace());
            c.companionGender = bot->getGender() == GENDER_FEMALE ? "female" : "male";
            c.companionLevel = bot->GetLevel();
            c.companionHealthPct = int(bot->GetHealthPct());
            c.companionManaPct = bot->getPowerType() == POWER_MANA ? int(bot->GetPowerPct(POWER_MANA)) : -1;
            c.companionInCombat = bot->IsInCombat();
            Group* group = owner->GetGroup();
            c.sameGroup = group && group->IsMember(bot->GetGUID());
            c.partySize = group ? int(group->GetMembersCount()) : 1;
            c.followState = _control && _control->IsLeading(bot->GetGUID()) ? std::string("lead") : PlayerbotsBridge::FollowState(bot);
            // other AI companions in the same group (so she knows who else may answer)
            if (Group* g = owner->GetGroup())
                for (CompanionKey const& other : _service->ActiveForOwner(key.owner))
                    if (other.companion != key.companion && g->IsMember(PlayerGuid(uint32(other.companion))))
                    {
                        // name plus who they are, so she doesn't guess their gender
                        auto [gender, pronouns] = _service->GenderOf(other);
                        Player* ob = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(other.companion)));
                        if (gender.empty() && ob)
                            gender = ob->getGender() == GENDER_FEMALE ? "female" : "male";
                        std::string who = gender;
                        if (ob)
                            who += (who.empty() ? "" : " ") + RaceName(ob->getRace()) + " " + ClassName(ob->getClass());
                        if (!pronouns.empty())
                            who += ", " + pronouns;
                        c.partyCompanions.push_back(_service->DisplayName(other) + (who.empty() ? "" : " (" + who + ")"));
                    }
            uint32 total = 0, count = 0;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (it->GetTemplate() && slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD)
                    {
                        total += it->GetTemplate()->ItemLevel;
                        ++count;
                    }
            if (count)
                c.equipmentSummary = "average item level " + std::to_string(total / count);
        }
        return c;
    }

    void RomanceManager::PollTrackers()
    {
        time_t now = Now();
        for (CompanionKey const& key : _service->ActiveKeys())
        {
            Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.owner)));
            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.companion)));
            if (!owner || !bot || !owner->IsInWorld() || !bot->IsInWorld())
                continue;
            Tracker& t = _trackers[key];
            if (!t.initialized)
            {
                t.initialized = true;
                t.zone = owner->GetZoneId();
                t.ownerLevel = owner->GetLevel();
                t.companionLevel = bot->GetLevel();
                t.ownerAlive = owner->IsAlive();
                t.companionAlive = bot->IsAlive();
                continue;
            }
            bool combat = owner->IsInCombat() || bot->IsInCombat();
            if (combat && !t.inCombat)
            {
                t.inCombat = true;
                t.nearDeath = false;
                t.kills = 0;
                t.lastKill.clear();
                _service->OnGameEvent(key, { GameEventType::CombatStart, "", 0, now }, now);
            }
            if (combat && !t.nearDeath && owner->IsAlive() && owner->GetHealthPct() < kNearDeathPct)
            {
                t.nearDeath = true;
                _service->OnGameEvent(key, { GameEventType::PlayerNearDeath, "", int(owner->GetHealthPct()), now }, now);
            }
            if (!combat && t.inCombat)
            {
                t.inCombat = false;
                _service->OnGameEvent(key, { GameEventType::CombatEnd, t.lastKill, int(t.kills), now }, now);
            }
            if (t.ownerAlive && !owner->IsAlive())
                _service->OnGameEvent(key, { GameEventType::PlayerDied, "", 0, now }, now);
            if (t.companionAlive && !bot->IsAlive())
                _service->OnGameEvent(key, { GameEventType::CompanionDied, "", 0, now }, now);
            t.ownerAlive = owner->IsAlive();
            t.companionAlive = bot->IsAlive();
            if (bot->GetLevel() > t.companionLevel)
                _service->OnGameEvent(key, { GameEventType::CompanionLevelUp, "", int(bot->GetLevel()), now }, now);
            if (owner->GetLevel() > t.ownerLevel)
                _service->OnGameEvent(key, { GameEventType::PlayerLevelUp, "", int(owner->GetLevel()), now }, now);
            t.companionLevel = bot->GetLevel();
            t.ownerLevel = owner->GetLevel();
            if (owner->GetZoneId() != t.zone)
            {
                t.zone = owner->GetZoneId();
                _service->OnGameEvent(key, { GameEventType::ZoneChanged, AreaName(t.zone), 0, now }, now);
            }
        }
    }

    // ================================================================ executing service output

    void RomanceManager::Execute(std::vector<OutgoingAction> const& actions)
    {
        for (auto const& a : actions)
        {
            Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(a.key.owner)));
            if (!owner || !owner->IsInWorld())
                continue;
            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(a.key.companion)));
            switch (a.type)
            {
                case OutgoingAction::Type::Speak:
                {
                    if (!bot || !bot->IsInWorld())
                        break;
                    bool party = a.channel == ChatChannel::Party && owner->GetGroup() && owner->GetGroup()->IsMember(bot->GetGUID());
                    for (auto const& piece : SplitForChat(a.text, 240))
                    {
                        if (party && PlayerbotsBridge::SayToParty(bot, piece))
                            continue;
                        bot->Whisper(piece, LANG_UNIVERSAL, owner);
                    }
                    break;
                }
                case OutgoingAction::Type::Emote:
                {
                    if (!bot || !bot->IsInWorld() || bot->IsInCombat())
                        break;
                    if (bot->GetMapId() == owner->GetMapId() && bot->GetDistance(owner) < 30.0f)
                        bot->SetFacingToObject(owner);
                    std::string target = owner->GetName();
                    switch (a.action)
                    {
                        case ActionType::EmoteHug: bot->TextEmote("hugs " + target + "."); break;
                        case ActionType::EmoteKiss: bot->HandleEmoteCommand(EMOTE_ONESHOT_KISS); bot->TextEmote("kisses " + target + "."); break;
                        case ActionType::EmoteWave: bot->HandleEmoteCommand(EMOTE_ONESHOT_WAVE); bot->TextEmote("waves at " + target + "."); break;
                        case ActionType::EmoteLaugh: bot->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH); break;
                        case ActionType::EmoteBlush: bot->HandleEmoteCommand(EMOTE_ONESHOT_SHY); bot->TextEmote("blushes."); break;
                        case ActionType::EmoteDance: bot->HandleEmoteCommand(EMOTE_ONESHOT_DANCE); break;
                        case ActionType::EmoteCheer: bot->HandleEmoteCommand(EMOTE_ONESHOT_CHEER); break;
                        case ActionType::EmoteBow: bot->HandleEmoteCommand(EMOTE_ONESHOT_BOW); break;
                        case ActionType::EmoteCry: bot->HandleEmoteCommand(EMOTE_ONESHOT_CRY); break;
                        default: break;
                    }
                    break;
                }
                case OutgoingAction::Type::Command:
                {
                    if (!bot)
                        break;
                    char const* cmd = nullptr;
                    switch (a.action)
                    {
                        case ActionType::Follow: cmd = "follow"; break;
                        case ActionType::Stay: cmd = "stay"; break;
                        case ActionType::Attack: cmd = "attack"; break;
                        case ActionType::Flee: cmd = "flee"; break;
                        default: break;
                    }
                    if (cmd)
                        PlayerbotsBridge::Command(bot, owner, cmd);
                    if (a.action == ActionType::Leave)
                        _pendingLeaves[a.key] = Now() + kLeaveDelaySec;   // let the farewell arrive first
                    break;
                }
                case OutgoingAction::Type::UiPush:
                    SendAddon(owner, a.payload);
                    break;
                case OutgoingAction::Type::Notice:
                    Notice(owner, a.text);
                    break;
            }
        }
    }

    // ================================================================ chat hooks

    bool RomanceManager::OnWhisper(Player* sender, Player* receiver, uint32 lang, std::string& msg)
    {
        if (!_enabled || !_service || !sender || !receiver)
            return true;

        if (lang == LANG_ADDON)
        {
            std::string const prefix = std::string(kAddonPrefix) + "\t";
            if (sender == receiver && msg.compare(0, prefix.size(), prefix) == 0)
            {
                OnAddonMessage(sender, msg.substr(prefix.size()));
                return false;   // swallow: our protocol traffic is never relayed
            }
            return true;
        }

        if (sender == receiver || PlayerbotsBridge::IsBot(sender))
            return true;        // companions talking, bot-to-bot: never trigger the AI (no loops)

        CompanionKey key{ sender->GetGUID().GetCounter(), receiver->GetGUID().GetCounter() };
        // A configured companion that is online as the sender's bot but not active yet is activated on demand.
        if (!_service->IsActive(key) && !EnsureActive(sender, receiver, true))
            return true;        // not a romance companion: Playerbots handles it as usual

        auto res = TalkTo(sender, receiver, key, msg, ChatChannel::Whisper, false);
        switch (res.result)
        {
            case RomanceService::MessageResult::NotForUs:
            case RomanceService::MessageResult::AiDisabled:
                ExpectBotFeedback(sender, receiver);
                return true;
            case RomanceService::MessageResult::PassToPlayerbots:
                ExpectBotFeedback(sender, receiver);
                msg = res.rewritten;
                return true;
            case RomanceService::MessageResult::Busy:
                Notice(sender, receiver->GetName() + " is still answering your last message.");
                return false;
            case RomanceService::MessageResult::Accepted:
            {
                // Show the player their own whisper (Player::Whisper would have done this), but keep the
                // conversational text away from the Playerbots command parser.
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_INFORM, LANG_UNIVERSAL, receiver, receiver, msg);
                sender->SendDirectMessage(&data);
                Execute(_service->TakeOutgoing());
                return false;
            }
        }
        return true;
    }

    RomanceService::HandleResult RomanceManager::TalkTo(Player* owner, Player* bot, CompanionKey const& key, std::string const& text,
                                                        ChatChannel channel, bool partyTarget)
    {
        // The one path for whisper, party and addon chat: game knowledge and companion commands first,
        // then the AI; plain movement words ("follow me") go to Playerbots once the line is accepted.
        auto ctx = BuildContext(key).value_or(WorldContext{});
        if (bot)
            ApplyControl(owner, bot, key, text, ctx);
        auto res = _service->OnPlayerMessage(key, text, channel, ctx, Now(), partyTarget);
        if (res.result == RomanceService::MessageResult::Accepted && res.command && bot)
        {
            static char const* const cmds[] = { "", "follow", "stay", "attack", "flee" };
            int idx = int(*res.command);
            if (idx >= 1 && idx <= 4)
                PlayerbotsBridge::Command(bot, owner, cmds[idx]);
        }
        return res;
    }

    void RomanceManager::ApplyControl(Player* owner, Player* bot, CompanionKey const& key, std::string const& text, WorldContext& ctx)
    {
        if (!_cfg.controlEnabled || !owner || !bot)
            return;
        RomanceService::ControlRequest req = _service->PrepareControl(key, text, bot->GetName());
        if (!req.active)
            return;
        // Knowledge questions get real data even with natural commands off ("what's in your bags?").
        _control->AddKnowledge(ctx, owner, bot, req.knowledgeTopics);
        if (auto const& cmd = req.command)
        {
            ctx.commandOutcome = _control->Execute(owner, bot, *cmd, _service->StageOf(key), Now());
            ctx.commandHandled = !ctx.commandOutcome.empty();
            if (_cfg.debugLogging)
                LOG_INFO("module", "[mod-romance] {} -> {}: command {} '{}' => {}", owner->GetName(), bot->GetName(),
                         CompanionCommandId(cmd->kind), cmd->arg, ctx.commandOutcome);
        }
    }

    int64 RomanceManager::EstrangedHours(Player* owner, uint32 guidLow)
    {
        if (!owner || !guidLow)
            return -1;
        CompanionRecord rec = _service->GetOrCreateRecord({ owner->GetGUID().GetCounter(), guidLow },
                                                          owner->GetSession()->GetAccountId(), "", Now());
        if (!RelationshipEngine::Away(rec.relationship, Now()))
            return -1;
        return std::max<int64>(1, (rec.relationship.estrangedUntil - Now() + 3599) / 3600);
    }

    void RomanceManager::PollControl()
    {
        time_t now = Now();
        for (CompanionKey const& key : _service->ActiveKeys())
        {
            Player* owner = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.owner)));
            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.companion)));
            if (!owner || !bot || !owner->IsInWorld() || !bot->IsInWorld())
                continue;
            std::string notice = _control->Poll(owner, bot, now);
            if (!notice.empty())
                _service->OnGameEvent(key, { GameEventType::CompanionNotice, notice, 2, now }, now);
        }
        for (auto const& [botGuid, text] : _control->TakeNotices())
            for (CompanionKey const& key : _service->ActiveKeys())
                if (key.companion == botGuid.GetCounter())
                    _service->OnGameEvent(key, { GameEventType::CompanionNotice, text, 2, now }, now);
    }

    bool RomanceManager::OnGroupChat(Player* sender, uint32 type, uint32 lang, std::string& msg)
    {
        if (!_enabled || !_service || !sender || lang == LANG_ADDON)
            return true;
        if (type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER && type != CHAT_MSG_RAID && type != CHAT_MSG_RAID_LEADER)
            return true;
        if (PlayerbotsBridge::IsBot(sender))
            return true;
        Group* group = sender->GetGroup();
        if (!group)
            return true;

        // Companions of the sender that are in this group.
        std::vector<CompanionKey> inGroup;
        for (CompanionKey const& key : _service->ActiveForOwner(sender->GetGUID().GetCounter()))
            if (group->IsMember(PlayerGuid(uint32(key.companion))))
                inGroup.push_back(key);
        if (inGroup.empty())
            return true;

        // Who answers: the companions named in the line; everyone for "everyone, ..." / "guys, ...";
        // without a name, every companion (or the one talked to last, Romance.Party.UnaddressedReplies = last)
        // - but only if no other real player is in the group (no AI replies to everyday group chatter).
        std::vector<CompanionKey> addressed;
        bool everyone = AddressesEveryone(msg);
        for (auto const& k : inGroup)
            if (everyone || _service->IsAddressed(k, msg))
                addressed.push_back(k);
        bool otherHumans = false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* m = ref->GetSource())
                if (m != sender && !PlayerbotsBridge::IsBot(m))
                    otherHumans = true;
        bool picked = everyone;
        if (addressed.empty() && !otherHumans)
        {
            picked = true;
            if (_cfg.partyAllAnswer)
                addressed = inGroup;
            else
            {
                CompanionKey last = inGroup.front();
                for (auto const& k : inGroup)
                    if (_service->LastActivity(k) > _service->LastActivity(last))
                        last = k;
                addressed.push_back(last);
            }
        }

        std::vector<CompanionKey> answering;   // replying with the AI, or still busy with an AI reply
        for (CompanionKey const& key : addressed)
        {
            auto result = TalkTo(sender, ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.companion))), key, msg,
                                 ChatChannel::Party, picked).result;
            if (result == RomanceService::MessageResult::Accepted || result == RomanceService::MessageResult::Busy)
                answering.push_back(key);
        }
        // Playerbots also reads party lines as commands. Its answers are only wanted from companions
        // that won't reply with the AI to this line.
        for (CompanionKey const& key : inGroup)
            if (std::find(answering.begin(), answering.end(), key) == answering.end())
                ExpectBotFeedback(sender, ObjectAccessor::FindConnectedPlayer(PlayerGuid(uint32(key.companion))));
        Execute(_service->TakeOutgoing());
        return true;            // party chat is always shown to the party
    }

    // ================================================================ addon protocol

    void RomanceManager::SendAddon(Player* player, Json const& payload)
    {
        if (!player || !player->GetSession())
            return;
        Peer& peer = _peers[player->GetGUID().GetCounter()];
        for (auto const& chunk : EncodeAddonMessage(payload, peer.nextMessageId++))
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(kAddonPrefix) + "\t" + chunk);
            player->SendDirectMessage(&data);
        }
        if (peer.nextMessageId > 2000000000)
            peer.nextMessageId = 1;
    }

    void RomanceManager::ExpectBotFeedback(Player* owner, Player* bot)
    {
        // A line that goes to Playerbots without an AI reply: its answer ("Following", stats, ...) is
        // the only response, so the addon shows it instead of hiding it as feedback noise.
        if (!owner || !bot)
            return;
        Json p = Json::MakeObject();
        p.Set("op", "bot_feedback");
        p.Set("guid", int64_t(bot->GetGUID().GetCounter()));
        p.Set("name", bot->GetName());
        SendAddon(owner, p);
    }

    void RomanceManager::Reply(Player* player, Json const& req, Json payload)
    {
        if (req.Has("req"))
            payload.Set("req", req["req"]);
        SendAddon(player, payload);
    }

    void RomanceManager::ReplyError(Player* player, Json const& req, std::string const& code, std::string const& message)
    {
        Json p = Json::MakeObject();
        p.Set("op", "error");
        p.Set("code", code);
        p.Set("message", message);
        p.Set("for", req.GetString("op"));
        Reply(player, req, p);
    }

    void RomanceManager::OnAddonMessage(Player* player, std::string const& body)
    {
        Peer& peer = _peers[player->GetGUID().GetCounter()];
        if (body.size() > 250 || !peer.limiter.Allow(NowMs()))
        {
            if (++peer.rejected % 50 == 1)
                LOG_WARN("module", "[mod-romance] rate limit / oversize addon traffic from {} ({} rejected)", player->GetName(), peer.rejected);
            return;
        }
        auto r = peer.reassembler.Feed(body, NowMs());
        if (r.status == AddonReassembler::Status::Incomplete)
            return;
        if (r.status != AddonReassembler::Status::Complete)
        {
            if (_cfg.debugLogging)
                LOG_INFO("module", "[mod-romance] rejected addon message from {}: {}", player->GetName(), r.error);
            Json err = Json::MakeObject();
            err.Set("op", "error");
            err.Set("code", "protocol");
            err.Set("message", r.error);
            SendAddon(player, err);
            return;
        }
        // Each complete operation costs extra tokens, limiting expensive ops even with small payloads.
        if (!peer.limiter.Allow(NowMs(), 2.0))
        {
            ReplyError(player, r.message, "rate_limited", "Too many requests, slow down.");
            return;
        }
        HandleAddonOp(player, r.message);
        Execute(_service->TakeOutgoing());
    }

    bool RomanceManager::ResolveOwnedCompanion(Player* owner, uint32 guidLow, std::string& name, std::string& error)
    {
        if (!guidLow || guidLow == owner->GetGUID().GetCounter())
        {
            error = "invalid_character";
            return false;
        }
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(PlayerGuid(guidLow));
        if (!entry)
        {
            error = "unknown_character";
            return false;
        }
        uint32 myAccount = owner->GetSession()->GetAccountId();
        bool owned = entry->AccountId == myAccount ||
                     (_cfg.allowLinkedAccounts && PlayerbotsBridge::IsAccountLinked(owner, entry->AccountId));
        if (!owned)
        {
            // Never reveal anything about characters that are not the player's own.
            error = "not_owner";
            return false;
        }
        name = entry->Name;
        return true;
    }

    CharacterStatus RomanceManager::StatusFor(Player* owner, uint32 guidLow, std::string& reason)
    {
        CompanionKey key{ owner->GetGUID().GetCounter(), guidLow };
        if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(PlayerGuid(guidLow));
            entry && OtherFaction(owner, entry->Race))
        {
            reason = std::string("Other faction (") + (Player::TeamIdForRace(entry->Race) == TEAM_ALLIANCE ? "Alliance" : "Horde") + ").";
            return CharacterStatus::Unavailable;
        }
        if (Player* p = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guidLow)))
        {
            if (!PlayerbotsBridge::IsBot(p))
            {
                reason = "This character is logged in by a player.";
                return CharacterStatus::OnlineAsPlayer;
            }
            if (!PlayerbotsBridge::IsMyBot(p, owner))
            {
                reason = "This character is controlled by someone else.";
                return CharacterStatus::ControlledElsewhere;
            }
            bool inParty = owner->GetGroup() && owner->GetGroup()->IsMember(p->GetGUID());
            if (!_service->IsActive(key))
            {
                CompanionRecord rec = _service->GetOrCreateRecord(key, owner->GetSession()->GetAccountId(), p->GetName(), Now());
                if (!rec.settings.aiEnabled)
                {
                    reason = "AI companion is disabled for this character.";
                    return CharacterStatus::AiDisabled;
                }
            }
            reason = inParty ? "In your party." : "Summoned as your bot.";
            return inParty ? CharacterStatus::InParty : CharacterStatus::Summoned;
        }
        if (auto other = _service->ActiveByCompanion(guidLow); other && other->owner != key.owner)
        {
            reason = "Already a companion of another of your characters.";
            return CharacterStatus::ControlledElsewhere;
        }
        if (!PlayerbotsBridge::Available())
        {
            reason = "mod-playerbots is not installed on this server.";
            return CharacterStatus::Unavailable;
        }
        if (_pendingSummons.count(guidLow))
        {
            reason = "Logging in...";
            return CharacterStatus::Summoned;
        }
        reason = "Offline - ready to be called.";
        return CharacterStatus::Available;
    }

    Json RomanceManager::PresetListJson(uint32 account)
    {
        Json presets = Json::MakeArray();
        for (auto const& pr : _service->ListPresets(account))
            presets.Push(pr.ToJson());
        return presets;
    }

    Json RomanceManager::BuildCharacterList(Player* owner)
    {
        uint32 myAccount = owner->GetSession()->GetAccountId();
        std::vector<uint32> accounts{ myAccount };
        if (_cfg.allowLinkedAccounts)
            for (uint32 a : PlayerbotsBridge::LinkedAccounts(myAccount))
                if (a != myAccount && PlayerbotsBridge::IsAccountLinked(owner, a))
                    accounts.push_back(a);
        std::ostringstream list;
        for (size_t i = 0; i < accounts.size(); ++i)
            list << (i ? "," : "") << accounts[i];

        Json chars = Json::MakeArray();
        QueryResult r = CharacterDatabase.Query(
            "SELECT guid, name, race, class, gender, level, account FROM characters WHERE account IN ({}) ORDER BY level DESC, name LIMIT 60",
            list.str());
        std::map<uint32, CompanionRecord> configured;
        for (auto const& rec : _service->ConfiguredCompanions(owner->GetGUID().GetCounter()))
            configured[uint32(rec.key.companion)] = rec;
        if (r)
        {
            do
            {
                Field* f = r->Fetch();
                uint32 guid = f[0].Get<uint32>();
                if (guid == owner->GetGUID().GetCounter())
                    continue;
                std::string reason;
                CharacterStatus st = StatusFor(owner, guid, reason);
                Json c = Json::MakeObject();
                c.Set("guid", int64_t(guid));
                c.Set("name", f[1].Get<std::string>());
                c.Set("race", int(f[2].Get<uint8>()));
                c.Set("raceName", RaceName(f[2].Get<uint8>()));
                c.Set("class", int(f[3].Get<uint8>()));
                c.Set("className", ClassName(f[3].Get<uint8>()));
                c.Set("gender", int(f[4].Get<uint8>()));
                c.Set("level", int(f[5].Get<uint8>()));
                c.Set("linked", f[6].Get<uint32>() != myAccount);
                c.Set("status", CharacterStatusId(st));
                c.Set("reason", reason);
                c.Set("active", _service->IsActive({ owner->GetGUID().GetCounter(), guid }));
                auto it = configured.find(guid);
                c.Set("configured", it != configured.end() && it->second.configured);
                if (it != configured.end())
                {
                    c.Set("stageLabel", RelationshipEngine::StageLabel(it->second.relationship.stage, it->second.personality));
                    if (RelationshipEngine::Away(it->second.relationship, Now()))
                        c.Set("estrangedUntil", int64_t(it->second.relationship.estrangedUntil));
                    else if (it->second.relationship.estranged)
                        c.Set("estranged", true);
                }
                chars.Push(c);
            } while (r->NextRow());
        }
        Json out = Json::MakeObject();
        out.Set("op", "chars");
        out.Set("chars", chars);
        out.Set("maxActive", int64_t(_cfg.maxActiveCompanionsPerPlayer));
        return out;
    }

    bool RomanceManager::ActivateCompanion(Player* owner, Player* bot, bool announce)
    {
        CompanionKey key{ owner->GetGUID().GetCounter(), bot->GetGUID().GetCounter() };
        time_t now = Now();
        if (OtherFaction(owner, bot->getRace(true)))
        {
            // Added through Playerbots directly: it stays a regular bot, the AI is never attached.
            if (announce)
                Notice(owner, bot->GetName() + ": " + HumanError("other_faction", 0));
            return false;
        }
        CompanionRecord rec = _service->GetOrCreateRecord(key, owner->GetSession()->GetAccountId(), bot->GetName(), now);
        if (!rec.settings.aiEnabled)
        {
            Json p = Json::MakeObject();
            p.Set("op", "chars_changed");
            SendAddon(owner, p);
            return false;
        }
        auto r = _service->Activate(key, owner->GetSession()->GetAccountId(), bot->GetName(), owner->GetName(), now);
        if (!r.ok)
        {
            Notice(owner, bot->GetName() + " joined as a regular bot: " +
                              HumanError(r.error, _cfg.maxActiveCompanionsPerPlayer, EstrangedHours(owner, bot->GetGUID().GetCounter())));
            return false;
        }
        _trackers.erase(key);
        if (announce)
            _service->OnGameEvent(key, { GameEventType::Summoned, AreaName(owner->GetZoneId()), 0, now }, now);
        Json p = Json::MakeObject();
        p.Set("op", "chars_changed");
        SendAddon(owner, p);
        return true;
    }

    std::string RomanceManager::Summon(Player* owner, uint32 guidLow)
    {
        std::string name, error;
        if (!ResolveOwnedCompanion(owner, guidLow, name, error))
            return error;
        if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(PlayerGuid(guidLow));
            entry && OtherFaction(owner, entry->Race))
            return "other_faction";
        if (Player* existing = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guidLow)))
        {
            if (!PlayerbotsBridge::IsBot(existing))
                return "online_as_player";
            if (!PlayerbotsBridge::IsMyBot(existing, owner))
                return "controlled_elsewhere";
            return ActivateCompanion(owner, existing, true) ? "ok" : "activation_failed";
        }
        if (auto other = _service->ActiveByCompanion(guidLow); other && other->owner != owner->GetGUID().GetCounter())
            return "controlled_elsewhere";
        if (_pendingSummons.count(guidLow))
            return "already_loading";
        CompanionRecord rec = _service->GetOrCreateRecord({ owner->GetGUID().GetCounter(), guidLow }, owner->GetSession()->GetAccountId(), name, Now());
        if (rec.settings.aiEnabled && RelationshipEngine::Away(rec.relationship, Now()))
            return "estranged";
        if (rec.settings.aiEnabled && _service->ActiveForOwner(owner->GetGUID().GetCounter()).size() >= _cfg.maxActiveCompanionsPerPlayer)
            return "limit_reached";
        std::string res = PlayerbotsBridge::Summon(owner, PlayerGuid(guidLow));
        if (res == "ok")
            _pendingSummons[guidLow] = { owner->GetGUID().GetCounter(), Now() };
        return res;
    }

    std::string RomanceManager::Dismiss(Player* owner, uint32 guidLow, bool logout)
    {
        std::string name, error;
        if (!ResolveOwnedCompanion(owner, guidLow, name, error))
            return error;
        CompanionKey key{ owner->GetGUID().GetCounter(), guidLow };
        _service->Deactivate(key, Now());
        _trackers.erase(key);
        _control->Forget(PlayerGuid(guidLow));
        if (logout)
            PlayerbotsBridge::Dismiss(owner, PlayerGuid(guidLow));
        return "ok";
    }

    void RomanceManager::HandleAddonOp(Player* player, Json const& msg)
    {
        std::string op = msg.GetString("op");
        uint32 ownerGuid = player->GetGUID().GetCounter();
        uint32 account = player->GetSession()->GetAccountId();
        time_t now = Now();

        if (op == "hello")
        {
            Json p = Json::MakeObject();
            p.Set("op", "hello");
            p.Set("protocol", kProtocolVersion);
            p.Set("module", std::string(kModuleVersion));
            p.Set("enabled", _enabled);
            Json f = Json::MakeObject();
            f.Set("ollama", _cfg.ollamaEnabled);
            f.Set("proactive", _cfg.proactiveEnabled);
            f.Set("naturalCommands", _cfg.naturalLanguageCommands);
            f.Set("initialStage", _cfg.allowInitialStageSelection);
            f.Set("maxInitialStage", int64_t(_cfg.maxInitialStage));
            f.Set("modelChoice", _cfg.allowPlayerModelChoice);
            f.Set("export", _cfg.allowMemoryExport);
            f.Set("reset", _cfg.allowFullReset);
            f.Set("debug", _cfg.debugTabForPlayers || player->GetSession()->GetSecurity() >= SEC_GAMEMASTER);
            f.Set("playerbots", PlayerbotsBridge::Available());
            f.Set("memory", _cfg.memoryEnabled);
            f.Set("botFeedback", true);     // replies reach the addon before the chat line; bot_feedback pushes
            p.Set("features", f);
            p.Set("presets", PresetListJson(account));
            p.Set("maxCustomPresets", int64_t(_cfg.maxCustomPresets));
            Json stages = Json::MakeArray();
            for (int i = 0; i < kStageCount; ++i)
            {
                Json s = Json::MakeObject();
                s.Set("id", StageId(Stage(i)));
                s.Set("label", StageDefaultLabel(Stage(i)));
                stages.Push(s);
            }
            p.Set("stages", stages);
            p.Set("styles", [] { Json a = Json::MakeArray(); for (auto const& s : PersonalityProfile::KnownStyles()) a.Push(s); return a; }());
            p.Set("status", _service->StatusJson(nullptr));
            Reply(player, msg, p);
            return;
        }

        if (!_enabled)
        {
            ReplyError(player, msg, "module_disabled", "The romance module is disabled on this server.");
            return;
        }

        if (op == "list_chars")
        {
            Reply(player, msg, BuildCharacterList(player));
            return;
        }
        if (op == "get_status")
        {
            Json p = _service->StatusJson(nullptr);
            p.Set("op", "status");
            if (uint32 g = JsonGuid(msg))
            {
                CompanionKey key{ ownerGuid, g };
                p = _service->StatusJson(&key);
                p.Set("op", "status");
            }
            Reply(player, msg, p);
            return;
        }
        if (op == "get_profile")
        {
            bool isDefault = false;
            PlayerProfile prof = _service->GetPlayerProfile(account, ownerGuid, &isDefault);
            Json p = Json::MakeObject();
            p.Set("op", "profile");
            p.Set("profile", prof.ToJson());
            p.Set("accountDefault", isDefault);
            Reply(player, msg, p);
            return;
        }
        if (op == "save_profile")
        {
            auto res = _service->SavePlayerProfile(account, ownerGuid, PlayerProfile::FromJson(msg["profile"]), msg.GetBool("accountDefault", false));
            Json p = Json::MakeObject();
            p.Set("op", "ack");
            p.Set("for", op);
            p.Set("ok", res.ok);
            Reply(player, msg, p);
            return;
        }
        if (op == "list_presets" || op == "save_preset" || op == "delete_preset")
        {
            Json p = Json::MakeObject();
            p.Set("op", "presets");
            bool ok = true;
            std::string code;
            if (op == "save_preset")
            {
                auto res = _service->SaveCustomPreset(account, msg.GetString("label"), msg.GetString("description"),
                                                      PersonalityProfile::FromJson(msg["personality"]), msg.GetString("replace"));
                ok = res.ok;
                code = res.error;
                p.Set("savedId", res.id);
            }
            else if (op == "delete_preset")
            {
                ok = _service->DeleteCustomPreset(account, msg.GetString("preset"));
                if (!ok)
                    code = "unknown_preset";
            }
            p.Set("for", op);
            p.Set("ok", ok);
            if (!code.empty())
            {
                p.Set("code", code);
                p.Set("message", HumanError(code, _cfg.maxActiveCompanionsPerPlayer));
            }
            p.Set("presets", PresetListJson(account));
            Reply(player, msg, p);
            return;
        }
        if (op == "list_models")
        {
            Json p = Json::MakeObject();
            p.Set("op", "models");
            Json m = Json::MakeArray();
            if (_cfg.allowPlayerModelChoice)
                for (auto const& name : _service->Status().models)
                    if (_cfg.allowedModels.empty() || std::find(_cfg.allowedModels.begin(), _cfg.allowedModels.end(), name) != _cfg.allowedModels.end())
                        m.Push(name);
            p.Set("models", m);
            p.Set("default", _cfg.model);
            p.Set("choice", _cfg.allowPlayerModelChoice);
            Reply(player, msg, p);
            return;
        }

        // ---- everything below addresses one companion and requires ownership
        uint32 guid = JsonGuid(msg);
        std::string name, error;
        if (!ResolveOwnedCompanion(player, guid, name, error))
        {
            ReplyError(player, msg, error, "That character does not belong to you.");
            return;
        }
        CompanionKey key{ ownerGuid, guid };

        auto ack = [&](bool ok, std::string const& code = "", std::vector<std::string> const& notes = {})
        {
            Json p = Json::MakeObject();
            p.Set("op", "ack");
            p.Set("for", op);
            p.Set("guid", int64_t(guid));
            p.Set("ok", ok);
            if (!code.empty())
            {
                p.Set("code", code);
                p.Set("message", HumanError(code, _cfg.maxActiveCompanionsPerPlayer, EstrangedHours(player, guid)));
            }
            Json n = Json::MakeArray();
            for (auto const& s : notes)
                n.Push(s);
            p.Set("notes", n);
            Reply(player, msg, p);
        };
        auto sendCompanion = [&]()
        {
            CompanionRecord rec = _service->GetOrCreateRecord(key, account, name, now);
            Json p = _service->CompanionJson(rec);
            p.Set("op", "companion");
            std::string reason;
            p.Set("status", CharacterStatusId(StatusFor(player, guid, reason)));
            p.Set("reason", reason);
            Reply(player, msg, p);
        };

        if (op == "get_companion")
            sendCompanion();
        else if (op == "save_personality")
        {
            auto res = _service->SavePersonality(key, account, name, PersonalityProfile::FromJson(msg["personality"]), now);
            ack(res.ok, res.error, res.notes);
            sendCompanion();
        }
        else if (op == "get_character_sheet")
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guid));
            if (!bot || !bot->IsInWorld())
            {
                ack(false, "not_online");
                return;
            }
            Json p = _control->CharacterSheet(player, bot);
            p.Set("op", "character_sheet");
            p.Set("guid", int64_t(guid));
            Reply(player, msg, p);
        }
        else if (op == "apply_preset")
        {
            auto res = _service->ApplyPresetTo(key, account, name, msg.GetString("preset"), now);
            ack(res.ok, res.error, res.notes);
            sendCompanion();
        }
        else if (op == "set_relationship")
        {
            auto stage = StageFromId(msg.GetString("stage"));
            if (!stage)
            {
                ReplyError(player, msg, "invalid_stage");
                return;
            }
            auto res = _service->SetInitialRelationship(key, account, name, *stage, msg.GetBool("romantic", true),
                                                        msg.GetString("backstory"), now);
            ack(res.ok, res.error, res.notes);
            sendCompanion();
        }
        else if (op == "save_settings")
        {
            auto res = _service->SaveSettings(key, account, name, CompanionSettings::FromJson(msg["settings"]), now);
            ack(res.ok, res.error, res.notes);
            // Turning the AI on for an already-summoned bot activates it right away.
            CompanionRecord rec = _service->GetOrCreateRecord(key, account, name, now);
            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guid));
            if (rec.settings.aiEnabled && bot && PlayerbotsBridge::IsMyBot(bot, player) && !_service->IsActive(key))
                ActivateCompanion(player, bot, false);
            else if (!rec.settings.aiEnabled && _service->IsActive(key))
                _service->Deactivate(key, now);
            sendCompanion();
        }
        else if (op == "summon")
        {
            std::string res = Summon(player, guid);
            ack(res == "ok", res == "ok" ? "" : res);
        }
        else if (op == "dismiss")
        {
            std::string res = Dismiss(player, guid, msg.GetBool("logout", true));
            ack(res == "ok", res == "ok" ? "" : res);
        }
        else if (op == "chat")
        {
            std::string text = msg.GetString("text");
            Player* bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guid));
            if (bot && !_service->IsActive(key))
                EnsureActive(player, bot, false);   // talking in the Chat tab is an explicit choice
            if (!_service->IsActive(key) || !bot)
            {
                ReplyError(player, msg, "not_active", name + " is not with you right now.");
                return;
            }
            auto res = TalkTo(player, bot, key, text, ChatChannel::Addon, false);
            if (res.result == RomanceService::MessageResult::PassToPlayerbots)
            {
                ExpectBotFeedback(player, bot);
                PlayerbotsBridge::Command(bot, player, res.rewritten);
            }
            else if (res.result == RomanceService::MessageResult::Busy)
                ReplyError(player, msg, "busy", name + " is still answering.");
            else if (res.result == RomanceService::MessageResult::AiDisabled)
                ReplyError(player, msg, "ai_disabled");
        }
        else if (op == "get_history")
        {
            size_t limit = size_t(std::max<int64_t>(1, std::min<int64_t>(50, msg.GetInt("limit", 30))));
            Json items = Json::MakeArray();
            for (auto const& t : _service->RecentChat(key, limit))
            {
                Json j = Json::MakeObject();
                j.Set("from", t.role == ChatTurn::Role::Player ? "player" : t.role == ChatTurn::Role::Companion ? "companion" : "event");
                j.Set("text", t.text);
                j.Set("ts", int64_t(t.at));
                items.Push(j);
            }
            Json p = Json::MakeObject();
            p.Set("op", "history");
            p.Set("guid", int64_t(guid));
            p.Set("items", items);
            Reply(player, msg, p);
        }
        else if (op == "get_memories")
        {
            std::optional<MemoryKind> kind = MemoryKindFromId(msg.GetString("kind"));
            size_t offset = size_t(std::max<int64_t>(0, msg.GetInt("offset", 0)));
            size_t limit = size_t(std::max<int64_t>(1, std::min<int64_t>(25, msg.GetInt("limit", 15))));
            size_t total = 0;
            Json items = Json::MakeArray();
            for (auto const& m : _service->ListMemories(key, kind, LimitUtf8(msg.GetString("search"), 64), offset, limit, &total))
                items.Push(m.ToJson());
            Json p = Json::MakeObject();
            p.Set("op", "memories");
            p.Set("guid", int64_t(guid));
            p.Set("items", items);
            p.Set("total", int64_t(total));
            p.Set("offset", int64_t(offset));
            Reply(player, msg, p);
        }
        else if (op == "edit_memory")
        {
            std::optional<int> importance;
            if (msg.Has("importance"))
                importance = int(msg.GetInt("importance", 50));
            auto m = _service->EditMemory(key, uint32(msg.GetInt("id", 0)), msg.GetString("text"), importance, now);
            ack(m.has_value(), m ? "" : "not_found");
        }
        else if (op == "delete_memory")
            ack(_service->DeleteMemory(key, uint32(msg.GetInt("id", 0))), "");
        else if (op == "add_memory")
        {
            MemoryKind kind = MemoryKindFromId(msg.GetString("kind", "fact")).value_or(MemoryKind::Fact);
            auto m = _service->AddUserMemory(key, kind, msg.GetString("text"), int(msg.GetInt("importance", 60)), now);
            ack(m.has_value(), m ? "" : "invalid_text");
        }
        else if (op == "pin_memory")
        {
            auto m = _service->PinMemory(key, uint32(msg.GetInt("id", 0)), msg.GetBool("pinned", true));
            ack(m.has_value(), m ? "" : "not_found");
        }
        else if (op == "clear_memories")
        {
            std::optional<MemoryKind> kind = MemoryKindFromId(msg.GetString("kind"));
            size_t n = _service->ClearMemories(key, kind);
            ack(true, "", { std::to_string(n) + " memories removed" });
        }
        else if (op == "clear_history")
        {
            _service->ClearChatHistory(key);
            ack(true);
        }
        else if (op == "reset_companion")
        {
            if (msg.GetString("confirm") != "RESET")
            {
                ReplyError(player, msg, "confirmation_required", "Send confirm=RESET to erase everything.");
                return;
            }
            bool ok = _service->ResetCompanion(key, now);
            ack(ok, ok ? "" : "reset_disabled");
            sendCompanion();
        }
        else if (op == "export")
        {
            // Paged: the addon asks for part 0, 1, ... until `parts`; each page stays far below its receive limit.
            std::vector<std::string> pages = _service->ExportPages(key, account);
            if (pages.empty())
            {
                ReplyError(player, msg, "export_disabled", "Exporting memories is disabled on this server.");
                return;
            }
            int64_t part = msg.GetInt("part", 0);
            if (part < 0 || part >= int64_t(pages.size()))
            {
                ReplyError(player, msg, "bad_part");
                return;
            }
            Json p = Json::MakeObject();
            p.Set("op", "export_data");
            p.Set("guid", int64_t(guid));
            p.Set("part", part);
            p.Set("parts", int64_t(pages.size()));
            p.Set("data", pages[size_t(part)]);
            Reply(player, msg, p);
        }
        else if (op == "import_begin" || op == "import_part" || op == "import_end")
        {
            ImportAssembler& imp = _peers[player->GetGUID().GetCounter()].imports[guid];
            std::string err;
            if (op == "import_begin")
                err = imp.Begin(size_t(std::max<int64_t>(0, msg.GetInt("parts", 0))), now);
            else if (op == "import_part")
                err = imp.Add(size_t(std::max<int64_t>(0, msg.GetInt("index", -1))), msg.GetString("data"), now);
            else
            {
                auto full = imp.Finish(err);
                if (full)
                {
                    auto res = _service->ImportCompanion(key, account, name, *full, now);
                    ack(res.ok, res.error, res.notes);
                    sendCompanion();
                    return;
                }
            }
            ack(err.empty(), err);
        }
        else if (op == "import")
        {
            auto data = Json::Parse(msg.GetString("data"), nullptr, JsonParseLimits{ kMaxInboundMessageBytes, 12 });
            if (!data)
            {
                ReplyError(player, msg, "invalid_json");
                return;
            }
            auto res = _service->ImportCompanion(key, account, name, *data, now);
            ack(res.ok, res.error, res.notes);
        }
        else if (op == "get_debug")
        {
            if (!_cfg.debugTabForPlayers && player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
            {
                ReplyError(player, msg, "forbidden");
                return;
            }
            Json p = _service->StatusJson(&key);
            p.Set("op", "debug");
            if (SessionDebug const* d = _service->Debug(key))
                p.Set("debug", d->ToJson());
            Reply(player, msg, p);
        }
        else
            ReplyError(player, msg, "unknown_op", op.substr(0, 32));
    }

    // ================================================================ chat commands

    std::string RomanceManager::CommandStatus(Player* player)
    {
        if (!_service)
            return "Romance module is not running.";
        OllamaStatus const& s = _service->Status();
        std::ostringstream o;
        o << "Romance " << (_enabled ? "enabled" : "disabled") << ". Ollama: " << (s.online ? "online" : (s.checked ? "OFFLINE" : "checking..."));
        if (!s.version.empty())
            o << " (v" << s.version << ")";
        o << ", model " << _cfg.model << ", active companions " << _service->ActiveCount() << ", queue " << _service->PendingRequests() << ".";
        if (player)
            for (auto const& k : _service->ActiveForOwner(player->GetGUID().GetCounter()))
            {
                CompanionRecord rec = _service->GetOrCreateRecord(k, player->GetSession()->GetAccountId(), "", Now());
                o << " Your companion: " << rec.companionName << " ("
                  << RelationshipEngine::StageLabel(rec.relationship.stage, rec.personality) << ", " << MoodId(rec.relationship.mood) << ").";
            }
        if (!s.lastError.empty())
            o << " Last error: " << s.lastError;
        return o.str();
    }

    std::string RomanceManager::CommandSummon(Player* player, std::string const& name)
    {
        if (!_enabled)
            return "The romance module is disabled.";
        ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
        if (!guid)
            return "No character named " + name + ".";
        std::string res = Summon(player, guid.GetCounter());
        Execute(_service->TakeOutgoing());
        return res == "ok" ? "Calling " + name + "..."
                           : "Cannot call " + name + ": " + HumanError(res, _cfg.maxActiveCompanionsPerPlayer, EstrangedHours(player, guid.GetCounter()));
    }

    std::string RomanceManager::CommandDismiss(Player* player)
    {
        if (!_enabled)
            return "The romance module is disabled.";
        auto keys = _service->ActiveForOwner(player->GetGUID().GetCounter());
        if (keys.empty())
            return "You have no active companion.";
        for (auto const& k : keys)
            Dismiss(player, uint32(k.companion), true);
        Execute(_service->TakeOutgoing());
        return "Your companion says goodbye for now.";
    }

    std::string RomanceManager::CommandToggleAi(Player* player, bool on)
    {
        if (!_enabled)
            return "The romance module is disabled.";
        Player* target = player->GetSelectedPlayer();
        if (!target || !PlayerbotsBridge::IsMyBot(target, player))
            return "Target one of your own bots first.";
        CompanionKey key{ player->GetGUID().GetCounter(), target->GetGUID().GetCounter() };
        CompanionRecord rec = _service->GetOrCreateRecord(key, player->GetSession()->GetAccountId(), target->GetName(), Now());
        rec.settings.aiEnabled = on;
        _service->SaveSettings(key, player->GetSession()->GetAccountId(), target->GetName(), rec.settings, Now());
        if (on)
            ActivateCompanion(player, target, true);
        else
            _service->Deactivate(key, Now());
        Execute(_service->TakeOutgoing());
        return target->GetName() + (on ? " is now your AI companion." : " is a regular bot again.");
    }
}
