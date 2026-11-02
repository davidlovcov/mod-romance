/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * RomanceService: the engine-independent heart of the module.
 *
 * Threading contract
 *  - Every public method is called from ONE game thread (the AzerothCore world
 *    thread). The service holds no AzerothCore objects.
 *  - LLM requests are built here from immutable snapshots, executed on the
 *    bounded WorkerPool, and their results are handed back through a
 *    ResultQueue that Tick() drains on the game thread. All validation and
 *    every state mutation (relationship, memories, persistence) happens in Tick().
 *  - Effects that need the game world (chat, emotes, Playerbots commands, UI
 *    pushes) are returned as OutgoingAction values for the caller to execute.
 *
 * Only companions explicitly activated through Activate() ever reach the LLM.
 */

#ifndef MOD_ROMANCE_SERVICE_H
#define MOD_ROMANCE_SERVICE_H

#include "RomanceCommands.h"
#include "RomanceConfig.h"
#include "RomanceTransfer.h"
#include "RomanceGift.h"
#include "RomanceLlm.h"
#include "RomanceMemory.h"
#include "RomancePrompt.h"
#include "RomanceRelationship.h"
#include "RomanceRepository.h"
#include "RomanceText.h"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace romance
{
    enum class ChatChannel : uint8_t { Whisper = 0, Party = 1, Addon = 2 };

    struct OutgoingAction
    {
        enum class Type : uint8_t
        {
            Speak = 0,        // companion speaks `text` (whisper to owner or party, see channel)
            Emote,            // companion performs `action` emote towards owner
            Command,          // route `action` through Playerbots (follow/stay/attack/flee)
            UiPush,           // send `payload` to the owner's addon
            Notice,           // system message to the owner (not in character)
        };
        Type type = Type::UiPush;
        CompanionKey key;
        std::string text;
        ChatChannel channel = ChatChannel::Whisper;
        ActionType action = ActionType::None;
        Json payload;
    };

    struct OllamaStatus
    {
        bool online = false;
        bool checked = false;
        std::string version;
        std::vector<std::string> models;
        std::string lastError;
        uint32_t lastLatencyMs = 0;
        TimePoint lastCheck = 0;
        TimePoint lastSuccess = 0;
        Json ToJson() const;
    };

    struct SessionDebug
    {
        uint32_t lastPromptChars = 0;
        uint32_t lastLatencyMs = 0;
        std::string lastStatus;
        std::string lastError;
        std::vector<std::string> lastWarnings;
        std::string lastRaw;          // truncated raw model output
        uint32_t requests = 0;
        uint32_t failures = 0;
        Json ToJson() const;
    };

    class RomanceService
    {
    public:
        using ContextProvider = std::function<std::optional<WorldContext>(CompanionKey const&)>;

        RomanceService(RomanceConfig const& cfg, IRomanceRepository& repo, std::shared_ptr<ILlmBackend> backend,
                       uint32_t seed = 0);
        ~RomanceService();
        RomanceService(RomanceService const&) = delete;
        RomanceService& operator=(RomanceService const&) = delete;

        // Cancels in-flight requests, joins workers and persists every session.
        void Shutdown();
        // Rebuild worker pool / backend after a config reload.
        void Reconfigure(std::shared_ptr<ILlmBackend> backend);

        // ------------------------------------------------------------ records & lifecycle
        CompanionRecord GetOrCreateRecord(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName, TimePoint now);
        std::vector<CompanionRecord> ConfiguredCompanions(Guid owner);

        struct ActivateResult
        {
            bool ok = false;
            std::string error;      // limit_reached | module_disabled | ...
        };
        ActivateResult Activate(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                std::string const& ownerName, TimePoint now);
        void Deactivate(CompanionKey const& key, TimePoint now);
        bool IsActive(CompanionKey const& key) const;
        std::vector<CompanionKey> ActiveKeys() const;
        std::vector<CompanionKey> ActiveForOwner(Guid owner) const;
        std::optional<CompanionKey> ActiveByCompanion(Guid companion) const;
        size_t ActiveCount() const { return _sessions.size(); }

        // ------------------------------------------------------------ configuration
        struct SaveResult
        {
            bool ok = false;
            std::string error;
            std::vector<std::string> notes;
            std::string id;                       // id of a created object (custom presets)
        };
        SaveResult SavePersonality(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                   PersonalityProfile profile, TimePoint now);
        // Presets: built-in ones plus the account's custom presets.
        std::vector<PersonalityPreset> ListPresets(uint32_t account);
        std::optional<PersonalityPreset> FindPresetFor(uint32_t account, std::string const& id);
        // Replaces the companion's personality with the preset (name/nickname kept).
        SaveResult ApplyPresetTo(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                 std::string const& presetId, TimePoint now);
        // Saves `profile` as a custom preset; `replaceId` = "custom:<n>" overwrites one of the account's presets.
        SaveResult SaveCustomPreset(uint32_t account, std::string const& label, std::string const& description,
                                    PersonalityProfile profile, std::string const& replaceId);
        bool DeleteCustomPreset(uint32_t account, std::string const& id);

        SaveResult SetInitialRelationship(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                          Stage stage, bool romantic, std::string const& backstory, TimePoint now);
        SaveResult SaveSettings(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                CompanionSettings settings, TimePoint now);
        PlayerProfile GetPlayerProfile(uint32_t account, Guid owner, bool* isAccountDefault = nullptr);
        SaveResult SavePlayerProfile(uint32_t account, Guid owner, PlayerProfile profile, bool accountDefault);

        // ------------------------------------------------------------ gameplay input
        enum class MessageResult : uint8_t
        {
            NotForUs = 0,       // not an active companion conversation: let the message through untouched
            PassToPlayerbots,   // a Playerbots command: let it through (possibly rewritten)
            Accepted,           // roleplay message queued for the companion
            Busy,               // too many pending requests / queue full
            AiDisabled,
        };
        struct HandleResult
        {
            MessageResult result = MessageResult::NotForUs;
            std::string rewritten;                 // for PassToPlayerbots
            std::optional<ActionType> command;     // deterministic natural-language command to execute now
        };
        // `partyTarget`: in party chat, this companion was chosen to answer even though the message
        // does not contain her name (only companion in an all-bot group / last one talked to).
        HandleResult OnPlayerMessage(CompanionKey const& key, std::string const& text, ChatChannel channel,
                                     WorldContext const& ctx, TimePoint now, bool partyTarget = false);
        // True if `text` addresses this companion by name or nickname.
        bool IsAddressed(CompanionKey const& key, std::string const& text) const;
        TimePoint LastActivity(CompanionKey const& key) const;
        // The name an active companion goes by (personality name, else character name).
        std::string DisplayName(CompanionKey const& key) const;
        // What the game layer must do for a player line before it reaches the AI - the same for whisper, party and
        // addon chat: which game knowledge to add, and which companion command (if any) to carry out.
        // No command for raw Playerbots commands ("!follow"), with natural commands off, or with the AI off.
        struct ControlRequest
        {
            bool active = false;                       // an active AI companion: knowledge may be added
            uint32_t knowledgeTopics = 0;              // see romance::knowledge
            std::optional<CompanionCommand> command;
        };
        ControlRequest PrepareControl(CompanionKey const& key, std::string const& text, std::string const& characterName);
        // Relationship stage of an active companion (nullopt if not active).
        std::optional<Stage> StageOf(CompanionKey const& key) const;
        void OnGameEvent(CompanionKey const& key, GameEvent const& ev, TimePoint now);

        struct GiftOutcome
        {
            bool rewarded = false;
            std::string reason;          // duplicate | not_active | ...
            GiftEvaluation evaluation;
        };
        GiftOutcome OnGift(CompanionKey const& key, GiftItem const& item, WorldContext const& ctx, TimePoint now);

        // Drive the service: drains results, proactive dialogue, mood, persistence, health checks.
        void Tick(TimePoint now, ContextProvider const& contexts);
        std::vector<OutgoingAction> TakeOutgoing();

        // ------------------------------------------------------------ memory management (player owned)
        std::vector<MemoryEntry> ListMemories(CompanionKey const& key, std::optional<MemoryKind> kind, std::string const& search,
                                              size_t offset, size_t limit, size_t* total);
        bool DeleteMemory(CompanionKey const& key, uint32_t id);
        std::optional<MemoryEntry> EditMemory(CompanionKey const& key, uint32_t id, std::string const& text, std::optional<int> importance, TimePoint now);
        std::optional<MemoryEntry> AddUserMemory(CompanionKey const& key, MemoryKind kind, std::string const& text, int importance, TimePoint now);
        std::optional<MemoryEntry> PinMemory(CompanionKey const& key, uint32_t id, bool pinned);
        size_t ClearMemories(CompanionKey const& key, std::optional<MemoryKind> kind);
        void ClearChatHistory(CompanionKey const& key);
        bool ResetCompanion(CompanionKey const& key, TimePoint now);     // personality, relationship, memories, chat
        std::vector<ChatTurn> RecentChat(CompanionKey const& key, size_t limit);

        Json ExportCompanion(CompanionKey const& key, uint32_t ownerAccount);
        // The export split into pages for the addon (empty if export is disabled).
        std::vector<std::string> ExportPages(CompanionKey const& key, uint32_t ownerAccount, size_t maxPageBytes = kExportPageBytes);
        SaveResult ImportCompanion(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                   Json const& data, TimePoint now);

        // ------------------------------------------------------------ status
        OllamaStatus const& Status() const { return _status; }
        Json StatusJson(CompanionKey const* key) const;
        SessionDebug const* Debug(CompanionKey const& key) const;
        Json CompanionJson(CompanionRecord const& rec) const;
        Json RelationshipJson(CompanionKey const& key, RelationshipState const& st, PersonalityProfile const& p) const;
        void RequestHealthCheck() { _nextHealthCheck = 0; }
        size_t PendingRequests() const;
        RomanceConfig const& Config() const { return _cfg; }

        // exposed for tests
        MessageClassifier& Classifier() { return _classifier; }
        RelationshipEngine& Engine() { return _engine; }

    private:
        struct PendingReaction
        {
            GameEvent event;
            TimePoint notBefore = 0;
            TimePoint expires = 0;
            int priority = 0;
        };

        struct Session
        {
            CompanionRecord rec;
            std::string ownerName;
            MemoryStore memories;
            std::deque<ChatTurn> history;
            std::vector<GiftHistoryEntry> recentGifts;
            std::set<uint64_t> rewardedItems;
            std::vector<CancelToken> tokens;
            uint64_t generation = 0;
            uint32_t pending = 0;
            bool summarizing = false;
            bool dirty = false;
            TimePoint lastSave = 0;
            TimePoint lastReplyAt = 0;
            TimePoint lastActivity = 0;
            std::chrono::steady_clock::time_point lastAcceptedMessage{};
            TimePoint nextProactiveAt = 0;
            std::deque<TimePoint> proactiveTimes;
            uint32_t unansweredProactive = 0;
            TimePoint lastEventReaction = 0;
            TimePoint lastOfflineNotice = 0;
            TimePoint lastMoodDecay = 0;
            std::optional<PendingReaction> reaction;
            std::deque<std::pair<TimePoint, std::string>> recentEvents;
            bool nearDeathThisFight = false;
            int killsThisFight = 0;
            std::string language = "en";
            WorldContext lastContext;
            SessionDebug debug;

            explicit Session(RomanceConfig const& cfg) : memories(cfg) { }
        };

        struct JobContext
        {
            CompanionKey key;
            uint64_t generation = 0;
            bool summary = false;
            TriggerKind trigger = TriggerKind::PlayerMessage;
            ChatChannel channel = ChatChannel::Whisper;
            std::string playerText;
            size_t playerTextLen = 0;
            std::optional<ProposalDecision> proposal;
            std::optional<ActionType> executedCommand;
            std::vector<ChatMessage> messages;
            int retries = 0;
            std::vector<ChatTurn> summaryTurns;
            std::string eventMemory;          // memory text to store with a reaction (e.g. ceremony)
            CancelToken token;
            bool allowActions = false;
        };

        struct JobResult
        {
            JobContext ctx;
            LlmResult llm;
            bool health = false;
            std::optional<std::vector<std::string>> models;
            std::optional<std::string> version;
        };

        Session* Find(CompanionKey const& key);
        Session const* Find(CompanionKey const& key) const;
        CompanionRecord LoadOrDefault(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName, TimePoint now);
        void SaveRecord(Session& s, TimePoint now, bool force);
        void Persist(CompanionRecord const& rec);

        bool Submit(Session& s, JobContext ctx);
        LlmRequest MakeRequest(Session const& s, std::vector<ChatMessage> messages, bool summary) const;
        PromptInput MakePrompt(Session& s, TriggerKind trigger, std::string const& triggerText, WorldContext const& ctx,
                               TimePoint now, bool withHistory);
        void RequestReaction(Session& s, TriggerKind trigger, std::string const& text, WorldContext const& ctx, TimePoint now,
                             std::string const& eventMemory = std::string(), bool shortHint = true);

        void ProcessResult(JobResult& r, TimePoint now);
        void HandleReply(Session& s, JobResult& r, TimePoint now);
        void HandleSummary(Session& s, JobResult& r, TimePoint now);
        void HandleFailure(Session& s, JobResult& r, TimePoint now);
        void ApplyProposalOutcome(Session& s, ProposalDecision const& d, TimePoint now);
        void PushMemoryChanged(CompanionKey const& key, int64_t id);
        void Leave(Session& s, TimePoint now);
        void Reconcile(Session& s, TimePoint now);
        void StoreMemory(Session& s, MemoryKind kind, MemorySource src, int importance, std::string const& text, TimePoint now);
        void Deliver(Session& s, std::string const& reply, ChatChannel channel, TimePoint now);
        void PushRelationship(Session& s);
        void PushTyping(Session& s, bool on);
        void AddRecentEvent(Session& s, std::string const& text, TimePoint now);
        void MaybeProactive(Session& s, WorldContext const& ctx, TimePoint now);
        void MaybeFireReaction(Session& s, WorldContext const& ctx, TimePoint now);
        void ScheduleNextProactive(Session& s, TimePoint now);
        void MaybeSummarize(Session& s, TimePoint now);
        std::string ResolveLanguage(Session& s, std::string const& playerText);
        std::string FallbackLine(Session const& s, ProposalDecision const& d) const;
        void RunHealthCheck(TimePoint now);
        std::string PlayerName(Session const& s) const;
        PlayerProfile const& ProfileFor(Session& s);

        RomanceConfig const& _cfg;
        IRomanceRepository& _repo;
        std::shared_ptr<ILlmBackend> _backend;
        std::unique_ptr<WorkerPool> _pool;
        std::shared_ptr<ResultQueue<JobResult>> _results;
        RelationshipEngine _engine;
        GiftEvaluator _gifts;
        MessageClassifier _classifier;
        std::map<CompanionKey, std::unique_ptr<Session>> _sessions;
        std::map<std::pair<uint32_t, Guid>, PlayerProfile> _profileCache;
        std::vector<OutgoingAction> _out;
        OllamaStatus _status;
        TimePoint _nextHealthCheck = 0;
        bool _healthInFlight = false;
        bool _shutdown = false;
        std::mt19937 _rng;
        // Every activation, reset and reconfigure gets a new generation: results that were queued or in flight
        // for an earlier session of the same companion can never be applied to a later one.
        uint64_t _nextGeneration = 0;
    };
}

#endif
