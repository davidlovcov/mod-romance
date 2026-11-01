/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Server-wide (administrator controlled) configuration. Values are loaded from
 * mod_romance.conf by the AzerothCore layer; defaults here match the .conf.dist.
 * Per-player preferences live in CompanionSettings instead.
 */

#ifndef MOD_ROMANCE_CONFIG_H
#define MOD_ROMANCE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace romance
{
    struct RomanceConfig
    {
        // module
        bool enabled = true;
        bool debugLogging = false;
        uint32_t maxActiveCompanionsPerPlayer = 3;
        uint32_t maxAiCompanionsServer = 50;
        bool allowLinkedAccounts = true;      // Playerbots "linked accounts" may provide companions
        bool allowInitialStageSelection = true;
        uint32_t maxInitialStage = 7;         // highest stage players may pick initially (7 = married)

        // ollama
        bool ollamaEnabled = true;
        std::string ollamaUrl = "http://127.0.0.1:11434";
        std::string model = "qwen3.5:4b";
        std::string fallbackModel = "qwen3:4b-instruct";
        std::vector<std::string> allowedModels;   // empty = any installed model may be selected
        bool allowPlayerModelChoice = true;
        double temperature = 0.8;
        double minTemperature = 0.2;
        double maxTemperature = 1.2;
        uint32_t contextLength = 8192;
        uint32_t maxResponseTokens = 300;
        uint32_t maxReplyChars = 450;
        bool thinking = false;                // prefer non-thinking chat for responsiveness
        bool structuredOutput = true;         // use JSON-schema "format"
        uint32_t requestTimeoutMs = 45000;
        uint32_t connectTimeoutMs = 3000;
        uint32_t maxConcurrentRequests = 2;   // worker threads
        uint32_t maxQueuedRequests = 16;      // global queue bound (backpressure)
        uint32_t maxPendingPerCompanion = 2;
        uint32_t minResponseCooldownMs = 1500;
        uint32_t healthCheckIntervalSec = 30;
        std::string keepAlive = "10m";

        // conversation
        uint32_t historyTurns = 12;           // short-term turns sent to the model
        uint32_t storedChatTurns = 80;        // persisted chat log per companion
        uint32_t maxPlayerMessageChars = 500;
        uint32_t maxProfileFieldChars = 600;
        uint32_t maxCustomPresets = 20;           // per account

        // companion control (natural-language commands, game knowledge)
        bool controlEnabled = true;               // give/cast/attack/sell/share/lead from chat
        uint32_t giveMinStage = 2;                // stage needed before she gives items or gold (2 = friends)
        uint64_t maxGoldPerRequest = 500000;      // copper (50 gold); 0 = no limit
        bool autoSellJunk = true;                 // sell gray items at a vendor when bags are nearly full
        uint32_t autoSellFreeSlots = 2;           // "nearly full" = this many free slots or fewer
        bool leadMode = true;                     // experimental: she leads to a quest (Playerbots "new rpg")
        float commandRange = 50.0f;               // yards to search for enemies named in commands
        uint32_t maxInstructionChars = 1200;
        bool partyOnlyChat = false;           // force replies to party chat
        std::string allowedLanguages = "auto"; // auto | en | de | en,de
        bool naturalLanguageCommands = true;
        bool gameplayContext = true;
        bool blockChatInInstances = false;    // keep chatting in dungeons (combat still blocks proactive)

        // proactive
        bool proactiveEnabled = true;
        uint32_t proactiveMinIntervalSec = 240;
        uint32_t proactiveMaxIntervalSec = 900;
        double proactiveChance = 0.6;
        uint32_t proactiveMaxPerHour = 6;
        uint32_t idleCheckInSec = 600;        // "are you still there?" after this much silence
        uint32_t eventReactionCooldownSec = 90;
        double eventReactionChance = 0.5;

        // memory
        bool memoryEnabled = true;
        uint32_t maxMemoriesPerCompanion = 400;
        uint32_t memoryRetrievalCount = 10;
        uint32_t memoryRetrievalChars = 1400;
        uint32_t summarizeEveryTurns = 24;
        uint32_t memoryRetentionDays = 0;     // 0 = keep forever (pruning by importance still applies)
        double dedupeThreshold = 0.72;

        // relationship
        double progressionSpeed = 1.0;
        double affectionGainMultiplier = 1.0;
        double affectionLossMultiplier = 1.0;
        double moodSensitivity = 1.0;
        double romanticInitiative = 1.0;
        double dailyGainCap = 12.0;           // max positive points per value per day from chat
        uint32_t minDaysDatingBeforeEngagement = 2;
        uint32_t minDaysEngagedBeforeMarriage = 1;
        bool requireRingForEngagement = false;
        double neglectDaysBeforeDecay = 7;

        // gifts
        double giftMultiplier = 1.0;
        double jewelryMultiplier = 1.6;
        double giftDailyCap = 30.0;           // max affection points per day from gifts
        std::vector<uint32_t> romanticItemEntries; // admin list of extra "romantic" item entries
        std::vector<std::string> romanticItemKeywords = {
            "rose", "flower", "bouquet", "lily", "blossom", "bloom", "chocolate", "perfume", "love", "heart",
            "valentine", "blume", "rosen", "pralinen" };

        // privacy
        bool allowMemoryExport = true;
        bool allowFullReset = true;
        bool debugTabForPlayers = true;
    };
}

#endif
