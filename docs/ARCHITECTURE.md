# Architecture

## Overview

```
 WoW 3.3.5a client                         worldserver (AzerothCore Playerbot fork)
 ┌──────────────────┐  addon whispers     ┌────────────────────────────────────────────────────────┐
 │ TrueCompanion   │ ◄─────────────────► │ src/ac  (AzerothCore integration, world thread)        │
 │  Json / Protocol │   "TCMP" chunks     │  TrueCompanionScripts   hooks: chat, login, kills, packets   │
 │  Core (state)    │                     │  TrueCompanionManager   ownership, addon ops, summon, trades,│
 │  UI tabs         │  normal whispers    │                   snapshots, executes actions          │
 └──────────────────┘ ◄─────────────────► │  PlayerbotsBridge AddPlayerBot / HandleCommand / ...   │
                                          │  TrueCompanionSqlRepository (characters DB)                  │
                                          ├────────────────────────────────────────────────────────┤
                                          │ src/core (engine independent, unit tested)             │
                                          │  TrueCompanionService ── RelationshipEngine  GiftEvaluator   │
                                          │        │          MemoryStore         MessageClassifier│
                                          │        │          DialogueContextBuilder (prompts)     │
                                          │        ▼                                               │
                                          │  WorkerPool (bounded) ─► OllamaBackend ─► HttpRequest  │──► Ollama /api/chat
                                          │        ▲                                               │
                                          │  ResultQueue ◄── results (drained by Tick on world thr)│
                                          └────────────────────────────────────────────────────────┘
```

The design asked for components such as CompanionManager, PersonalityManager, OllamaClient and AddonMessageHandler. They map onto the code like this:

| Requested | Implementation |
|---|---|
| CompanionManager, CompanionOwnershipService, PlayerbotIntegration, AddonMessageHandler | `ac/TrueCompanionManager` (+ `ac/TrueCompanionPlayerbots`) |
| CompanionState, PersonalityManager, PlayerProfileService, ConversationManager | `core/TrueCompanionService` with `core/TrueCompanionTypes` |
| TrueCompanionRelationshipManager | `core/TrueCompanionRelationship` (`RelationshipEngine`) |
| TrueCompanionMemoryManager | `core/TrueCompanionMemory` (`MemoryStore`) |
| TrueCompanionEventProcessor | `TrueCompanionService::OnGameEvent` + `TrueCompanionManager::PollTrackers` |
| GiftEvaluationService | `core/TrueCompanionGift` |
| DialogueContextBuilder | `core/TrueCompanionPrompt` |
| OllamaClient | `core/TrueCompanionLlm` (`OllamaBackend`, `WorkerPool`) + `core/TrueCompanionHttp` |
| TrueCompanionConfig | `core/TrueCompanionConfig` + `ac/TrueCompanionAcConfig` |
| Presets (built-in and custom) | `core/TrueCompanionPresets`, stored by `ITrueCompanionRepository::*CustomPreset*` |
| Natural-language commands, knowledge topics | `core/TrueCompanionCommands` (parser, engine independent and unit tested) |
| Command execution, game knowledge, character sheet, lead mode, auto-sell | `ac/TrueCompanionControl` (`CompanionControl`, world thread) |

Commands run **before** the message reaches the model: `TrueCompanionManager::ApplyControl` parses the line, `CompanionControl` executes it against the real game state and writes a one-sentence outcome into `WorldContext::commandOutcome`. The prompt shows the outcome, so the companion only claims what really happened. Knowledge (bags, quests, spells...) is added to `WorldContext::knowledge` only when the message asks for it, which keeps prompts short for small models.

## Why a separate core

All companion logic (relationship rules, memory, prompts, validation, protocol, HTTP, worker pool, persistence interface) lives in `src/core`, which includes no AzerothCore headers. That makes it:

* unit-testable without a server (`tests/`, 75 tests, also run under sanitizers);
* safe to reason about thread-wise, because its only contact with the game is plain data snapshots in and `OutgoingAction` values out.

