/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionAcConfig.h"
#include "TrueCompanionTypes.h"

#include "Config.h"
#include "Define.h"

#include <sstream>

namespace truecompanion::ac
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

    TrueCompanionConfig LoadConfigFromWorldserver()
    {
        TrueCompanionConfig c;
        c.enabled = Opt<bool>("TrueCompanion.Enable", c.enabled);
        c.debugLogging = Opt<bool>("TrueCompanion.Debug", c.debugLogging);
        c.maxActiveCompanionsPerPlayer = Opt<uint32>("TrueCompanion.MaxActiveCompanionsPerPlayer", c.maxActiveCompanionsPerPlayer);
        c.maxAiCompanionsServer = Opt<uint32>("TrueCompanion.MaxAiCompanionsServer", c.maxAiCompanionsServer);
        c.allowLinkedAccounts = Opt<bool>("TrueCompanion.AllowLinkedAccounts", c.allowLinkedAccounts);
        c.allowInitialStageSelection = Opt<bool>("TrueCompanion.AllowInitialStageSelection", c.allowInitialStageSelection);
        c.maxInitialStage = std::min<uint32>(7, Opt<uint32>("TrueCompanion.MaxInitialStage", c.maxInitialStage));

        c.ollamaEnabled = Opt<bool>("TrueCompanion.Ollama.Enable", c.ollamaEnabled);
        c.ollamaUrl = Opt<std::string>("TrueCompanion.Ollama.Url", c.ollamaUrl);
        c.model = Opt<std::string>("TrueCompanion.Ollama.Model", c.model);
        c.fallbackModel = Opt<std::string>("TrueCompanion.Ollama.FallbackModel", c.fallbackModel);
        c.allowedModels = SplitList(Opt<std::string>("TrueCompanion.Ollama.AllowedModels", ""));
        c.allowPlayerModelChoice = Opt<bool>("TrueCompanion.Ollama.AllowPlayerModelChoice", c.allowPlayerModelChoice);
        c.temperature = Opt<float>("TrueCompanion.Ollama.Temperature", float(c.temperature));
        c.minTemperature = Opt<float>("TrueCompanion.Ollama.MinTemperature", float(c.minTemperature));
        c.maxTemperature = Opt<float>("TrueCompanion.Ollama.MaxTemperature", float(c.maxTemperature));
        c.contextLength = Opt<uint32>("TrueCompanion.Ollama.ContextLength", c.contextLength);
        c.maxResponseTokens = Opt<uint32>("TrueCompanion.Ollama.MaxResponseTokens", c.maxResponseTokens);
        c.maxReplyChars = Opt<uint32>("TrueCompanion.Ollama.MaxReplyChars", c.maxReplyChars);
        c.thinking = Opt<bool>("TrueCompanion.Ollama.Thinking", c.thinking);
        c.structuredOutput = Opt<bool>("TrueCompanion.Ollama.StructuredOutput", c.structuredOutput);
        c.requestTimeoutMs = Opt<uint32>("TrueCompanion.Ollama.RequestTimeoutMs", c.requestTimeoutMs);
        c.connectTimeoutMs = Opt<uint32>("TrueCompanion.Ollama.ConnectTimeoutMs", c.connectTimeoutMs);
        c.maxConcurrentRequests = std::max<uint32>(1, Opt<uint32>("TrueCompanion.Ollama.MaxConcurrentRequests", c.maxConcurrentRequests));
        c.maxQueuedRequests = std::max<uint32>(1, Opt<uint32>("TrueCompanion.Ollama.MaxQueuedRequests", c.maxQueuedRequests));
        c.maxPendingPerCompanion = std::max<uint32>(1, Opt<uint32>("TrueCompanion.Ollama.MaxPendingPerCompanion", c.maxPendingPerCompanion));
        c.minResponseCooldownMs = Opt<uint32>("TrueCompanion.Ollama.MinResponseCooldownMs", c.minResponseCooldownMs);
        c.healthCheckIntervalSec = Opt<uint32>("TrueCompanion.Ollama.HealthCheckIntervalSec", c.healthCheckIntervalSec);
        c.keepAlive = Opt<std::string>("TrueCompanion.Ollama.KeepAlive", c.keepAlive);

        c.historyTurns = Opt<uint32>("TrueCompanion.Chat.HistoryTurns", c.historyTurns);
        c.storedChatTurns = std::max<uint32>(c.historyTurns, Opt<uint32>("TrueCompanion.Chat.StoredTurns", c.storedChatTurns));
        c.maxPlayerMessageChars = Opt<uint32>("TrueCompanion.Chat.MaxPlayerMessageChars", c.maxPlayerMessageChars);
        c.maxProfileFieldChars = Opt<uint32>("TrueCompanion.Chat.MaxProfileFieldChars", c.maxProfileFieldChars);
        c.maxCustomPresets = Opt<uint32>("TrueCompanion.Presets.MaxCustomPerAccount", c.maxCustomPresets);
        c.controlEnabled = Opt<bool>("TrueCompanion.Control.Enable", c.controlEnabled);
        c.estrangementEnabled = Opt<bool>("TrueCompanion.Estrangement.Enable", c.estrangementEnabled);
        c.estrangeResentment = std::max(20.0f, std::min(100.0f, Opt<float>("TrueCompanion.Estrangement.Resentment", float(c.estrangeResentment))));
        c.estrangeCooldownHours = std::max<uint32>(1, Opt<uint32>("TrueCompanion.Estrangement.CooldownHours", c.estrangeCooldownHours));
        c.reconcilePerDay = std::max(1.0f, std::min(100.0f, Opt<float>("TrueCompanion.Estrangement.ReconcilePerDay", float(c.reconcilePerDay))));
        c.giveMinStage = std::min<uint32>(7, Opt<uint32>("TrueCompanion.Control.GiveMinStage", c.giveMinStage));
        c.maxGoldPerRequest = uint64_t(Opt<uint32>("TrueCompanion.Control.MaxGoldPerRequest", uint32(c.maxGoldPerRequest / 10000))) * 10000;
        c.autoSellJunk = Opt<bool>("TrueCompanion.Control.AutoSellJunk", c.autoSellJunk);
        c.autoSellFreeSlots = Opt<uint32>("TrueCompanion.Control.AutoSellFreeSlots", c.autoSellFreeSlots);
        c.leadMode = Opt<bool>("TrueCompanion.Control.LeadMode", c.leadMode);
        c.commandRange = std::max(5.0f, std::min(100.0f, Opt<float>("TrueCompanion.Control.CommandRange", c.commandRange)));
        c.interactRange = std::max(5.0f, std::min(60.0f, Opt<float>("TrueCompanion.Control.InteractRange", c.interactRange)));
        c.questSync = Opt<bool>("TrueCompanion.Control.QuestSync", c.questSync);
        c.partyAllAnswer = Opt<std::string>("TrueCompanion.Party.UnaddressedReplies", c.partyAllAnswer ? "all" : "last") != "last";
        c.maxInstructionChars = Opt<uint32>("TrueCompanion.Chat.MaxInstructionChars", c.maxInstructionChars);
        c.partyOnlyChat = Opt<bool>("TrueCompanion.Chat.PartyOnly", c.partyOnlyChat);
        c.allowedLanguages = Opt<std::string>("TrueCompanion.Chat.Language", c.allowedLanguages);
        c.naturalLanguageCommands = Opt<bool>("TrueCompanion.Chat.NaturalLanguageCommands", c.naturalLanguageCommands);
        c.gameplayContext = Opt<bool>("TrueCompanion.Chat.GameplayContext", c.gameplayContext);
        c.blockChatInInstances = Opt<bool>("TrueCompanion.Chat.QuietInInstances", c.blockChatInInstances);

        c.proactiveEnabled = Opt<bool>("TrueCompanion.Proactive.Enable", c.proactiveEnabled);
        c.proactiveMinIntervalSec = Opt<uint32>("TrueCompanion.Proactive.MinIntervalSec", c.proactiveMinIntervalSec);
        c.proactiveMaxIntervalSec = Opt<uint32>("TrueCompanion.Proactive.MaxIntervalSec", c.proactiveMaxIntervalSec);
        c.proactiveChance = Opt<float>("TrueCompanion.Proactive.Chance", float(c.proactiveChance));
        c.proactiveMaxPerHour = Opt<uint32>("TrueCompanion.Proactive.MaxPerHour", c.proactiveMaxPerHour);
        c.idleCheckInSec = Opt<uint32>("TrueCompanion.Proactive.IdleCheckInSec", c.idleCheckInSec);
        c.eventReactionCooldownSec = Opt<uint32>("TrueCompanion.Proactive.EventReactionCooldownSec", c.eventReactionCooldownSec);
        c.eventReactionChance = Opt<float>("TrueCompanion.Proactive.EventReactionChance", float(c.eventReactionChance));

        c.memoryEnabled = Opt<bool>("TrueCompanion.Memory.Enable", c.memoryEnabled);
        c.maxMemoriesPerCompanion = Opt<uint32>("TrueCompanion.Memory.MaxPerCompanion", c.maxMemoriesPerCompanion);
        c.memoryRetrievalCount = Opt<uint32>("TrueCompanion.Memory.RetrievalCount", c.memoryRetrievalCount);
        c.memoryRetrievalChars = Opt<uint32>("TrueCompanion.Memory.RetrievalChars", c.memoryRetrievalChars);
        c.summarizeEveryTurns = Opt<uint32>("TrueCompanion.Memory.SummarizeEveryTurns", c.summarizeEveryTurns);
        c.memoryRetentionDays = Opt<uint32>("TrueCompanion.Memory.RetentionDays", c.memoryRetentionDays);
        c.dedupeThreshold = Opt<float>("TrueCompanion.Memory.DedupeThreshold", float(c.dedupeThreshold));

        c.progressionSpeed = Opt<float>("TrueCompanion.Relationship.ProgressionSpeed", float(c.progressionSpeed));
        c.affectionGainMultiplier = Opt<float>("TrueCompanion.Relationship.GainMultiplier", float(c.affectionGainMultiplier));
        c.affectionLossMultiplier = Opt<float>("TrueCompanion.Relationship.LossMultiplier", float(c.affectionLossMultiplier));
        c.moodSensitivity = Opt<float>("TrueCompanion.Relationship.MoodSensitivity", float(c.moodSensitivity));
        c.romanticInitiative = Opt<float>("TrueCompanion.Relationship.RomanticInitiative", float(c.romanticInitiative));
        c.dailyGainCap = Opt<float>("TrueCompanion.Relationship.DailyGainCap", float(c.dailyGainCap));
        c.minDaysDatingBeforeEngagement = Opt<uint32>("TrueCompanion.Relationship.MinDaysDatingBeforeEngagement", c.minDaysDatingBeforeEngagement);
        c.minDaysEngagedBeforeMarriage = Opt<uint32>("TrueCompanion.Relationship.MinDaysEngagedBeforeMarriage", c.minDaysEngagedBeforeMarriage);
        c.requireRingForEngagement = Opt<bool>("TrueCompanion.Relationship.RequireRingForEngagement", c.requireRingForEngagement);
        c.neglectDaysBeforeDecay = Opt<float>("TrueCompanion.Relationship.NeglectDaysBeforeDecay", float(c.neglectDaysBeforeDecay));

        c.giftMultiplier = Opt<float>("TrueCompanion.Gift.Multiplier", float(c.giftMultiplier));
        c.jewelryMultiplier = Opt<float>("TrueCompanion.Gift.JewelryMultiplier", float(c.jewelryMultiplier));
        c.giftDailyCap = Opt<float>("TrueCompanion.Gift.DailyCap", float(c.giftDailyCap));
        for (auto const& e : SplitList(Opt<std::string>("TrueCompanion.Gift.RomanticItemEntries", "")))
            if (uint32 v = uint32(std::strtoul(e.c_str(), nullptr, 10)))
                c.romanticItemEntries.push_back(v);
        std::string kw = Opt<std::string>("TrueCompanion.Gift.RomanticKeywords", "");
        if (!kw.empty())
        {
            c.romanticItemKeywords.clear();
            for (auto const& k : SplitList(kw))
                c.romanticItemKeywords.push_back(ToLower(k));
        }

        c.allowMemoryExport = Opt<bool>("TrueCompanion.Privacy.AllowExport", c.allowMemoryExport);
        c.allowFullReset = Opt<bool>("TrueCompanion.Privacy.AllowFullReset", c.allowFullReset);
        c.debugTabForPlayers = Opt<bool>("TrueCompanion.Privacy.DebugTabForPlayers", c.debugTabForPlayers);
        return c;
    }
}
