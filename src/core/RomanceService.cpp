/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceService.h"
#include "RomancePresets.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace romance
{
    namespace
    {
        constexpr int64_t kSaveIntervalSec = 30;
        constexpr int64_t kOfflineNoticeSec = 120;
        constexpr size_t kMaxRecentEvents = 6;
        constexpr int64_t kRecentEventWindow = 15 * 60;

        double Round1(double v) { return std::round(v * 10.0) / 10.0; }

        Json ValuesJson(RelationshipValues const& v)
        {
            Json j = Json::MakeObject();
            j.Set("friendship", Round1(v.friendship));
            j.Set("affection", Round1(v.affection));
            j.Set("attraction", Round1(v.attraction));
            j.Set("trust", Round1(v.trust));
            j.Set("familiarity", Round1(v.familiarity));
            j.Set("comfort", Round1(v.comfort));
            j.Set("satisfaction", Round1(v.satisfaction));
            return j;
        }

        bool LooksLikeRequest(std::string const& text)
        {
            static char const* const stems[] = { "follow", "come", "stay", "wait", "attack", "fight", "kill", "run", "flee",
                                                 "retreat", "folg", "komm", "bleib", "warte", "greif", "kämpf", "lauf", "flieh",
                                                 "help me", "hilf mir" };
            std::string l = ToLower(text);
            for (char const* s : stems)
                if (l.find(s) != std::string::npos)
                    return true;
            return false;
        }

        double TokenOverlap(std::string const& a, std::string const& b)
        {
            auto ta = Tokenize(a);
            auto tb = Tokenize(b);
            if (ta.empty())
                return 0;
            size_t hit = 0;
            for (auto const& x : ta)
                for (auto const& y : tb)
                    if (x == y || (x.size() >= 5 && y.size() >= 5 && x.compare(0, 5, y, 0, 5) == 0))
                    {
                        ++hit;
                        break;
                    }
            return double(hit) / double(ta.size());
        }
    }

    Json OllamaStatus::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("online", online);
        j.Set("checked", checked);
        j.Set("version", version);
        Json m = Json::MakeArray();
        for (auto const& n : models)
            m.Push(n);
        j.Set("models", m);
        j.Set("lastError", lastError);
        j.Set("latencyMs", int64_t(lastLatencyMs));
        j.Set("lastCheck", int64_t(lastCheck));
        return j;
    }

    Json SessionDebug::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("promptChars", int64_t(lastPromptChars));
        j.Set("latencyMs", int64_t(lastLatencyMs));
        j.Set("status", lastStatus);
        j.Set("error", lastError);
        Json w = Json::MakeArray();
        for (auto const& s : lastWarnings)
            w.Push(s);
        j.Set("warnings", w);
        j.Set("raw", lastRaw);
        j.Set("requests", int64_t(requests));
        j.Set("failures", int64_t(failures));
        return j;
    }

    RomanceService::RomanceService(RomanceConfig const& cfg, IRomanceRepository& repo, std::shared_ptr<ILlmBackend> backend, uint32_t seed)
        : _cfg(cfg), _repo(repo), _backend(std::move(backend)),
          _pool(std::make_unique<WorkerPool>(cfg.maxConcurrentRequests, cfg.maxQueuedRequests)),
          _results(std::make_shared<ResultQueue<JobResult>>()),
          _engine(cfg, seed ? seed : std::random_device{}()), _gifts(cfg), _rng(seed ? seed + 1 : std::random_device{}())
    {
    }

    RomanceService::~RomanceService() { Shutdown(); }

    void RomanceService::Shutdown()
    {
        if (_shutdown)
            return;
        _shutdown = true;
        for (auto& [k, s] : _sessions)
            for (auto& t : s->tokens)
                t->store(true);
        if (_pool)
            _pool->Shutdown();
        _results->Drain();
        for (auto& [k, s] : _sessions)
            SaveRecord(*s, s->rec.updatedAt, true);
        _sessions.clear();
    }

    void RomanceService::Reconfigure(std::shared_ptr<ILlmBackend> backend)
    {
        for (auto& [k, s] : _sessions)
        {
            for (auto& t : s->tokens)
                t->store(true);
            s->tokens.clear();
            s->pending = 0;
            s->summarizing = false;
            ++s->generation;   // results of the old pool are ignored
        }
        if (_pool)
            _pool->Shutdown();
        _pool = std::make_unique<WorkerPool>(_cfg.maxConcurrentRequests, _cfg.maxQueuedRequests);
        _results->Drain();
        _healthInFlight = false;
        if (backend)
            _backend = std::move(backend);
        _nextHealthCheck = 0;
    }

    // ================================================================= records

    RomanceService::Session* RomanceService::Find(CompanionKey const& key)
    {
        auto it = _sessions.find(key);
        return it == _sessions.end() ? nullptr : it->second.get();
    }

    RomanceService::Session const* RomanceService::Find(CompanionKey const& key) const
    {
        auto it = _sessions.find(key);
        return it == _sessions.end() ? nullptr : it->second.get();
    }

    CompanionRecord RomanceService::LoadOrDefault(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName, TimePoint now)
    {
        if (auto rec = _repo.LoadCompanion(key))
        {
            if (!companionName.empty())
                rec->companionName = companionName;
            if (rec->personality.name.empty())
                rec->personality.name = rec->companionName;
            if (ownerAccount)
                rec->ownerAccount = ownerAccount;
            return *rec;
        }
        CompanionRecord rec;
        rec.key = key;
        rec.ownerAccount = ownerAccount;
        rec.companionName = companionName;
        rec.personality.name = companionName;
        rec.relationship = RelationshipEngine::InitialFor(Stage::Strangers, true, now);
        rec.settings.proactiveEnabled = _cfg.proactiveEnabled;
        rec.settings.naturalCommands = _cfg.naturalLanguageCommands;
        rec.settings.partyChatMode = _cfg.partyOnlyChat;
        rec.createdAt = now;
        rec.updatedAt = now;
        return rec;
    }

    CompanionRecord RomanceService::GetOrCreateRecord(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName, TimePoint now)
    {
        if (Session* s = Find(key))
            return s->rec;
        return LoadOrDefault(key, ownerAccount, companionName, now);
    }

    std::vector<CompanionRecord> RomanceService::ConfiguredCompanions(Guid owner)
    {
        std::vector<CompanionRecord> out = _repo.LoadCompanionsForOwner(owner);
        for (auto& r : out)
            if (Session* s = Find(r.key))
                r = s->rec;
        return out;
    }

    void RomanceService::Persist(CompanionRecord const& rec) { _repo.SaveCompanion(rec); }

    void RomanceService::SaveRecord(Session& s, TimePoint now, bool force)
    {
        if (!force && !s.dirty)
            return;
        s.rec.updatedAt = std::max(s.rec.updatedAt, now);
        Persist(s.rec);
        s.dirty = false;
        s.lastSave = now;
    }

    RomanceService::ActivateResult RomanceService::Activate(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                                            std::string const& ownerName, TimePoint now)
    {
        ActivateResult r;
        if (!_cfg.enabled)
        {
            r.error = "module_disabled";
            return r;
        }
        if (Session* existing = Find(key))
        {
            existing->ownerName = ownerName;
            r.ok = true;
            return r;
        }
        if (auto other = ActiveByCompanion(key.companion))
        {
            r.error = "companion_in_use";
            return r;
        }
        if (ActiveForOwner(key.owner).size() >= _cfg.maxActiveCompanionsPerPlayer)
        {
            r.error = "limit_reached";
            return r;
        }
        if (_sessions.size() >= _cfg.maxAiCompanionsServer)
        {
            r.error = "server_limit_reached";
            return r;
        }

        auto s = std::make_unique<Session>(_cfg);
        s->rec = LoadOrDefault(key, ownerAccount, companionName, now);
        if (RelationshipEngine::Away(s->rec.relationship, now))
        {
            r.error = "estranged";
            return r;
        }
        s->ownerName = ownerName;
        s->memories.Load(_repo.LoadMemories(key));
        for (auto const& t : _repo.LoadRecentChat(key, std::min<uint32_t>(_cfg.storedChatTurns, 60)))
            s->history.push_back(t);
        for (auto const& g : _repo.LoadGifts(key, now - 24 * 3600))
            s->recentGifts.push_back({ g.entry, g.at, g.score });
        s->rewardedItems = _repo.LoadRewardedItemGuids(key.companion);
        _engine.ApplyNeglect(s->rec.relationship, now);
        std::string lang = s->rec.personality.language;
        if (lang == "en" || lang == "de")
            s->language = lang;
        else if (_cfg.allowedLanguages == "de")
            s->language = "de";
        s->lastActivity = now;
        s->lastMoodDecay = now;
        s->lastSave = now;
        Session& ref = *s;
        _sessions.emplace(key, std::move(s));
        ScheduleNextProactive(ref, now);
        ref.dirty = true;
        SaveRecord(ref, now, true);

        Json p = Json::MakeObject();
        p.Set("op", "companion_active");
        p.Set("guid", int64_t(key.companion));
        p.Set("active", true);
        _out.push_back({ OutgoingAction::Type::UiPush, key, "", ChatChannel::Addon, ActionType::None, p });
        PushRelationship(ref);
        r.ok = true;
        return r;
    }

    void RomanceService::Deactivate(CompanionKey const& key, TimePoint now)
    {
        Session* s = Find(key);
        if (!s)
            return;
        for (auto& t : s->tokens)
            t->store(true);
        SaveRecord(*s, now, true);
        _sessions.erase(key);
        Json p = Json::MakeObject();
        p.Set("op", "companion_active");
        p.Set("guid", int64_t(key.companion));
        p.Set("active", false);
        _out.push_back({ OutgoingAction::Type::UiPush, key, "", ChatChannel::Addon, ActionType::None, p });
    }

    bool RomanceService::IsActive(CompanionKey const& key) const { return Find(key) != nullptr; }

    std::vector<CompanionKey> RomanceService::ActiveKeys() const
    {
        std::vector<CompanionKey> out;
        for (auto const& [k, s] : _sessions)
            out.push_back(k);
        return out;
    }

    std::vector<CompanionKey> RomanceService::ActiveForOwner(Guid owner) const
    {
        std::vector<CompanionKey> out;
        for (auto const& [k, s] : _sessions)
            if (k.owner == owner)
                out.push_back(k);
        return out;
    }

    std::optional<CompanionKey> RomanceService::ActiveByCompanion(Guid companion) const
    {
        for (auto const& [k, s] : _sessions)
            if (k.companion == companion)
                return k;
        return std::nullopt;
    }

    // ================================================================= configuration

    bool RomanceService::WantsCompanionCommand(CompanionKey const& key, std::string const& text, std::string* displayName)
    {
        Session* s = Find(key);
        if (!s || _shutdown || !s->rec.settings.aiEnabled)
            return false;
        std::string cname = s->rec.personality.name.empty() ? s->rec.companionName : s->rec.personality.name;
        if (displayName)
            *displayName = cname;
        bool natural = _cfg.naturalLanguageCommands && s->rec.settings.naturalCommands;
        if (!natural)
            return false;
        return _classifier.Classify(text, cname, natural).route == MessageRoute::Roleplay;
    }

    std::optional<Stage> RomanceService::StageOf(CompanionKey const& key) const
    {
        auto it = _sessions.find(key);
        if (it == _sessions.end())
            return std::nullopt;
        return it->second->rec.relationship.stage;
    }

    RomanceService::SaveResult RomanceService::SavePersonality(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                                               PersonalityProfile profile, TimePoint now)
    {
        SaveResult r;
        r.notes = profile.Sanitize(_cfg.maxProfileFieldChars, _cfg.maxInstructionChars);
        if (profile.name.empty())
            profile.name = companionName;
        if (!profile.preset.empty() && !FindPreset(profile.preset) && !CustomPresetNumber(profile.preset))
            profile.preset.clear();

        if (Session* s = Find(key))
        {
            bool romanceWasOn = s->rec.personality.romanceEnabled;
            s->rec.personality = profile;
            s->rec.configured = true;
            if (profile.language == "en" || profile.language == "de")
                s->language = profile.language;
            if (romanceWasOn && !profile.romanceEnabled && s->rec.relationship.stage > Stage::CloseFriends)
                r.notes.push_back("romance disabled: relationship stage is kept but treated as platonic");
            s->dirty = true;
            SaveRecord(*s, now, true);
            PushRelationship(*s);
        }
        else
        {
            CompanionRecord rec = LoadOrDefault(key, ownerAccount, companionName, now);
            rec.personality = profile;
            rec.configured = true;
            rec.updatedAt = now;
            Persist(rec);
        }
        r.ok = true;
        return r;
    }

    std::vector<PersonalityPreset> RomanceService::ListPresets(uint32_t account)
    {
        std::vector<PersonalityPreset> out = BuiltInPresets();
        for (auto& p : _repo.LoadCustomPresets(account))
        {
            p.custom = true;
            out.push_back(std::move(p));
        }
        return out;
    }

    std::optional<PersonalityPreset> RomanceService::FindPresetFor(uint32_t account, std::string const& id)
    {
        if (PersonalityPreset const* p = FindPreset(id))
            return *p;
        if (!CustomPresetNumber(id))
            return std::nullopt;
        for (auto& p : _repo.LoadCustomPresets(account))
            if (p.id == id)
            {
                p.custom = true;
                return p;
            }
        return std::nullopt;
    }

    RomanceService::SaveResult RomanceService::ApplyPresetTo(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                                             std::string const& presetId, TimePoint now)
    {
        SaveResult r;
        auto preset = FindPresetFor(ownerAccount, presetId);
        if (!preset)
        {
            r.error = "unknown_preset";
            return r;
        }
        PersonalityProfile p = GetOrCreateRecord(key, ownerAccount, companionName, now).personality;
        ApplyPreset(p, *preset);
        return SavePersonality(key, ownerAccount, companionName, p, now);
    }

    RomanceService::SaveResult RomanceService::SaveCustomPreset(uint32_t account, std::string const& labelIn, std::string const& description,
                                                                PersonalityProfile profile, std::string const& replaceId)
    {
        SaveResult r;
        std::string label = Trim(LimitUtf8(labelIn, 40));
        if (label.empty())
        {
            r.error = "preset_label_required";
            return r;
        }
        r.notes = profile.Sanitize(_cfg.maxProfileFieldChars, _cfg.maxInstructionChars);
        std::vector<PersonalityPreset> existing = _repo.LoadCustomPresets(account);
        uint32_t number = CustomPresetNumber(replaceId);
        if (number && std::none_of(existing.begin(), existing.end(), [&](PersonalityPreset const& p) { return p.id == replaceId; }))
            number = 0; // not one of this account's presets: create a new one
        if (!number)
        {
            if (existing.size() >= _cfg.maxCustomPresets)
            {
                r.error = "preset_limit_reached";
                return r;
            }
            for (auto const& p : existing)
                number = std::max(number, CustomPresetNumber(p.id));
            ++number;
        }
        PersonalityPreset preset;
        preset.id = CustomPresetId(number);
        preset.label = label;
        preset.description = Trim(LimitUtf8(description, 200));
        preset.custom = true;
        // A preset is a personality template: the companion's own name and nickname are not part of it.
        profile.name.clear();
        profile.nickname.clear();
        profile.stageLabels = {};
        profile.preset = preset.id;
        preset.profile = profile;
        _repo.SaveCustomPreset(account, number, preset);
        r.ok = true;
        r.id = preset.id;
        return r;
    }

    bool RomanceService::DeleteCustomPreset(uint32_t account, std::string const& id)
    {
        uint32_t number = CustomPresetNumber(id);
        if (!number)
            return false;
        auto existing = _repo.LoadCustomPresets(account);
        if (std::none_of(existing.begin(), existing.end(), [&](PersonalityPreset const& p) { return p.id == id; }))
            return false;
        _repo.DeleteCustomPreset(account, number);
        return true;
    }

    RomanceService::SaveResult RomanceService::SetInitialRelationship(CompanionKey const& key, uint32_t ownerAccount,
                                                                      std::string const& companionName, Stage stage, bool romantic,
                                                                      std::string const& backstoryIn, TimePoint now)
    {
        SaveResult r;
        if (!_cfg.allowInitialStageSelection)
        {
            r.error = "initial_stage_disabled";
            return r;
        }
        if (uint32_t(stage) > _cfg.maxInitialStage)
        {
            r.error = "stage_not_allowed";
            return r;
        }

        Session* s = Find(key);
        CompanionRecord rec = s ? s->rec : LoadOrDefault(key, ownerAccount, companionName, now);
        bool romanticFinal = romantic && rec.personality.romanceEnabled;
        if (!romanticFinal && stage > Stage::CloseFriends)
        {
            stage = Stage::CloseFriends;
            r.notes.push_back("platonic companions start at most as close friends");
        }
        rec.relationship = RelationshipEngine::InitialFor(stage, romanticFinal, now);
        rec.configured = true;
        rec.updatedAt = now;

        std::string backstory = LimitUtf8(Trim(backstoryIn), _cfg.maxProfileFieldChars);
        std::string pname = s ? PlayerName(*s) : std::string("you");
        std::string cname = rec.personality.name.empty() ? companionName : rec.personality.name;
        std::vector<std::pair<MemoryKind, std::string>> seeds;
        if (!backstory.empty())
            seeds.push_back({ MemoryKind::Relationship, backstory });
        if (stage >= Stage::Dating)
        {
            static char const* const what[] = { "", "", "", "", "", "are a couple", "are engaged", "are married" };
            seeds.push_back({ MemoryKind::Relationship, cname + " and " + (s ? pname : std::string("their partner")) + " " + what[int(stage)] + "." });
        }

        if (s)
        {
            s->rec = rec;
            for (auto const& [kind, text] : seeds)
                StoreMemory(*s, kind, MemorySource::Profile, 90, text, now);
            s->dirty = true;
            SaveRecord(*s, now, true);
            PushRelationship(*s);
        }
        else
        {
            Persist(rec);
            if (_cfg.memoryEnabled && !seeds.empty())
            {
                MemoryStore store(_cfg);
                store.Load(_repo.LoadMemories(key));
                for (auto const& [kind, text] : seeds)
                {
                    auto add = store.Add(kind, MemorySource::Profile, 90, text, now);
                    if (add.stored)
                        _repo.SaveMemory(key, add.entry);
                }
            }
        }
        _repo.LogRelationshipEvent(key, "initial_stage", StageId(stage), now);
        r.ok = true;
        return r;
    }

    RomanceService::SaveResult RomanceService::SaveSettings(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                                            CompanionSettings st, TimePoint now)
    {
        SaveResult r;
        if (!_cfg.allowPlayerModelChoice)
            st.model.clear();
        else if (!st.model.empty() && !_cfg.allowedModels.empty() &&
                 std::find(_cfg.allowedModels.begin(), _cfg.allowedModels.end(), st.model) == _cfg.allowedModels.end())
        {
            r.error = "model_not_allowed";
            return r;
        }
        if (st.temperature >= 0)
            st.temperature = std::max(_cfg.minTemperature, std::min(_cfg.maxTemperature, st.temperature));
        if (st.maxReplyChars > 0)
            st.maxReplyChars = int(std::min<uint32_t>(uint32_t(st.maxReplyChars), _cfg.maxReplyChars));
        if (!_cfg.proactiveEnabled)
            st.proactiveEnabled = false;
        if (!_cfg.naturalLanguageCommands)
            st.naturalCommands = false;
        if (_cfg.partyOnlyChat)
            st.partyChatMode = true;

        if (Session* s = Find(key))
        {
            s->rec.settings = st;
            s->dirty = true;
            SaveRecord(*s, now, true);
            ScheduleNextProactive(*s, now);
        }
        else
        {
            CompanionRecord rec = LoadOrDefault(key, ownerAccount, companionName, now);
            rec.settings = st;
            rec.updatedAt = now;
            Persist(rec);
        }
        r.ok = true;
        return r;
    }

    PlayerProfile RomanceService::GetPlayerProfile(uint32_t account, Guid owner, bool* isAccountDefault)
    {
        if (isAccountDefault)
            *isAccountDefault = false;
        auto it = _profileCache.find({ account, owner });
        if (it != _profileCache.end() && !it->second.Empty())
            return it->second;
        if (auto p = _repo.LoadPlayerProfile(account, owner); p && !p->Empty())
        {
            _profileCache[{ account, owner }] = *p;
            return *p;
        }
        if (isAccountDefault)
            *isAccountDefault = true;
        auto dit = _profileCache.find({ account, 0 });
        if (dit != _profileCache.end())
            return dit->second;
        PlayerProfile def = _repo.LoadPlayerProfile(account, 0).value_or(PlayerProfile{});
        _profileCache[{ account, 0 }] = def;
        return def;
    }

    RomanceService::SaveResult RomanceService::SavePlayerProfile(uint32_t account, Guid owner, PlayerProfile profile, bool accountDefault)
    {
        SaveResult r;
        profile.Sanitize(_cfg.maxProfileFieldChars);
        Guid target = accountDefault ? 0 : owner;
        _repo.SavePlayerProfile(account, target, profile);
        _profileCache[{ account, target }] = profile;
        if (accountDefault)
            _profileCache.erase({ account, owner }); // re-resolve character profile next time
        r.ok = true;
        return r;
    }

    PlayerProfile const& RomanceService::ProfileFor(Session& s)
    {
        static PlayerProfile const empty;
        GetPlayerProfile(s.rec.ownerAccount, s.rec.key.owner);
        auto it = _profileCache.find({ s.rec.ownerAccount, s.rec.key.owner });
        if (it != _profileCache.end() && !it->second.Empty())
            return it->second;
        auto dit = _profileCache.find({ s.rec.ownerAccount, 0 });
        return dit != _profileCache.end() ? dit->second : empty;
    }

    std::string RomanceService::PlayerName(Session const& s) const
    {
        auto it = _profileCache.find({ s.rec.ownerAccount, s.rec.key.owner });
        if (it != _profileCache.end() && !it->second.preferredName.empty())
            return it->second.preferredName;
        auto dit = _profileCache.find({ s.rec.ownerAccount, 0 });
        if (dit != _profileCache.end() && !dit->second.preferredName.empty())
            return dit->second.preferredName;
        return s.ownerName;
    }

    // ================================================================= conversation

    std::string RomanceService::ResolveLanguage(Session& s, std::string const& playerText)
    {
        std::string fixed = s.rec.personality.language;
        if (_cfg.allowedLanguages == "en" || _cfg.allowedLanguages == "de")
            return s.language = _cfg.allowedLanguages;
        if (fixed == "en" || fixed == "de")
            return s.language = fixed;
        std::string detected = DetectLanguage(playerText);
        if (!detected.empty())
            s.language = detected;
        return s.language;
    }

    PromptInput RomanceService::MakePrompt(Session& s, TriggerKind trigger, std::string const& triggerText, WorldContext const& ctx,
                                           TimePoint now, bool withHistory)
    {
        PromptInput in;
        in.trigger = trigger;
        in.triggerText = triggerText;
        in.personality = &s.rec.personality;
        in.player = &ProfileFor(s);
        in.relationship = &s.rec.relationship;
        in.relationshipDescription = _engine.DescribeForPrompt(s.rec.relationship, s.rec.personality);
        if (_cfg.memoryEnabled)
        {
            std::string query = triggerText;
            for (auto it = s.history.rbegin(); it != s.history.rend() && std::distance(s.history.rbegin(), it) < 2; ++it)
                query += " " + it->text;
            in.memories = s.memories.Retrieve(query, now, _cfg.memoryRetrievalCount, _cfg.memoryRetrievalChars);
        }
        in.world = ctx;
        in.world.recentEvents.clear();
        for (auto const& [at, text] : s.recentEvents)
            if (now - at <= kRecentEventWindow)
                in.world.recentEvents.push_back(text + " (" + DialogueContextBuilder::RelativeTime(at, now) + ")");
        in.includeWorld = _cfg.gameplayContext;
        if (withHistory)
        {
            size_t n = std::min<size_t>(_cfg.historyTurns, s.history.size());
            in.history.assign(s.history.end() - long(n), s.history.end());
        }
        in.companionName = s.rec.personality.name.empty() ? s.rec.companionName : s.rec.personality.name;
        in.worldName = s.rec.companionName;
        in.playerName = s.ownerName;
        in.language = s.language;
        uint32_t maxChars = _cfg.maxReplyChars;
        if (s.rec.settings.maxReplyChars > 0)
            maxChars = std::min<uint32_t>(maxChars, uint32_t(s.rec.settings.maxReplyChars));
        in.maxReplyChars = maxChars;
        in.allowActions = trigger == TriggerKind::PlayerMessage && _cfg.naturalLanguageCommands && s.rec.settings.naturalCommands;
        in.now = now;
        return in;
    }

    LlmRequest RomanceService::MakeRequest(Session const& s, std::vector<ChatMessage> messages, bool summary) const
    {
        LlmRequest req;
        req.model = _cfg.model;
        if (_cfg.allowPlayerModelChoice && !s.rec.settings.model.empty())
            req.model = s.rec.settings.model;
        req.fallbackModel = _cfg.fallbackModel;
        req.messages = std::move(messages);
        if (_cfg.structuredOutput)
            req.schema = summary ? DialogueContextBuilder::SummarySchema() : DialogueContextBuilder::ReplySchema();
        req.temperature = summary ? 0.3 : (s.rec.settings.temperature >= 0 ? s.rec.settings.temperature : _cfg.temperature);
        req.numCtx = _cfg.contextLength;
        req.numPredict = summary ? std::max<uint32_t>(_cfg.maxResponseTokens, 400) : _cfg.maxResponseTokens;
        req.think = _cfg.thinking;
        req.keepAlive = _cfg.keepAlive;
        req.timeoutMs = _cfg.requestTimeoutMs;
        req.connectTimeoutMs = _cfg.connectTimeoutMs;
        return req;
    }

    bool RomanceService::Submit(Session& s, JobContext ctx)
    {
        if (_shutdown || !_pool)
            return false;
        if (!ctx.summary && s.pending >= _cfg.maxPendingPerCompanion)
            return false;
        ctx.key = s.rec.key;
        ctx.generation = s.generation;
        ctx.token = MakeCancelToken();
        LlmRequest req = MakeRequest(s, ctx.messages, ctx.summary);
        size_t promptChars = 0;
        for (auto const& m : req.messages)
            promptChars += m.content.size();

        std::shared_ptr<ILlmBackend> backend = _backend;
        std::shared_ptr<ResultQueue<JobResult>> results = _results;
        CancelToken token = ctx.token;
        auto job = [backend, results, req = std::move(req), token, ctx]() mutable
        {
            JobResult r;
            r.ctx = std::move(ctx);
            if (token->load())
                r.llm.status = LlmResult::Status::Cancelled;
            else
                r.llm = backend->Chat(req, token);
            results->Push(std::move(r));
        };
        if (!_pool->Submit(std::move(job)))
            return false;
        s.tokens.push_back(token);
        if (!ctx.summary)
            ++s.pending;
        ++s.debug.requests;
        s.debug.lastPromptChars = uint32_t(promptChars);
        return true;
    }

    bool RomanceService::IsAddressed(CompanionKey const& key, std::string const& text) const
    {
        Session const* s = Find(key);
        if (!s)
            return false;
        std::string cname = s->rec.personality.name.empty() ? s->rec.companionName : s->rec.personality.name;
        return CountName(text, cname) > 0 || CountName(text, s->rec.companionName) > 0 ||
               (!s->rec.personality.nickname.empty() && CountName(text, s->rec.personality.nickname) > 0);
    }

    std::string RomanceService::DisplayName(CompanionKey const& key) const
    {
        Session const* s = Find(key);
        if (!s)
            return "";
        return s->rec.personality.name.empty() ? s->rec.companionName : s->rec.personality.name;
    }

    TimePoint RomanceService::LastActivity(CompanionKey const& key) const
    {
        Session const* s = Find(key);
        return s ? std::max(s->lastActivity, s->lastReplyAt) : 0;
    }

    RomanceService::HandleResult RomanceService::OnPlayerMessage(CompanionKey const& key, std::string const& text, ChatChannel channel,
                                                                WorldContext const& ctx, TimePoint now, bool partyTarget)
    {
        HandleResult hr;
        Session* s = Find(key);
        if (!s || _shutdown)
            return hr;

        std::string cname = s->rec.personality.name.empty() ? s->rec.companionName : s->rec.personality.name;
        bool natural = _cfg.naturalLanguageCommands && s->rec.settings.naturalCommands;
        ClassifiedMessage cm = _classifier.Classify(text, cname, natural);
        if (cm.route == MessageRoute::Ignore)
            return hr;

        if (channel == ChatChannel::Party)
        {
            // In party chat she answers when addressed by name, or when the caller picked her as the
            // target of an unaddressed line (see RomanceManager::OnGroupChat).
            if (!s->rec.settings.listenInParty || !(partyTarget || IsAddressed(key, text)))
                return hr;
        }

        if (cm.route == MessageRoute::PlayerbotCommand)
        {
            hr.result = MessageResult::PassToPlayerbots;
            hr.rewritten = cm.text;
            return hr;
        }

        if (!s->rec.settings.aiEnabled || !_cfg.ollamaEnabled)
        {
            hr.result = MessageResult::AiDisabled;
            return hr;
        }

        std::string playerText = SanitizePlayerText(cm.text, _cfg.maxPlayerMessageChars);
        if (playerText.empty())
            return hr;

        if (s->pending >= _cfg.maxPendingPerCompanion)
        {
            hr.result = MessageResult::Busy;
            return hr;
        }
        // Minimum spacing between roleplay messages (anti-spam); Playerbots commands are not affected.
        auto steadyNow = std::chrono::steady_clock::now();
        if (_cfg.minResponseCooldownMs > 0 && s->lastAcceptedMessage.time_since_epoch().count() != 0 &&
            steadyNow - s->lastAcceptedMessage < std::chrono::milliseconds(_cfg.minResponseCooldownMs))
        {
            hr.result = MessageResult::Busy;
            return hr;
        }

        ResolveLanguage(*s, playerText);

        std::optional<ProposalDecision> decision;
        if (cm.proposal != ProposalIntent::None)
        {
            ProposalKind kind = ProposalKind::None;
            switch (cm.proposal)
            {
                case ProposalIntent::Dating: kind = ProposalKind::Dating; break;
                case ProposalIntent::Engage: kind = ProposalKind::Engagement; break;
                case ProposalIntent::Marry:
                    kind = s->rec.relationship.stage == Stage::Engaged ? ProposalKind::Marriage : ProposalKind::Engagement;
                    break;
                case ProposalIntent::Breakup: kind = ProposalKind::Breakup; break;
                default: break;
            }
            if (kind == ProposalKind::Engagement && s->rec.relationship.stage < Stage::Dating)
                kind = ProposalKind::Engagement; // evaluated as "too soon"
            ProposalDecision d = _engine.EvaluateProposal(s->rec.relationship, kind, s->rec.personality, now);
            if (d.outcome != ProposalOutcome::None)
                decision = d;
        }

        // The game layer already carried out a companion command (give, cast, attack a named enemy, ...):
        // the outcome is in the context and no second command may run.
        if (ctx.commandHandled)
            cm.action.reset();
        if (cm.action)
        {
            if (IsMovementCommand(*cm.action))
                hr.command = cm.action;
        }

        PromptInput in = MakePrompt(*s, TriggerKind::PlayerMessage, playerText, ctx, now, true);
        in.proposal = decision;
        in.requestedAction = cm.action;
        JobContext jc;
        jc.trigger = TriggerKind::PlayerMessage;
        jc.channel = channel;
        jc.playerText = playerText;
        jc.playerTextLen = playerText.size();
        jc.proposal = decision;
        jc.executedCommand = hr.command;
        jc.allowActions = in.allowActions;
        jc.messages = DialogueContextBuilder::Build(in);

        if (!Submit(*s, std::move(jc)))
        {
            hr.result = MessageResult::Busy;
            return hr;
        }

        s->lastAcceptedMessage = steadyNow;
        // Record the player's turn only once it is queued.
        ChatTurn turn{ ChatTurn::Role::Player, playerText, now };
        s->history.push_back(turn);
        while (s->history.size() > _cfg.storedChatTurns)
            s->history.pop_front();
        _repo.AppendChat(key, turn);
        ++s->rec.turnsSinceSummary;
        s->unansweredProactive = 0;
        s->lastActivity = now;
        s->rec.relationship.lastPlayerMessage = now;
        s->lastContext = ctx;
        s->dirty = true;

        Json p = Json::MakeObject();
        p.Set("op", "chat_msg");
        p.Set("guid", int64_t(key.companion));
        p.Set("from", "player");
        p.Set("text", playerText);
        p.Set("ts", int64_t(now));
        _out.push_back({ OutgoingAction::Type::UiPush, key, "", ChatChannel::Addon, ActionType::None, p });
        PushTyping(*s, true);

        hr.result = MessageResult::Accepted;
        return hr;
    }

    void RomanceService::AddRecentEvent(Session& s, std::string const& text, TimePoint now)
    {
        s.recentEvents.push_back({ now, text });
        while (s.recentEvents.size() > kMaxRecentEvents)
            s.recentEvents.pop_front();
    }

    void RomanceService::OnGameEvent(CompanionKey const& key, GameEvent const& ev, TimePoint now)
    {
        Session* s = Find(key);
        if (!s)
            return;
        RelationshipDelta d = _engine.ApplyGameEvent(s->rec.relationship, ev, s->rec.personality, now);
        if (d.Any())
            s->dirty = true;
        if (d.stageChangedTo)
        {
            _repo.LogRelationshipEvent(key, "stage_change", std::string(StageId(*d.stageChangedFrom)) + "->" + StageId(*d.stageChangedTo), now);
            PushRelationship(*s);
        }

        std::string pname = PlayerName(*s);
        std::string cname = s->rec.personality.name.empty() ? s->rec.companionName : s->rec.personality.name;
        int priority = 0;
        bool always = false;
        std::string text;
        switch (ev.type)
        {
            case GameEventType::CombatStart:
                s->nearDeathThisFight = false;
                s->killsThisFight = 0;
                break;
            case GameEventType::CombatEnd:
                s->killsThisFight = ev.value;
                if (s->nearDeathThisFight)
                {
                    text = "The fight just ended. It was close: " + pname + " was badly hurt during it.";
                    priority = 3;
                    always = true;
                    AddRecentEvent(*s, "a dangerous fight in which " + pname + " nearly fell", now);
                }
                else if (ev.value > 0)
                {
                    text = "You just won a fight together (" + std::to_string(ev.value) + (ev.value == 1 ? " enemy" : " enemies") +
                           (ev.detail.empty() ? "" : ", including " + ev.detail) + ").";
                    priority = 1;
                    AddRecentEvent(*s, "won a fight against " + std::to_string(ev.value) + (ev.value == 1 ? " enemy" : " enemies") +
                                       (ev.detail.empty() ? "" : " (" + ev.detail + ")"), now);
                }
                s->nearDeathThisFight = false;
                break;
            case GameEventType::PlayerNearDeath:
                s->nearDeathThisFight = true;
                break;
            case GameEventType::PlayerDied:
                text = pname + " was just killed in battle.";
                priority = 3;
                always = true;
                AddRecentEvent(*s, pname + " died in battle", now);
                break;
            case GameEventType::CompanionDied:
                AddRecentEvent(*s, "you were knocked out in battle", now);
                break;
            case GameEventType::CompanionLevelUp:
                text = "You just reached level " + std::to_string(ev.value) + ".";
                priority = 2;
                always = true;
                AddRecentEvent(*s, "you reached level " + std::to_string(ev.value), now);
                StoreMemory(*s, MemoryKind::Episode, MemorySource::Observed, 40,
                            cname + " reached level " + std::to_string(ev.value) + " while adventuring with " + pname + ".", now);
                break;
            case GameEventType::PlayerLevelUp:
                text = pname + " just reached level " + std::to_string(ev.value) + ".";
                priority = 2;
                always = true;
                AddRecentEvent(*s, pname + " reached level " + std::to_string(ev.value), now);
                break;
            case GameEventType::QuestComplete:
                text = "You just completed the quest \"" + ev.detail + "\" together.";
                priority = 1;
                AddRecentEvent(*s, "completed the quest \"" + ev.detail + "\"", now);
                StoreMemory(*s, MemoryKind::Episode, MemorySource::Observed, 30,
                            cname + " and " + pname + " completed the quest \"" + ev.detail + "\".", now);
                break;
            case GameEventType::NotableLoot:
                text = pname + " just looted " + ev.detail + ".";
                priority = 1;
                AddRecentEvent(*s, pname + " found " + ev.detail, now);
                break;
            case GameEventType::ZoneChanged:
                if (!ev.detail.empty())
                {
                    text = "You just arrived in " + ev.detail + " together.";
                    priority = 1;
                    AddRecentEvent(*s, "arrived in " + ev.detail, now);
                    StoreMemory(*s, MemoryKind::Episode, MemorySource::Observed, 25,
                                cname + " and " + pname + " traveled to " + ev.detail + " together.", now);
                }
                break;
            case GameEventType::Summoned:
            case GameEventType::PlayerReturned:
            {
                std::string since = s->rec.relationship.lastInteraction > 0 && now - s->rec.relationship.lastInteraction > 600
                                        ? " You last spent time together " + DialogueContextBuilder::RelativeTime(s->rec.relationship.lastInteraction, now) + "."
                                        : "";
                text = "You have just joined " + pname + " again" + (ev.detail.empty() ? "" : " in " + ev.detail) + "." + since;
                priority = 2;
                always = true;
                break;
            }
            case GameEventType::EquipmentChanged:
                if (!ev.detail.empty())
                    AddRecentEvent(*s, "you equipped " + ev.detail, now);
                break;
            case GameEventType::CompanionNotice:
                text = ev.detail;
                priority = std::max(1, std::min(3, ev.value));
                always = ev.value >= 2;
                break;
            default:
                break;
        }
        if (text.empty() || priority == 0)
            return;
        if (!always)
        {
            int64_t cooldown = _cfg.eventReactionCooldownSec;
            if (now - s->lastEventReaction < cooldown)
                return;
            std::uniform_real_distribution<double> u(0.0, 1.0);
            if (u(_rng) > _cfg.eventReactionChance)
                return;
        }
        if (s->reaction && s->reaction->priority > priority && s->reaction->expires >= now)
            return;
        PendingReaction pr;
        pr.event = ev;
        pr.event.detail = text;
        pr.notBefore = now;
        pr.expires = now + 120;
        pr.priority = priority;
        s->reaction = pr;
    }

    RomanceService::GiftOutcome RomanceService::OnGift(CompanionKey const& key, GiftItem const& item, WorldContext const& ctx, TimePoint now)
    {
        GiftOutcome out;
        Session* s = Find(key);
        if (!s)
        {
            out.reason = "not_active";
            return out;
        }
        if (item.itemGuid && s->rewardedItems.count(item.itemGuid))
        {
            out.reason = "duplicate";
            return out;
        }
        std::vector<GiftHistoryEntry> recent;
        for (auto const& g : s->recentGifts)
            if (now - g.at <= 24 * 3600)
                recent.push_back(g);
        out.evaluation = _gifts.Evaluate(item, s->rec.personality, s->rec.relationship, recent, now);
        GiftEvaluation const& ev = out.evaluation;
        _engine.ApplyGift(s->rec.relationship, ev, now);
        s->dirty = true;

        if (item.itemGuid)
            s->rewardedItems.insert(item.itemGuid);
        s->recentGifts.push_back({ item.entry, now, ev.score });
        if (s->recentGifts.size() > 50)
            s->recentGifts.erase(s->recentGifts.begin());
        GiftRecord gr;
        gr.itemGuid = item.itemGuid;
        gr.entry = item.entry;
        gr.count = item.count;
        gr.score = ev.score;
        gr.category = GiftCategoryId(ev.category);
        gr.name = item.name;
        gr.at = now;
        _repo.InsertGift(key, gr);

        std::string pname = PlayerName(*s);
        std::string cname = s->rec.personality.name.empty() ? s->rec.companionName : s->rec.personality.name;
        std::string desc = GiftEvaluator::Describe(item, ev.category);
        std::string where = ctx.zone.empty() ? "" : " in " + ctx.zone;
        if (ev.memorable)
        {
            std::string text = pname + " gave " + cname + " " + desc + where + ".";
            if (ev.isRing && s->rec.relationship.stage >= Stage::Dating)
                text += " The ring meant a great deal to " + cname + ".";
            int importance = int(std::max(40.0, std::min(90.0, 40.0 + ev.score / 2.0)));
            StoreMemory(*s, MemoryKind::Gift, MemorySource::Observed, importance, text, now);
        }
        AddRecentEvent(*s, pname + " gave you " + desc, now);

        std::string notes;
        for (auto const& n : ev.notes)
            notes += (notes.empty() ? "" : "; ") + n;
        std::string text = pname + " just handed you " + desc + " as a gift." +
                           (notes.empty() ? "" : " About it: " + notes + ".") +
                           " How meaningful it is to you: " + std::to_string(int(ev.score)) + "/100.";
        RequestReaction(*s, TriggerKind::Gift, text, ctx, now, "", ev.score < 25);

        PushRelationship(*s);
        _repo.LogRelationshipEvent(key, "gift", desc + " score=" + std::to_string(int(ev.score)), now);
        out.rewarded = true;
        return out;
    }

    void RomanceService::RequestReaction(Session& s, TriggerKind trigger, std::string const& text, WorldContext const& ctx, TimePoint now,
                                         std::string const& eventMemory, bool shortHint)
    {
        if (!s.rec.settings.aiEnabled || !_cfg.ollamaEnabled)
            return;
        PromptInput in = MakePrompt(s, trigger, text, ctx, now, true);
        in.shortReplyHint = shortHint;
        JobContext jc;
        jc.trigger = trigger;
        jc.channel = (s.rec.settings.partyChatMode || _cfg.partyOnlyChat) ? ChatChannel::Party : ChatChannel::Whisper;
        jc.eventMemory = eventMemory;
        jc.messages = DialogueContextBuilder::Build(in);
        if (!Submit(s, std::move(jc)))
        {
            // keep it for later if the companion is busy
            PendingReaction pr;
            pr.event.detail = text;
            pr.event.type = GameEventType::CombatEnd;
            pr.notBefore = now + 2;
            pr.expires = now + 90;
            pr.priority = trigger == TriggerKind::Gift ? 4 : 1;
            if (!s.reaction || s.reaction->priority <= pr.priority)
                s.reaction = pr;
            return;
        }
        if (trigger != TriggerKind::Proactive)
        {
            ChatTurn ev{ ChatTurn::Role::Event, text, now };
            s.history.push_back(ev);
            while (s.history.size() > _cfg.storedChatTurns)
                s.history.pop_front();
            _repo.AppendChat(s.rec.key, ev);
        }
        s.lastEventReaction = now;
        PushTyping(s, true);
    }

    // ================================================================= tick

    void RomanceService::Tick(TimePoint now, ContextProvider const& contexts)
    {
        if (_shutdown)
            return;
        for (auto& r : _results->Drain())
            ProcessResult(r, now);

        if (_cfg.ollamaEnabled && !_healthInFlight && now >= _nextHealthCheck)
            RunHealthCheck(now);

        for (auto& [key, sp] : _sessions)
        {
            Session& s = *sp;
            if (now > s.lastMoodDecay)
            {
                _engine.DecayMood(s.rec.relationship, double(now - s.lastMoodDecay));
                s.lastMoodDecay = now;
            }
            std::optional<WorldContext> ctx = contexts ? contexts(key) : std::nullopt;
            if (ctx)
            {
                s.lastContext = *ctx;
                MaybeFireReaction(s, *ctx, now);
                MaybeProactive(s, *ctx, now);
            }
            MaybeSummarize(s, now);
            if (s.dirty && now - s.lastSave >= kSaveIntervalSec)
                SaveRecord(s, now, false);
        }
    }

    std::vector<OutgoingAction> RomanceService::TakeOutgoing()
    {
        std::vector<OutgoingAction> out;
        out.swap(_out);
        return out;
    }

    void RomanceService::RunHealthCheck(TimePoint now)
    {
        _nextHealthCheck = now + std::max<uint32_t>(5, _cfg.healthCheckIntervalSec);
        std::shared_ptr<ILlmBackend> backend = _backend;
        std::shared_ptr<ResultQueue<JobResult>> results = _results;
        uint32_t timeout = std::min<uint32_t>(_cfg.requestTimeoutMs, 5000);
        auto job = [backend, results, timeout]()
        {
            JobResult r;
            r.health = true;
            r.version = backend->Version(timeout);
            if (r.version)
                r.models = backend->ListModels(timeout);
            results->Push(std::move(r));
        };
        if (_pool && _pool->Submit(std::move(job)))
            _healthInFlight = true;
        _status.lastCheck = now;
    }

    void RomanceService::ProcessResult(JobResult& r, TimePoint now)
    {
        if (r.health)
        {
            _healthInFlight = false;
            bool wasOnline = _status.online;
            _status.checked = true;
            _status.online = r.version.has_value();
            if (r.version)
            {
                _status.version = *r.version;
                _status.lastSuccess = now;
                if (r.models)
                    _status.models = *r.models;
                _status.lastError.clear();
            }
            else
            {
                _status.lastError = "Ollama is not reachable at " + _cfg.ollamaUrl;
                _nextHealthCheck = now + std::min<uint32_t>(15, _cfg.healthCheckIntervalSec);
            }
            if (wasOnline != _status.online)
                for (auto& [k, s] : _sessions)
                {
                    Json p = StatusJson(&k);
                    p.Set("op", "status");
                    _out.push_back({ OutgoingAction::Type::UiPush, k, "", ChatChannel::Addon, ActionType::None, p });
                }
            return;
        }

        Session* s = Find(r.ctx.key);
        if (!s || s->generation != r.ctx.generation)
            return; // companion dismissed or service reconfigured meanwhile
        s->tokens.erase(std::remove(s->tokens.begin(), s->tokens.end(), r.ctx.token), s->tokens.end());
        if (r.ctx.summary)
            s->summarizing = false;
        else if (s->pending > 0)
            --s->pending;

        s->debug.lastLatencyMs = r.llm.latencyMs;
        s->debug.lastStatus = LlmStatusId(r.llm.status);
        _status.lastLatencyMs = r.llm.latencyMs;

        if (r.ctx.summary)
        {
            if (r.llm.Ok())
                HandleSummary(*s, r, now);
            return;
        }
        if (!r.llm.Ok())
        {
            HandleFailure(*s, r, now);
            return;
        }
        if (!_status.online)
        {
            _status.online = true;
            _status.lastError.clear();
        }
        HandleReply(*s, r, now);
    }

    void RomanceService::HandleFailure(Session& s, JobResult& r, TimePoint now)
    {
        ++s.debug.failures;
        s.debug.lastError = r.llm.error;
        if (r.llm.status == LlmResult::Status::Cancelled)
        {
            PushTyping(s, false);
            return;
        }
        if (r.llm.status == LlmResult::Status::Offline)
        {
            _status.online = false;
            _status.lastError = r.llm.error;
            _nextHealthCheck = std::min(_nextHealthCheck, now + 10);
        }
        PushTyping(s, false);
        if (r.ctx.trigger == TriggerKind::PlayerMessage && now - s.lastOfflineNotice >= kOfflineNoticeSec)
        {
            s.lastOfflineNotice = now;
            std::string cname = s.rec.personality.name.empty() ? s.rec.companionName : s.rec.personality.name;
            std::string why = r.llm.status == LlmResult::Status::Timeout ? "the AI took too long to answer"
                            : r.llm.status == LlmResult::Status::Offline ? "the AI service is offline"
                            : r.llm.status == LlmResult::Status::ModelMissing ? "the configured model is not installed"
                            : "the AI returned an error";
            _out.push_back({ OutgoingAction::Type::Notice, s.rec.key, cname + " seems lost in thought right now (" + why + ").",
                             ChatChannel::Whisper, ActionType::None, Json() });
        }
        Json p = Json::MakeObject();
        p.Set("op", "error");
        p.Set("guid", int64_t(s.rec.key.companion));
        p.Set("code", LlmStatusId(r.llm.status));
        p.Set("message", LimitUtf8(r.llm.error, 200));
        _out.push_back({ OutgoingAction::Type::UiPush, s.rec.key, "", ChatChannel::Addon, ActionType::None, p });
    }

    std::string RomanceService::FallbackLine(Session const& s, ProposalDecision const& d) const
    {
        bool de = s.language == "de";
        std::vector<std::string> lines;
        switch (d.outcome)
        {
            case ProposalOutcome::Accept:
                lines = de ? std::vector<std::string>{ "Ja... ja, natürlich will ich.", "Ich habe so gehofft, dass du das fragst. Ja!",
                                                       "*lächelt* Ja. Von ganzem Herzen, ja." }
                           : std::vector<std::string>{ "Yes... yes, of course I will.", "I was hoping you'd ask. Yes!",
                                                       "*smiles* Yes. With all my heart, yes." };
                break;
            case ProposalOutcome::Defer:
                lines = de ? std::vector<std::string>{ "Mein Herz rast gerade... gibst du mir noch ein bisschen Zeit?",
                                                       "Das bedeutet mir viel... aber lass mir noch etwas Zeit, ja?" }
                           : std::vector<std::string>{ "My heart is racing... can you give me a little more time to think?",
                                                       "That means a lot to me... but give me a little more time, okay?" };
                break;
            default:
                lines = de ? std::vector<std::string>{ "Es tut mir leid... dafür bin ich noch nicht bereit.",
                                                       "Lass uns das langsam angehen, ja? Wir kennen uns noch nicht lange genug.",
                                                       "*schaut verlegen weg* Noch nicht... gib uns etwas Zeit." }
                           : std::vector<std::string>{ "I'm sorry... I'm not ready for that yet.",
                                                       "Let's take it slow, okay? We don't know each other well enough yet.",
                                                       "*looks away, a little flustered* Not yet... give us some time." };
                break;
        }
        std::string last;
        for (auto it = s.history.rbegin(); it != s.history.rend(); ++it)
            if (it->role == ChatTurn::Role::Companion)
            {
                last = it->text;
                break;
            }
        std::vector<std::string> fresh;
        for (auto const& l : lines)
            if (l != last)
                fresh.push_back(l);
        if (fresh.empty())
            fresh = lines;
        return fresh[std::uniform_int_distribution<size_t>(0, fresh.size() - 1)(const_cast<std::mt19937&>(_rng))];
    }

    void RomanceService::HandleReply(Session& s, JobResult& r, TimePoint now)
    {
        std::string cname = s.rec.personality.name.empty() ? s.rec.companionName : s.rec.personality.name;
        std::string pname = PlayerName(s);
        uint32_t maxChars = _cfg.maxReplyChars;
        if (s.rec.settings.maxReplyChars > 0)
            maxChars = std::min<uint32_t>(maxChars, uint32_t(s.rec.settings.maxReplyChars));
        CompanionReply reply = DialogueContextBuilder::ParseReply(r.llm.content, cname, maxChars);
        s.debug.lastRaw = LimitUtf8(r.llm.content, 600);
        s.debug.lastWarnings = reply.warnings;
        s.debug.lastError.clear();

        auto retry = [&](std::string const& note) -> bool
        {
            if (r.ctx.retries >= 1)
                return false;
            JobContext jc = r.ctx;
            ++jc.retries;
            if (!jc.messages.empty())
                jc.messages[0].content += "\n" + note;
            if (Submit(s, std::move(jc)))
                return true;
            return false;
        };

        if (!reply.valid)
        {
            if (retry("Your previous answer was not valid. Answer with the JSON object described above."))
                return;
            r.llm.status = LlmResult::Status::BadResponse;
            r.llm.error = "invalid reply from model";
            HandleFailure(s, r, now);
            return;
        }

        // ---- relationship proposals: the server decides, the reply must agree
        // Small models sometimes label unrelated messages as proposals (e.g. right after one).
        // Only take the model's detection seriously if the player's words are about the relationship status.
        if (!r.ctx.proposal && reply.proposalKind != ProposalKind::None &&
            !MessageClassifier::MentionsRelationshipStatus(ToLower(r.ctx.playerText)))
        {
            reply.warnings.push_back("ignored proposal label on a message that is not about the relationship");
            reply.proposalKind = ProposalKind::None;
            reply.proposalResponse = ProposalOutcome::None;
        }
        if (!r.ctx.proposal && r.ctx.trigger == TriggerKind::PlayerMessage && reply.proposalKind != ProposalKind::None)
        {
            ProposalKind kind = reply.proposalKind;
            if (kind == ProposalKind::Marriage && s.rec.relationship.stage < Stage::Engaged)
                kind = ProposalKind::Engagement;
            ProposalDecision d = _engine.EvaluateProposal(s.rec.relationship, kind, s.rec.personality, now);
            if (d.outcome != ProposalOutcome::None)
            {
                if (reply.proposalResponse != d.outcome && kind != ProposalKind::Breakup && d.reason != "already")
                {
                    JobContext jc = r.ctx;
                    jc.proposal = d;
                    if (r.ctx.retries < 1)
                    {
                        ++jc.retries;
                        jc.messages[0].content += DialogueContextBuilder::ProposalDirective(d, pname);
                        if (Submit(s, std::move(jc)))
                            return;
                    }
                    reply.reply = FallbackLine(s, d);
                }
                r.ctx.proposal = d;
            }
        }
        else if (r.ctx.proposal && r.ctx.proposal->kind != ProposalKind::Breakup && r.ctx.proposal->reason != "already")
        {
            bool matches = reply.structured && reply.proposalResponse == r.ctx.proposal->outcome;
            if (!matches)
            {
                if (retry("You MUST answer the relationship question as instructed: proposal.response must be \"" +
                          std::string(ProposalOutcomeId(r.ctx.proposal->outcome)) + "\" and your reply must say so."))
                    return;
                reply.reply = FallbackLine(s, *r.ctx.proposal);
            }
        }

        // ---- avoid verbatim repetition
        std::vector<std::string> recentLines;
        for (auto it = s.history.rbegin(); it != s.history.rend() && recentLines.size() < 6; ++it)
            if (it->role == ChatTurn::Role::Companion)
                recentLines.push_back(it->text);
        if (IsRepetitive(reply.reply, recentLines) &&
            retry("Your previous attempt repeated something you already said (\"" + LimitUtf8(reply.reply, 120) +
                  "\"). Say something new and different."))
            return;

        // ---- relationship values (only player messages carry social signals)
        bool leaving = false, reconciled = false;
        if (r.ctx.trigger == TriggerKind::PlayerMessage)
        {
            RelationshipDelta d = _engine.ApplyConversation(s.rec.relationship, reply.sentiment, reply.signals, s.rec.personality,
                                                            r.ctx.playerTextLen, now);
            leaving = d.leaving;
            reconciled = d.reconciled;
            if (d.stageChangedTo)
            {
                _repo.LogRelationshipEvent(s.rec.key, "stage_change",
                                           std::string(StageId(*d.stageChangedFrom)) + "->" + StageId(*d.stageChangedTo), now);
                if (*d.stageChangedTo > *d.stageChangedFrom)
                    StoreMemory(s, MemoryKind::Relationship, MemorySource::Observed, 70,
                                cname + " and " + pname + " grew closer and became " +
                                    ToLower(RelationshipEngine::StageLabel(*d.stageChangedTo, s.rec.personality)) + ".", now);
            }
            if (d.strainedChanged && s.rec.relationship.strained)
                StoreMemory(s, MemoryKind::Relationship, MemorySource::Interpretation, 55,
                            pname + " said something that hurt " + cname + ": \"" + LimitUtf8(r.ctx.playerText, 120) + "\"", now);
            else if (d.strainedChanged)
                StoreMemory(s, MemoryKind::Relationship, MemorySource::Interpretation, 50,
                            cname + " and " + pname + " made up after a difficult moment.", now);
        }
        if (reply.hasEmotion)
            _engine.SetMood(s.rec.relationship, reply.emotion, 50);
        s.dirty = true;

        // ---- memory candidates proposed by the model
        if (_cfg.memoryEnabled)
        {
            for (auto const& m : reply.memories)
            {
                MemorySource src = MemorySource::Interpretation;
                if (r.ctx.trigger == TriggerKind::PlayerMessage && m.about != "companion" && TokenOverlap(m.text, r.ctx.playerText) >= 0.25)
                    src = MemorySource::PlayerStated;
                StoreMemory(s, m.kind, src, m.importance, m.text, now);
            }
            if (!r.ctx.eventMemory.empty())
                StoreMemory(s, MemoryKind::Relationship, MemorySource::Observed, 85, r.ctx.eventMemory, now);
        }

        // ---- proposal commit happens after coherent reply text is settled
        bool ceremony = false;
        if (r.ctx.proposal)
        {
            Stage before = s.rec.relationship.stage;
            ApplyProposalOutcome(s, *r.ctx.proposal, now);
            ceremony = before == Stage::Engaged && s.rec.relationship.stage == Stage::Married;
        }

        // ---- validated actions (only in answer to the player)
        if (r.ctx.trigger == TriggerKind::PlayerMessage && r.ctx.allowActions && reply.action != ActionType::None)
        {
            Stage st = s.rec.relationship.stage;
            bool romanticOk = s.rec.relationship.romantic && s.rec.personality.romanceEnabled;
            if (IsEmoteAction(reply.action))
            {
                bool ok = true;
                if (reply.action == ActionType::EmoteKiss)
                    ok = romanticOk && st >= Stage::RomanticInterest;
                else if (reply.action == ActionType::EmoteHug)
                    ok = st >= Stage::Friends;
                if (ok)
                    _out.push_back({ OutgoingAction::Type::Emote, s.rec.key, "", r.ctx.channel, reply.action, Json() });
            }
            else if (IsMovementCommand(reply.action) && !r.ctx.executedCommand && LooksLikeRequest(r.ctx.playerText))
                _out.push_back({ OutgoingAction::Type::Command, s.rec.key, "", r.ctx.channel, reply.action, Json() });
        }

        Deliver(s, reply.reply, r.ctx.channel, now);
        if (leaving)
            Leave(s, now);
        else if (reconciled)
            Reconcile(s, now);
        if (r.ctx.trigger == TriggerKind::Proactive)
        {
            ++s.unansweredProactive;
            s.proactiveTimes.push_back(now);
        }
        PushTyping(s, false);
        PushRelationship(s);

        if (ceremony)
            RequestReaction(s, TriggerKind::GameEvent,
                            "You and " + pname + " have just been married" + (s.lastContext.zone.empty() ? "" : " here in " + s.lastContext.zone) +
                                ". Speak your wedding vow to " + pname + " in one or two heartfelt sentences.",
                            s.lastContext, now, "", false);
    }

    void RomanceService::ApplyProposalOutcome(Session& s, ProposalDecision const& d, TimePoint now)
    {
        std::string cname = s.rec.personality.name.empty() ? s.rec.companionName : s.rec.personality.name;
        std::string pname = PlayerName(s);
        std::string where = s.lastContext.zone.empty() ? "" : " in " + s.lastContext.zone;
        _repo.LogRelationshipEvent(s.rec.key, "proposal",
                                   std::string(ProposalKindId(d.kind)) + ":" + ProposalOutcomeId(d.outcome) + ":" + d.reason, now);
        if (d.outcome == ProposalOutcome::Accept && d.reason != "already")
        {
            RelationshipDelta delta = _engine.CommitProposal(s.rec.relationship, d.kind, now);
            if (delta.stageChangedTo)
            {
                std::string text;
                int importance = 95;
                switch (d.kind)
                {
                    case ProposalKind::Dating: text = cname + " and " + pname + " became a couple" + where + "."; break;
                    case ProposalKind::Engagement: text = pname + " asked " + cname + " to marry them and the answer was yes: they got engaged" + where + "."; break;
                    case ProposalKind::Marriage: text = cname + " and " + pname + " were married" + where + "."; break;
                    case ProposalKind::Breakup: text = pname + " ended the romantic relationship with " + cname + where + "."; importance = 90; break;
                    default: break;
                }
                if (!text.empty())
                    StoreMemory(s, MemoryKind::Relationship, MemorySource::Observed, importance, text, now);
                _repo.LogRelationshipEvent(s.rec.key, "stage_change",
                                           std::string(StageId(*delta.stageChangedFrom)) + "->" + StageId(*delta.stageChangedTo), now);
                ActionType emote = d.kind == ProposalKind::Breakup ? ActionType::EmoteCry
                                 : d.kind == ProposalKind::Dating ? ActionType::EmoteHug
                                 : ActionType::EmoteKiss;
                _out.push_back({ OutgoingAction::Type::Emote, s.rec.key, "", ChatChannel::Whisper, emote, Json() });
                Json p = Json::MakeObject();
                p.Set("op", "milestone");
                p.Set("guid", int64_t(s.rec.key.companion));
                p.Set("stage", StageId(*delta.stageChangedTo));
                p.Set("label", RelationshipEngine::StageLabel(*delta.stageChangedTo, s.rec.personality));
                _out.push_back({ OutgoingAction::Type::UiPush, s.rec.key, "", ChatChannel::Addon, ActionType::None, p });
            }
            s.dirty = true;
        }
        else if (d.outcome == ProposalOutcome::Defer || d.outcome == ProposalOutcome::Decline)
        {
            s.rec.relationship.lastProposalAt = now;
            static char const* const asks[] = { "", "to be a couple", "to get engaged", "to marry", "" };
            StoreMemory(s, MemoryKind::Relationship, MemorySource::Observed, 60,
                        pname + " asked " + cname + " " + asks[int(d.kind)] + (where.empty() ? "" : where) + "; " + cname +
                            (d.outcome == ProposalOutcome::Defer ? " asked for more time." : " said no for now."),
                        now);
            s.dirty = true;
        }
    }

    void RomanceService::HandleSummary(Session& s, JobResult& r, TimePoint now)
    {
        SummaryReply sum = DialogueContextBuilder::ParseSummary(r.llm.content);
        if (!sum.valid)
            return;
        StoreMemory(s, MemoryKind::Summary, MemorySource::ModelSummary, 45, sum.summary, now);
        std::string playerLines;
        for (auto const& t : r.ctx.summaryTurns)
            if (t.role == ChatTurn::Role::Player)
                playerLines += t.text + " ";
        for (auto const& f : sum.facts)
        {
            MemorySource src = (f.about != "companion" && TokenOverlap(f.text, playerLines) >= 0.3) ? MemorySource::PlayerStated
                                                                                                    : MemorySource::Interpretation;
            StoreMemory(s, f.kind, src, f.importance, f.text, now);
        }
    }

    void RomanceService::StoreMemory(Session& s, MemoryKind kind, MemorySource src, int importance, std::string const& text, TimePoint now)
    {
        if (!_cfg.memoryEnabled)
            return;
        auto res = s.memories.Add(kind, src, importance, text, now);
        if (!res.stored)
            return;
        _repo.SaveMemory(s.rec.key, res.entry);
        std::vector<uint32_t> removed = s.memories.Prune(now);
        if (!removed.empty())
            _repo.DeleteMemories(s.rec.key, removed);
        Json p = Json::MakeObject();
        p.Set("op", "memory_changed");
        p.Set("guid", int64_t(s.rec.key.companion));
        p.Set("id", int64_t(res.entry.id));
        _out.push_back({ OutgoingAction::Type::UiPush, s.rec.key, "", ChatChannel::Addon, ActionType::None, p });
    }

    void RomanceService::Leave(Session& s, TimePoint now)
    {
        std::string cname = s.rec.personality.name.empty() ? s.rec.companionName : s.rec.personality.name;
        std::string pname = PlayerName(s);
        bool again = s.rec.relationship.timesLeft > 0;
        TimePoint until = _engine.Estrange(s.rec.relationship, now);
        int hours = int((until - now + 1800) / 3600);

        static char const* const en[] = {
            "I can't do this anymore. I'm leaving. Don't come looking for me.",
            "That's enough. I won't let you treat me like this. Goodbye.",
            "I'm done. I need to get away from you.",
            "No. I'm not staying with someone who treats me like this. Farewell.",
        };
        static char const* const de[] = {
            "Ich kann das nicht mehr. Ich gehe. Such nicht nach mir.",
            "Es reicht. So lasse ich mich nicht behandeln. Leb wohl.",
            "Ich bin fertig mit dir. Ich brauche Abstand.",
        };
        std::string farewell = s.language == "de" ? de[std::uniform_int_distribution<size_t>(0, 2)(_rng)]
                                                  : en[std::uniform_int_distribution<size_t>(0, 3)(_rng)];
        if (again)
            farewell = s.language == "de" ? "Schon wieder? Nein. Diesmal bin ich wirklich weg." : "Again? No. This time I'm really gone.";
        Deliver(s, farewell, ChatChannel::Whisper, now);

        StoreMemory(s, MemoryKind::Relationship, MemorySource::Observed, 95,
                    cname + " left " + pname + (again ? " again" : "") + " after being hurt too many times.", now);
        _repo.LogRelationshipEvent(s.rec.key, "estranged", "for " + std::to_string(hours) + "h", now);
        _out.push_back({ OutgoingAction::Type::Notice, s.rec.key,
                         cname + " has left you. " + cname + " will not answer your calls for about " + std::to_string(hours) +
                             " hours, and winning " + cname + " back will take sincere apologies and kindness over several days.",
                         ChatChannel::Whisper, ActionType::None, Json() });
        _out.push_back({ OutgoingAction::Type::Command, s.rec.key, "", ChatChannel::Whisper, ActionType::Leave, Json() });
        s.dirty = true;
        SaveRecord(s, now, true);
        PushRelationship(s);
    }

    void RomanceService::Reconcile(Session& s, TimePoint now)
    {
        std::string cname = s.rec.personality.name.empty() ? s.rec.companionName : s.rec.personality.name;
        std::string pname = PlayerName(s);
        StoreMemory(s, MemoryKind::Relationship, MemorySource::Observed, 85,
                    cname + " forgave " + pname + " after a long estrangement; trust is fragile but returning.", now);
        _repo.LogRelationshipEvent(s.rec.key, "reconciled", "", now);
        _out.push_back({ OutgoingAction::Type::Notice, s.rec.key, cname + " has forgiven you. Be careful with that trust.",
                         ChatChannel::Whisper, ActionType::None, Json() });
        s.dirty = true;
        SaveRecord(s, now, true);
    }

    void RomanceService::Deliver(Session& s, std::string const& reply, ChatChannel channel, TimePoint now)
    {
        ChatChannel out = channel == ChatChannel::Addon ? ChatChannel::Whisper : channel;
        if (s.rec.settings.partyChatMode || _cfg.partyOnlyChat)
            out = ChatChannel::Party;
        ChatTurn t{ ChatTurn::Role::Companion, reply, now };
        s.history.push_back(t);
        while (s.history.size() > _cfg.storedChatTurns)
            s.history.pop_front();
        _repo.AppendChat(s.rec.key, t);
        ++s.rec.turnsSinceSummary;
        if (s.rec.turnsSinceSummary % 20 == 0)
            _repo.PruneChat(s.rec.key, _cfg.storedChatTurns);

        _out.push_back({ OutgoingAction::Type::Speak, s.rec.key, reply, out, ActionType::None, Json() });
        Json p = Json::MakeObject();
        p.Set("op", "chat_msg");
        p.Set("guid", int64_t(s.rec.key.companion));
        p.Set("from", "companion");
        p.Set("text", reply);
        p.Set("ts", int64_t(now));
        p.Set("mood", MoodId(s.rec.relationship.mood));
        _out.push_back({ OutgoingAction::Type::UiPush, s.rec.key, "", ChatChannel::Addon, ActionType::None, p });
        s.lastReplyAt = now;
        s.lastActivity = now;
        s.rec.relationship.lastInteraction = now;
    }

    void RomanceService::PushTyping(Session& s, bool on)
    {
        Json p = Json::MakeObject();
        p.Set("op", "typing");
        p.Set("guid", int64_t(s.rec.key.companion));
        p.Set("on", on && s.pending > 0 ? true : on);
        _out.push_back({ OutgoingAction::Type::UiPush, s.rec.key, "", ChatChannel::Addon, ActionType::None, p });
    }

    Json RomanceService::RelationshipJson(CompanionKey const& key, RelationshipState const& st, PersonalityProfile const& p) const
    {
        Json j = Json::MakeObject();
        j.Set("guid", int64_t(key.companion));
        j.Set("stage", StageId(st.stage));
        j.Set("stageIndex", int(st.stage));
        j.Set("stageLabel", RelationshipEngine::StageLabel(st.stage, p));
        j.Set("romantic", st.romantic && p.romanceEnabled);
        j.Set("values", ValuesJson(st.v));
        j.Set("mood", MoodId(st.mood));
        j.Set("moodIntensity", Round1(st.moodIntensity));
        j.Set("strained", st.strained);
        j.Set("estranged", st.estranged);
        j.Set("estrangedUntil", int64_t(st.estrangedUntil));
        j.Set("reconciliation", int64_t(st.reconciliation));
        j.Set("resentment", int64_t(st.resentment));
        j.Set("stageSince", int64_t(st.stageSince));
        j.Set("messages", int64_t(st.messageCount));
        Json labels = Json::MakeArray();
        for (int i = 0; i < kStageCount; ++i)
            labels.Push(RelationshipEngine::StageLabel(Stage(i), p));
        j.Set("labels", labels);
        return j;
    }

    void RomanceService::PushRelationship(Session& s)
    {
        Json p = RelationshipJson(s.rec.key, s.rec.relationship, s.rec.personality);
        p.Set("op", "relationship");
        _out.push_back({ OutgoingAction::Type::UiPush, s.rec.key, "", ChatChannel::Addon, ActionType::None, p });
    }

    void RomanceService::MaybeFireReaction(Session& s, WorldContext const& ctx, TimePoint now)
    {
        if (!s.reaction)
            return;
        if (now > s.reaction->expires)
        {
            s.reaction.reset();
            return;
        }
        if (now < s.reaction->notBefore || s.pending > 0 || ctx.BusyForChat(false) || now - s.lastReplyAt < 3)
            return;
        PendingReaction pr = *s.reaction;
        s.reaction.reset();
        TriggerKind trig = pr.priority >= 4 ? TriggerKind::Gift : TriggerKind::GameEvent;
        RequestReaction(s, trig, pr.event.detail, ctx, now, "", pr.priority < 3);
    }

    void RomanceService::ScheduleNextProactive(Session& s, TimePoint now)
    {
        double freq = s.rec.settings.proactiveFrequency / 100.0;
        double minI = double(_cfg.proactiveMinIntervalSec), maxI = double(std::max(_cfg.proactiveMaxIntervalSec, _cfg.proactiveMinIntervalSec));
        double base = maxI - (maxI - minI) * freq;
        double initiative = 1.4 - (s.rec.personality.initiative / 100.0) * 0.8;
        double backoff = std::pow(2.0, double(std::min<uint32_t>(s.unansweredProactive, 3)));
        double jitter = std::uniform_real_distribution<double>(0.75, 1.25)(_rng);
        // Several companions share the conversation instead of all chattering at the same rate.
        double share = double(std::max<size_t>(1, ActiveForOwner(s.rec.key.owner).size()));
        s.nextProactiveAt = now + int64_t(base * initiative * backoff * jitter * share);
    }

    void RomanceService::MaybeProactive(Session& s, WorldContext const& ctx, TimePoint now)
    {
        if (!_cfg.proactiveEnabled || !s.rec.settings.proactiveEnabled || !s.rec.settings.aiEnabled || !_cfg.ollamaEnabled)
            return;
        if (s.rec.relationship.estranged)
            return;   // cold after an estrangement: she does not start conversations
        if (now < s.nextProactiveAt)
            return;
        if (s.pending > 0 || s.reaction || ctx.BusyForChat(_cfg.blockChatInInstances) || !ctx.sameGroup || ctx.playerAfk)
        {
            s.nextProactiveAt = now + 30;
            return;
        }
        if (now - s.lastActivity < 60 || s.unansweredProactive >= 2 || (_status.checked && !_status.online))
        {
            ScheduleNextProactive(s, now);
            return;
        }
        while (!s.proactiveTimes.empty() && now - s.proactiveTimes.front() > 3600)
            s.proactiveTimes.pop_front();
        if (s.proactiveTimes.size() >= _cfg.proactiveMaxPerHour)
        {
            ScheduleNextProactive(s, now);
            return;
        }
        std::uniform_real_distribution<double> u(0.0, 1.0);
        double chance = _cfg.proactiveChance * (0.5 + s.rec.settings.proactiveFrequency / 100.0);
        if (u(_rng) > chance)
        {
            ScheduleNextProactive(s, now);
            return;
        }

        std::string pname = PlayerName(s);
        RelationshipState const& rel = s.rec.relationship;
        PersonalityProfile const& p = s.rec.personality;
        struct Topic { double weight; std::string text; };
        std::vector<Topic> topics;
        if (rel.lastPlayerMessage > 0 && now - rel.lastPlayerMessage >= int64_t(_cfg.idleCheckInSec) && s.unansweredProactive == 0)
            topics.push_back({ 3.0, pname + " has been silent for a while (last words " +
                                        DialogueContextBuilder::RelativeTime(rel.lastPlayerMessage, now) +
                                        "). Gently check whether they are still there, without sounding needy." });
        for (auto it = s.recentGifts.rbegin(); it != s.recentGifts.rend(); ++it)
            if (now - it->at < 12 * 3600 && now - it->at > 600 && it->score >= 35)
            {
                for (auto const& m : s.memories.All())
                    if (m.kind == MemoryKind::Gift && std::abs(m.createdAt - it->at) < 5)
                    {
                        topics.push_back({ 1.5, "You find yourself thinking about this: " + m.text + " Mention it naturally." });
                        break;
                    }
                break;
            }
        {
            std::vector<MemoryEntry const*> old;
            for (auto const& m : s.memories.All())
                if ((m.kind == MemoryKind::Episode || m.kind == MemoryKind::Relationship || m.kind == MemoryKind::Promise) &&
                    m.importance >= 50 && now - m.createdAt > 86400)
                    old.push_back(&m);
            if (!old.empty())
            {
                MemoryEntry const* m = old[std::uniform_int_distribution<size_t>(0, old.size() - 1)(_rng)];
                topics.push_back({ 1.0, "A memory comes to mind (" + DialogueContextBuilder::RelativeTime(m->createdAt, now) + "): " +
                                            m->text + " Bring it up naturally if it fits." });
            }
        }
        if (!ctx.zone.empty())
            topics.push_back({ 2.0, "You are traveling together quietly through " + ctx.zone +
                                        (ctx.timeOfDay.empty() ? "" : " in the " + ctx.timeOfDay) +
                                        ". Make a small, natural remark about the journey, the place or the company." });
        if (rel.mood == Mood::Bored || rel.mood == Mood::Lonely || rel.mood == Mood::Curious)
            topics.push_back({ 1.0, std::string("You feel ") + MoodId(rel.mood) + "." });
        if (rel.romantic && p.romanceEnabled && rel.stage >= Stage::Dating && rel.v.affection >= 70)
            topics.push_back({ 1.2 * _cfg.romanticInitiative * (p.affectionLevel / 60.0),
                               "You feel affectionate toward " + pname + " and want to say something warm, subtle rather than over the top." });
        if (topics.empty())
        {
            ScheduleNextProactive(s, now);
            return;
        }
        double total = 0;
        for (auto const& t : topics)
            total += std::max(0.0, t.weight);
        double pick = u(_rng) * total;
        std::string chosen = topics.back().text;
        for (auto const& t : topics)
        {
            pick -= std::max(0.0, t.weight);
            if (pick <= 0)
            {
                chosen = t.text;
                break;
            }
        }
        RequestReaction(s, TriggerKind::Proactive, chosen, ctx, now, "", true);
        ScheduleNextProactive(s, now);
    }

    void RomanceService::MaybeSummarize(Session& s, TimePoint now)
    {
        if (!_cfg.memoryEnabled || s.summarizing || s.pending > 0 || !s.rec.settings.aiEnabled || !_cfg.ollamaEnabled)
            return;
        if (_cfg.summarizeEveryTurns == 0 || s.rec.turnsSinceSummary < _cfg.summarizeEveryTurns)
            return;
        if (_status.checked && !_status.online)
            return;
        size_t n = std::min<size_t>({ s.history.size(), size_t(s.rec.turnsSinceSummary), 40 });
        if (n < 4)
        {
            s.rec.turnsSinceSummary = 0;
            return;
        }
        JobContext jc;
        jc.summary = true;
        jc.summaryTurns.assign(s.history.end() - long(n), s.history.end());
        std::string cname = s.rec.personality.name.empty() ? s.rec.companionName : s.rec.personality.name;
        jc.messages = DialogueContextBuilder::BuildSummary(jc.summaryTurns, cname, PlayerName(s), s.language);
        if (Submit(s, std::move(jc)))
        {
            s.summarizing = true;
            s.rec.turnsSinceSummary = 0;
            s.dirty = true;
        }
        (void)now;
    }

    // ================================================================= memory management

    std::vector<MemoryEntry> RomanceService::ListMemories(CompanionKey const& key, std::optional<MemoryKind> kind, std::string const& search,
                                                          size_t offset, size_t limit, size_t* total)
    {
        if (Session* s = Find(key))
            return s->memories.Filter(kind, search, offset, limit, total);
        MemoryStore store(_cfg);
        store.Load(_repo.LoadMemories(key));
        return store.Filter(kind, search, offset, limit, total);
    }

    void RomanceService::PushMemoryChanged(CompanionKey const& key, int64_t id)
    {
        Json p = Json::MakeObject();
        p.Set("op", "memory_changed");
        p.Set("guid", int64_t(key.companion));
        if (id >= 0)
            p.Set("id", id);
        _out.push_back({ OutgoingAction::Type::UiPush, key, "", ChatChannel::Addon, ActionType::None, p });
    }

    bool RomanceService::DeleteMemory(CompanionKey const& key, uint32_t id)
    {
        Session* s = Find(key);
        if (s)
        {
            if (!s->memories.Remove(id))
                return false;
        }
        else
        {
            MemoryStore store(_cfg);
            store.Load(_repo.LoadMemories(key));
            if (!store.Find(id))
                return false;
        }
        _repo.DeleteMemories(key, { id });
        PushMemoryChanged(key, -1);
        return true;
    }

    std::optional<MemoryEntry> RomanceService::EditMemory(CompanionKey const& key, uint32_t id, std::string const& text,
                                                          std::optional<int> importance, TimePoint now)
    {
        std::optional<MemoryEntry> m;
        if (Session* s = Find(key))
            m = s->memories.Edit(id, text, importance, now);
        else
        {
            MemoryStore store(_cfg);
            store.Load(_repo.LoadMemories(key));
            m = store.Edit(id, text, importance, now);
        }
        if (m)
        {
            _repo.SaveMemory(key, *m);
            PushMemoryChanged(key, m->id);
        }
        return m;
    }

    std::optional<MemoryEntry> RomanceService::AddUserMemory(CompanionKey const& key, MemoryKind kind, std::string const& text, int importance, TimePoint now)
    {
        if (kind == MemoryKind::Summary)
            kind = MemoryKind::Fact;
        MemoryStore::AddResult res;
        if (Session* s = Find(key))
            res = s->memories.Add(kind, MemorySource::UserEdited, importance, text, now);
        else
        {
            MemoryStore store(_cfg);
            store.Load(_repo.LoadMemories(key));
            res = store.Add(kind, MemorySource::UserEdited, importance, text, now);
        }
        if (!res.stored)
            return std::nullopt;
        _repo.SaveMemory(key, res.entry);
        PushMemoryChanged(key, res.entry.id);
        return res.entry;
    }

    std::optional<MemoryEntry> RomanceService::PinMemory(CompanionKey const& key, uint32_t id, bool pinned)
    {
        std::optional<MemoryEntry> m;
        if (Session* s = Find(key))
            m = s->memories.SetPinned(id, pinned);
        else
        {
            MemoryStore store(_cfg);
            store.Load(_repo.LoadMemories(key));
            m = store.SetPinned(id, pinned);
        }
        if (m)
        {
            _repo.SaveMemory(key, *m);
            PushMemoryChanged(key, m->id);
        }
        return m;
    }

    size_t RomanceService::ClearMemories(CompanionKey const& key, std::optional<MemoryKind> kind)
    {
        std::vector<uint32_t> removed;
        if (Session* s = Find(key))
            removed = s->memories.RemoveKind(kind);
        else
        {
            MemoryStore store(_cfg);
            store.Load(_repo.LoadMemories(key));
            removed = store.RemoveKind(kind);
        }
        if (!kind)
            _repo.DeleteAllMemories(key);
        else if (!removed.empty())
            _repo.DeleteMemories(key, removed);
        PushMemoryChanged(key, -1);
        return removed.size();
    }

    void RomanceService::ClearChatHistory(CompanionKey const& key)
    {
        if (Session* s = Find(key))
        {
            s->history.clear();
            s->rec.turnsSinceSummary = 0;
        }
        _repo.ClearChat(key);
    }

    bool RomanceService::ResetCompanion(CompanionKey const& key, TimePoint now)
    {
        if (!_cfg.allowFullReset)
            return false;
        Session* s = Find(key);
        std::string name;
        uint32_t account = 0;
        if (s)
        {
            for (auto& t : s->tokens)
                t->store(true);
            s->tokens.clear();
            s->pending = 0;
            ++s->generation;
            name = s->rec.companionName;
            account = s->rec.ownerAccount;
        }
        else if (auto rec = _repo.LoadCompanion(key))
        {
            name = rec->companionName;
            account = rec->ownerAccount;
        }
        _repo.DeleteCompanion(key);
        _repo.DeleteAllMemories(key);
        _repo.ClearChat(key);
        _repo.LogRelationshipEvent(key, "reset", "full reset by owner", now);
        PushMemoryChanged(key, -1);
        if (s)
        {
            s->rec = LoadOrDefault(key, account, name, now);
            s->memories.Clear();
            s->history.clear();
            s->recentEvents.clear();
            s->reaction.reset();
            s->dirty = true;
            SaveRecord(*s, now, true);
            PushRelationship(*s);
        }
        return true;
    }

    std::vector<ChatTurn> RomanceService::RecentChat(CompanionKey const& key, size_t limit)
    {
        if (Session* s = Find(key))
        {
            size_t n = std::min(limit, s->history.size());
            return std::vector<ChatTurn>(s->history.end() - long(n), s->history.end());
        }
        return _repo.LoadRecentChat(key, uint32_t(limit));
    }

    Json RomanceService::ExportCompanion(CompanionKey const& key, uint32_t ownerAccount)
    {
        Json j = Json::MakeObject();
        if (!_cfg.allowMemoryExport)
        {
            j.Set("error", "export_disabled");
            return j;
        }
        CompanionRecord rec = GetOrCreateRecord(key, ownerAccount, "", 0);
        j.Set("format", "mod-romance-export");
        j.Set("version", 1);
        j.Set("companion", rec.companionName);
        j.Set("personality", rec.personality.ToJson());
        j.Set("settings", rec.settings.ToJson());
        j.Set("relationship", RelationshipJson(key, rec.relationship, rec.personality));
        Json mems = Json::MakeArray();
        size_t total = 0;
        for (auto const& m : ListMemories(key, std::nullopt, "", 0, 100000, &total))
            mems.Push(m.ToJson());
        j.Set("memories", mems);
        return j;
    }

    RomanceService::SaveResult RomanceService::ImportCompanion(CompanionKey const& key, uint32_t ownerAccount, std::string const& companionName,
                                                               Json const& data, TimePoint now)
    {
        SaveResult r;
        if (data.GetString("format") != "mod-romance-export")
        {
            r.error = "not_a_romance_export";
            return r;
        }
        if (data["personality"].IsObject())
        {
            SaveResult p = SavePersonality(key, ownerAccount, companionName, PersonalityProfile::FromJson(data["personality"]), now);
            r.notes.insert(r.notes.end(), p.notes.begin(), p.notes.end());
        }
        if (data["settings"].IsObject())
            SaveSettings(key, ownerAccount, companionName, CompanionSettings::FromJson(data["settings"]), now);
        size_t imported = 0;
        for (Json const& mj : data["memories"].AsArray())
        {
            if (imported >= _cfg.maxMemoriesPerCompanion)
                break;
            auto m = MemoryEntry::FromJson(mj);
            if (!m || m->kind == MemoryKind::Summary)
                continue;
            // Imported memories are the player's own statements now; they cannot claim to be server observations.
            if (AddUserMemory(key, m->kind, m->text, std::min(m->importance, 80), now))
                ++imported;
        }
        r.notes.push_back(std::to_string(imported) + " memories imported");
        r.notes.push_back("relationship values are server-authoritative and were not imported");
        r.ok = true;
        return r;
    }

    // ================================================================= status

    size_t RomanceService::PendingRequests() const { return _pool ? _pool->Queued() + _pool->Busy() : 0; }

    SessionDebug const* RomanceService::Debug(CompanionKey const& key) const
    {
        Session const* s = Find(key);
        return s ? &s->debug : nullptr;
    }

    Json RomanceService::StatusJson(CompanionKey const* key) const
    {
        Json j = Json::MakeObject();
        j.Set("enabled", _cfg.enabled);
        j.Set("ollamaEnabled", _cfg.ollamaEnabled);
        j.Set("ollama", _status.ToJson());
        j.Set("model", _cfg.model);
        Json q = Json::MakeObject();
        q.Set("queued", int64_t(_pool ? _pool->Queued() : 0));
        q.Set("busy", int64_t(_pool ? _pool->Busy() : 0));
        q.Set("capacity", int64_t(_cfg.maxQueuedRequests));
        j.Set("queue", q);
        j.Set("activeCompanions", int64_t(_sessions.size()));
        if (key)
            if (Session const* s = Find(*key))
            {
                j.Set("guid", int64_t(key->companion));
                j.Set("pending", int64_t(s->pending));
                j.Set("language", s->language);
                j.Set("memories", int64_t(s->memories.Size()));
            }
        return j;
    }

    Json RomanceService::CompanionJson(CompanionRecord const& rec) const
    {
        Json j = Json::MakeObject();
        j.Set("guid", int64_t(rec.key.companion));
        j.Set("name", rec.companionName);
        j.Set("configured", rec.configured);
        j.Set("personality", rec.personality.ToJson());
        j.Set("settings", rec.settings.ToJson());
        j.Set("relationship", RelationshipJson(rec.key, rec.relationship, rec.personality));
        j.Set("active", IsActive(rec.key));
        return j;
    }
}