`src/ac` is the thin, game-specific layer.

## Threading

| Code | Thread | Notes |
|---|---|---|
| Chat hooks (`OnPlayerCanUseChat`), addon ops | world thread | `CMSG_MESSAGECHAT` is `PROCESS_THREADUNSAFE` |
| `CanPacketReceive(CMSG_ACCEPT_TRADE)` → trade snapshot | world thread | `PROCESS_THREADUNSAFE`; maps idle |
| `WorldScript::OnUpdate` → `TrueCompanionManager::Update` → `TrueCompanionService::Tick` | world thread | `World::Update` calls it **after** `MapMgr::Update` returns, so reading Player objects is safe |
| Login/logout, kill, quest, loot hooks | any (map threads) | push records into a mutex-protected inbox only |
| `CanPacketSend(SMSG_TRADE_STATUS)` | any | marks a snapshot completed under a mutex |
| Ollama HTTP | `WorkerPool` threads | input is an immutable `LlmRequest` built on the world thread; output goes into `ResultQueue` |

No AzerothCore object is ever touched off the world thread by this module. Worker threads only see strings. Every validation step and every state change (relationship, memories, persistence, chat, emotes, Playerbots commands) happens in `Tick` / `Execute` on the world thread.

**Backpressure:** the pool has N workers (`MaxConcurrentRequests`) and a bounded queue (`MaxQueuedRequests`). A companion may have at most `MaxPendingPerCompanion` requests in flight, and roleplay messages are spaced by `MinResponseCooldownMs`. When the limits are hit, the player gets a short "still answering" notice instead of an unbounded queue.

**Cancellation and shutdown:** each request has a cancel token, which the HTTP client checks every 100 ms while waiting. Dismissing a companion, logout, `.reload config` and worldserver shutdown cancel the tokens, join the workers, and save every session. Results from an old session generation are discarded.

**Ollama down:** requests fail fast (connection refused). The player gets one friendly notice per 2 minutes ("seems lost in thought…"). A health probe (`/api/version`, `/api/tags`) runs every `HealthCheckIntervalSec` and updates the status shown in the addon. The worldserver keeps running normally.

## Conversation pipeline

1. **Routing** (`MessageClassifier`)
   * Messages that look like Playerbots commands (`follow`, `co +dps`, `#p …`, `.…`), or start with `!`, pass through to Playerbots unchanged.
   * Everything else is roleplay. Roleplay whispers are swallowed so the Playerbots command parser never sees them, and the sender still gets the usual "To Elara: …" echo.
   * In party chat, the companion only answers when addressed by name. Party messages are always shown to the party.
