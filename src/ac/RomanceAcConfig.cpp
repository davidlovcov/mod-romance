/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceAcConfig.h"
#include "RomanceTypes.h"

#include "Config.h"
#include "Define.h"

#include <sstream>

namespace romance::ac
{
    namespace
    {
        std::vector<std::string> SplitList(std::string const& s)
        {
            std::vector<std::string> out;
            std::stringstream ss(s);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                item = Trim(item);
                if (!item.empty())
                    out.push_back(item);
            }
            return out;
        }

        template <typename T>
        T Opt(char const* key, T def) { return sConfigMgr->GetOption<T>(key, def); }
    }

    RomanceConfig LoadConfigFromWorldserver()
    {
        RomanceConfig c;
        c.enabled = Opt<bool>("Romance.Enable", c.enabled);
        c.debugLogging = Opt<bool>("Romance.Debug", c.debugLogging);
        c.maxActiveCompanionsPerPlayer = Opt<uint32>("Romance.MaxActiveCompanionsPerPlayer", c.maxActiveCompanionsPerPlayer);
        c.maxAiCompanionsServer = Opt<uint32>("Romance.MaxAiCompanionsServer", c.maxAiCompanionsServer);
        c.allowLinkedAccounts = Opt<bool>("Romance.AllowLinkedAccounts", c.allowLinkedAccounts);
        c.allowInitialStageSelection = Opt<bool>("Romance.AllowInitialStageSelection", c.allowInitialStageSelection);
        c.maxInitialStage = std::min<uint32>(7, Opt<uint32>("Romance.MaxInitialStage", c.maxInitialStage));

        c.ollamaEnabled = Opt<bool>("Romance.Ollama.Enable", c.ollamaEnabled);
        c.ollamaUrl = Opt<std::string>("Romance.Ollama.Url", c.ollamaUrl);
        c.model = Opt<std::string>("Romance.Ollama.Model", c.model);
        c.fallbackModel = Opt<std::string>("Romance.Ollama.FallbackModel", c.fallbackModel);
        c.allowedModels = SplitList(Opt<std::string>("Romance.Ollama.AllowedModels", ""));
        c.allowPlayerModelChoice = Opt<bool>("Romance.Ollama.AllowPlayerModelChoice", c.allowPlayerModelChoice);
        c.temperature = Opt<float>("Romance.Ollama.Temperature", float(c.temperature));
        c.minTemperature = Opt<float>("Romance.Ollama.MinTemperature", float(c.minTemperature));
        c.maxTemperature = Opt<float>("Romance.Ollama.MaxTemperature", float(c.maxTemperature));
        c.contextLength = Opt<uint32>("Romance.Ollama.ContextLength", c.contextLength);
        c.maxResponseTokens = Opt<uint32>("Romance.Ollama.MaxResponseTokens", c.maxResponseTokens);
        c.maxReplyChars = Opt<uint32>("Romance.Ollama.MaxReplyChars", c.maxReplyChars);
        c.thinking = Opt<bool>("Romance.Ollama.Thinking", c.thinking);
        c.structuredOutput = Opt<bool>("Romance.Ollama.StructuredOutput", c.structuredOutput);
        c.requestTimeoutMs = Opt<uint32>("Romance.Ollama.RequestTimeoutMs", c.requestTimeoutMs);
        c.connectTimeoutMs = Opt<uint32>("Romance.Ollama.ConnectTimeoutMs", c.connectTimeoutMs);
        c.maxConcurrentRequests = std::max<uint32>(1, Opt<uint32>("Romance.Ollama.MaxConcurrentRequests", c.maxConcurrentRequests));
        c.maxQueuedRequests = std::max<uint32>(1, Opt<uint32>("Romance.Ollama.MaxQueuedRequests", c.maxQueuedRequests));
        c.maxPendingPerCompanion = std::max<uint32>(1, Opt<uint32>("Romance.Ollama.MaxPendingPerCompanion", c.maxPendingPerCompanion));
        c.minResponseCooldownMs = Opt<uint32>("Romance.Ollama.MinResponseCooldownMs", c.minResponseCooldownMs);
        c.healthCheckIntervalSec = Opt<uint32>("Romance.Ollama.HealthCheckIntervalSec", c.healthCheckIntervalSec);
        c.keepAlive = Opt<std::string>("Romance.Ollama.KeepAlive", c.keepAlive);

        c.historyTurns = Opt<uint32>("Romance.Chat.HistoryTurns", c.historyTurns);
        c.storedChatTurns = std::max<uint32>(c.historyTurns, Opt<uint32>("Romance.Chat.StoredTurns", c.storedChatTurns));
        c.maxPlayerMessageChars = Opt<uint32>("Romance.Chat.MaxPlayerMessageChars", c.maxPlayerMessageChars);
        c.maxProfileFieldChars = Opt<uint32>("Romance.Chat.MaxProfileFieldChars", c.maxProfileFieldChars);
        c.maxCustomPresets = Opt<uint32>("Romance.Presets.MaxCustomPerAccount", c.maxCustomPresets);
        c.controlEnabled = Opt<bool>("Romance.Control.Enable", c.controlEnabled);
        c.estrangementEnabled = Opt<bool>("Romance.Estrangement.Enable", c.estrangementEnabled);
        c.estrangeResentment = std::max(20.0f, std::min(100.0f, Opt<float>("Romance.Estrangement.Resentment", float(c.estrangeResentment))));
        c.estrangeCooldownHours = std::max<uint32>(1, Opt<uint32>("Romance.Estrangement.CooldownHours", c.estrangeCooldownHours));
        c.reconcilePerDay = std::max(1.0f, std::min(100.0f, Opt<float>("Romance.Estrangement.ReconcilePerDay", float(c.reconcilePerDay))));
        c.giveMinStage = std::min<uint32>(7, Opt<uint32>("Romance.Control.GiveMinStage", c.giveMinStage));
        c.maxGoldPerRequest = uint64_t(Opt<uint32>("Romance.Control.MaxGoldPerRequest", uint32(c.maxGoldPerRequest / 10000))) * 10000;
        c.autoSellJunk = Opt<bool>("Romance.Control.AutoSellJunk", c.autoSellJunk);
        c.autoSellFreeSlots = Opt<uint32>("Romance.Control.AutoSellFreeSlots", c.autoSellFreeSlots);
        c.leadMode = Opt<bool>("Romance.Control.LeadMode", c.leadMode);
        c.commandRange = std::max(5.0f, std::min(100.0f, Opt<float>("Romance.Control.CommandRange", c.commandRange)));
        c.interactRange = std::max(5.0f, std::min(60.0f, Opt<float>("Romance.Control.InteractRange", c.interactRange)));
        c.questSync = Opt<bool>("Romance.Control.QuestSync", c.questSync);
        c.partyAllAnswer = Opt<std::string>("Romance.Party.UnaddressedReplies", c.partyAllAnswer ? "all" : "last") != "last";
        c.maxInstructionChars = Opt<uint32>("Romance.Chat.MaxInstructionChars", c.maxInstructionChars);
        c.partyOnlyChat = Opt<bool>("Romance.Chat.PartyOnly", c.partyOnlyChat);
        c.allowedLanguages = Opt<std::string>("Romance.Chat.Language", c.allowedLanguages);
        c.naturalLanguageCommands = Opt<bool>("Romance.Chat.NaturalLanguageCommands", c.naturalLanguageCommands);
        c.gameplayContext = Opt<bool>("Romance.Chat.GameplayContext", c.gameplayContext);
        c.blockChatInInstances = Opt<bool>("Romance.Chat.QuietInInstances", c.blockChatInInstances);

        c.proactiveEnabled = Opt<bool>("Romance.Proactive.Enable", c.proactiveEnabled);
        c.proactiveMinIntervalSec = Opt<uint32>("Romance.Proactive.MinIntervalSec", c.proactiveMinIntervalSec);
        c.proactiveMaxIntervalSec = Opt<uint32>("Romance.Proactive.MaxIntervalSec", c.proactiveMaxIntervalSec);
        c.proactiveChance = Opt<float>("Romance.Proactive.Chance", float(c.proactiveChance));
        c.proactiveMaxPerHour = Opt<uint32>("Romance.Proactive.MaxPerHour", c.proactiveMaxPerHour);
        c.idleCheckInSec = Opt<uint32>("Romance.Proactive.IdleCheckInSec", c.idleCheckInSec);
        c.eventReactionCooldownSec = Opt<uint32>("Romance.Proactive.EventReactionCooldownSec", c.eventReactionCooldownSec);
        c.eventReactionChance = Opt<float>("Romance.Proactive.EventReactionChance", float(c.eventReactionChance));

        c.memoryEnabled = Opt<bool>("Romance.Memory.Enable", c.memoryEnabled);
        c.maxMemoriesPerCompanion = Opt<uint32>("Romance.Memory.MaxPerCompanion", c.maxMemoriesPerCompanion);
        c.memoryRetrievalCount = Opt<uint32>("Romance.Memory.RetrievalCount", c.memoryRetrievalCount);
        c.memoryRetrievalChars = Opt<uint32>("Romance.Memory.RetrievalChars", c.memoryRetrievalChars);
        c.summarizeEveryTurns = Opt<uint32>("Romance.Memory.SummarizeEveryTurns", c.summarizeEveryTurns);
        c.memoryRetentionDays = Opt<uint32>("Romance.Memory.RetentionDays", c.memoryRetentionDays);
        c.dedupeThreshold = Opt<float>("Romance.Memory.DedupeThreshold", float(c.dedupeThreshold));

        c.progressionSpeed = Opt<float>("Romance.Relationship.ProgressionSpeed", float(c.progressionSpeed));
        c.affectionGainMultiplier = Opt<float>("Romance.Relationship.GainMultiplier", float(c.affectionGainMultiplier));
        c.affectionLossMultiplier = Opt<float>("Romance.Relationship.LossMultiplier", float(c.affectionLossMultiplier));
        c.moodSensitivity = Opt<float>("Romance.Relationship.MoodSensitivity", float(c.moodSensitivity));
        c.romanticInitiative = Opt<float>("Romance.Relationship.RomanticInitiative", float(c.romanticInitiative));
        c.dailyGainCap = Opt<float>("Romance.Relationship.DailyGainCap", float(c.dailyGainCap));
        c.minDaysDatingBeforeEngagement = Opt<uint32>("Romance.Relationship.MinDaysDatingBeforeEngagement", c.minDaysDatingBeforeEngagement);
        c.minDaysEngagedBeforeMarriage = Opt<uint32>("Romance.Relationship.MinDaysEngagedBeforeMarriage", c.minDaysEngagedBeforeMarriage);
        c.requireRingForEngagement = Opt<bool>("Romance.Relationship.RequireRingForEngagement", c.requireRingForEngagement);
        c.neglectDaysBeforeDecay = Opt<float>("Romance.Relationship.NeglectDaysBeforeDecay", float(c.neglectDaysBeforeDecay));

        c.giftMultiplier = Opt<float>("Romance.Gift.Multiplier", float(c.giftMultiplier));
        c.jewelryMultiplier = Opt<float>("Romance.Gift.JewelryMultiplier", float(c.jewelryMultiplier));
        c.giftDailyCap = Opt<float>("Romance.Gift.DailyCap", float(c.giftDailyCap));
        for (auto const& e : SplitList(Opt<std::string>("Romance.Gift.RomanticItemEntries", "")))
            if (uint32 v = uint32(std::strtoul(e.c_str(), nullptr, 10)))
                c.romanticItemEntries.push_back(v);
        std::string kw = Opt<std::string>("Romance.Gift.RomanticKeywords", "");
        if (!kw.empty())
        {
            c.romanticItemKeywords.clear();
            for (auto const& k : SplitList(kw))
                c.romanticItemKeywords.push_back(ToLower(k));
        }

        c.allowMemoryExport = Opt<bool>("Romance.Privacy.AllowExport", c.allowMemoryExport);
        c.allowFullReset = Opt<bool>("Romance.Privacy.AllowFullReset", c.allowFullReset);
        c.debugTabForPlayers = Opt<bool>("Romance.Privacy.DebugTabForPlayers", c.debugTabForPlayers);
        return c;
    }
}