2. **Deterministic intent:** natural-language commands (EN/DE, negation aware) map to the allow-list follow / stay / attack / flee and are executed immediately through `PlayerbotAI::HandleCommand`. Proposal phrases (dating, engagement, marriage, breakup) are evaluated by the `RelationshipEngine` **before** the prompt is built.
3. **Prompt** (`DialogueContextBuilder`): persona (traits rendered as words, not numbers), speaking style, the player's custom instructions (fenced and declared unable to change the rules), the player profile, a qualitative relationship description, retrieved memories tagged with source and age, observed world state, any proposal directive, the rules (language, length, no AI talk, nothing explicit, vary wording), and the JSON output contract. History turns follow as user/assistant messages. World events are marked with `<<GAME_EVENT>>`, and players can't type that marker.
4. **Ollama `/api/chat`:** `stream:false`, `think:false` by default, `format` = JSON schema, `options{temperature,num_ctx,num_predict,repeat_penalty}`, `keep_alive`. Missing models fall back to `FallbackModel`. Servers or models that reject `think` or `format` are retried without them.
5. **Validation** (`ParseReply`): JSON is extracted even from fenced or wrapped output. Enums are checked against fixed lists, at most 4 signals and 2 memory candidates are kept, model-proposed importance is capped at 70, gift/summary memory kinds from the model are rejected, and actions outside the allow-list become `none`. The reply text is sanitized (WoW `|` escapes, `<think>` blocks, a "Name:" prefix, control characters) and cut at a sentence boundary. If no JSON is found, plain text is used **with no side effects**.
6. **Coherence:** if a proposal is involved and the reply disagrees with the server's decision, the request is retried once with a stronger directive. If it still disagrees, a short neutral line that matches the decision is used. The server's decision is the only thing persisted. Near-verbatim repetition of recent lines also triggers one regeneration.
7. **Effects:** relationship signals (player messages only), mood, memory candidates (`PlayerStated` only if the text overlaps what the player actually wrote, otherwise `Interpretation`), proposal commit plus milestone memory and emote, and validated actions. Kiss requires Romantic Interest or higher; movement commands from the model require a request in the player's own words.
8. **Delivery:** the reply is split into ≤ 240-byte chat lines and sent with `Player::Whisper` from the bot (or party chat via Playerbots' `SayToParty`), and mirrored to the addon.

## Memory architecture

| Layer | Storage | Use |
|---|---|---|
| Short-term | `true_companion_chat_log` (last `StoredTurns`), in-session deque | last `HistoryTurns` turns go to the prompt |
| Long-term facts and preferences | `true_companion_memory` kind `fact`/`preference` | retrieved by relevance |
| Episodic | kinds `episode`, `gift`, `summary` | shared moments, gifts, conversation summaries |
| Relationship | kind `relationship`/`promise` | milestones, arguments, apologies, promises |
| Personality | `true_companion_companion.personality` | stable profile, always in the system prompt |

* **Sources:** `observed` (server saw it: gifts, stage changes, level-ups, quests, zones), `player_stated`, `model_summary`, `interpretation`, `user_edited`, `profile`. The prompt labels them ("witnessed", "they told you", "your impression, unsure"…) and tells the model to treat only witnessed, certain and backstory memories as definite.
* **Importance:** 0–100. The server sets it for observed events; model suggestions are capped at 70.
* **Dedup:** token Jaccard (EN/DE stop words, light stemming, prefix matching) ≥ `DedupeThreshold` merges entries, raises importance, and keeps the more reliable source's wording.
* **Retrieval:** IDF-weighted overlap with the current message and recent turns (50%), importance (25%), recency (15%) and source reliability (10%), plus a bonus for pinned entries. A few "core" memories are always included. The result is limited by count and characters and ordered as a timeline.
* **Summaries:** every `SummarizeEveryTurns` turns, a low-temperature summary request produces one summary and up to 4 facts, with the same source checks.
* **Retention:** at most `MaxPerCompanion` entries, pruned by a retention score (importance, age, recall count, source). Pinned, observed-milestone and player-edited entries are never pruned. An optional age limit applies to unimportant entries.
* **Scope:** everything is keyed by `(owner character guid, companion character guid)`. Different players, or different mains of the same player, never share memories.

## Relationship model

Values range 0–100 and are stored as JSON in `true_companion_companion.relationship`, with a denormalized `stage` column for admin queries.

* **Positive changes:** each signal has a base value (e.g. compliment → affection +1.2, attraction +0.8 if at least Friends and romantic). It is scaled by `ProgressionSpeed × GainMultiplier × (1 − value/125)`, limited to 4 points per message and `DailyGainCap` per day. Familiarity grows only from substantive messages.
* **Negative changes:** insults, rudeness, dismissiveness, broken promises and jealousy triggers (scaled by the jealousy trait). Loyal companions forgive a little more. A low trust or satisfaction value marks the relationship as **strained** until trust and satisfaction recover through apologies and support.
* **Stages:** Strangers → Acquaintances → Friends → Close Friends → Romantic Interest progress automatically, one step at a time, with hysteresis. **Dating, Engaged and Married are reached only through accepted proposals**:

  | Proposal | Prerequisites |
  |---|---|
  | Dating | stage ≥ Friends, affection ≥ 60, attraction ≥ 55, trust ≥ 50, familiarity ≥ 40, not strained |
  | Engagement | Dating for ≥ `MinDaysDatingBeforeEngagement`, affection ≥ 80, attraction ≥ 65, trust ≥ 75, familiarity ≥ 65 (optional ring) |
  | Marriage | Engaged for ≥ `MinDaysEngagedBeforeMarriage`, affection ≥ 85, attraction ≥ 70, trust ≥ 85, familiarity ≥ 75 |

  Within 80% of the requirements the answer is "give me time". Shy companions sometimes defer even when they're ready. Asking again within 10 minutes defers. A breakup is always respected; it drops the stage to Close Friends and lowers the values, but keeps the memories. `autoAcceptProposals` is an explicit per-companion opt-in.
* **Starting stage:** templates set consistent values (e.g. Couple: affection 80, trust 78) and seed backstory memories (`profile` source).
* **Mood:** set by events (combat → focused, near death → worried, gifts → happy or excited, long absence → lonely) and the model's emotion. It decays back to relaxed (or "enjoying company" for couples).

## Gifts and trades

1. `CMSG_ACCEPT_TRADE` from the owner, with an active companion as trader: snapshot the items in the trade slots (guid, entry, count, template data, usability for the companion's class, item level currently in the matching slot, the companion's count of that entry) and the money.
2. `SMSG_TRADE_STATUS` sent to the owner with `TRADE_STATUS_TRADE_COMPLETE` (AzerothCore sends this only after `moveItems`): mark the snapshot completed. Cancel or close drops it.
3. Next world update: verify each item in the companion's possession (same item guid, or the entry count increased). Only then call `TrueCompanionService::OnGift`.
4. `OnGift` ignores item guids already rewarded (loaded from `true_companion_gift_log` on activation, plus a `UNIQUE(companion_guid, item_guid)` constraint), evaluates the gift, applies capped deltas, stores a gift memory, and requests a reaction.

The module never moves, deletes or equips items. Equipping stays with Playerbots' own logic (e.g. `autogear`).

## Security summary

* The owner's identity comes from the session. Every companion guid is checked against the account or a linked account. Error replies leak nothing about other players' characters.
* Addon input is limited in size, chunk count, concurrent partial messages and rate. Malformed JSON is rejected, and unknown ops get an error.
* Stored and imported data is re-sanitized: age ≥ 18, text limits, `|` stripped, styles from a fixed list. Imports never set relationship values.
* LLM output can't execute anything. Actions come from a fixed enum, commands from a 4-item allow-list routed through Playerbots' own security check, and relationship changes from capped deltas.
* Prompt-injection text from the player can't forge world events, and the rules state that player text can't change values or memories.
* The Ollama URL is admin-only. Players can choose models only from the installed (or allowed) list.
* Only explicitly activated companions ever cause LLM requests. Other bots, bot-to-bot whispers and the companion's own messages are ignored, so there are no loops.

## Database tables (characters DB)

`true_companion_companion`, `true_companion_player_profile`, `true_companion_memory`, `true_companion_chat_log`, `true_companion_gift_log`, `true_companion_relationship_log`. See `data/sql/db-characters/base/`.

Reads are synchronous and happen only when a companion is activated, configured or inspected. Writes are asynchronous, idempotent upserts. Memory ids are per pair and never reused, so asynchronous write ordering is safe.

## Licensing notes

mod-true-companion is GPL-2.0-or-later, compatible with AzerothCore (GPL-2.0) and mod-playerbots (GPL-2.0-or-later). mod-ollama-chat is **AGPL-3.0**. It was studied for its approach (asynchronous requests, personalities, memory, sentiment), but **no code was copied** from it, because including AGPL-3.0 code would impose AGPL terms on the combined work. The HTTP client, JSON, prompt builder and memory system are original implementations.
