# Configuration

There are two layers:

1. **Server settings** (administrator): `mod_true_companion.conf` (template: `conf/mod_true_companion.conf.dist`). Reload with `.reload config`. The worker pool and Ollama backend are rebuilt, and in-flight requests are cancelled.
2. **Player preferences** (per companion, in the addon): stored server-side in `true_companion_companion.settings`. They are always clamped to the server limits, and a player can never change the Ollama URL, the limits or other server settings.

## Server settings

| Option | Default | Meaning |
|---|---|---|
| `TrueCompanion.Enable` | 1 | Master switch |
| `TrueCompanion.Debug` | 0 | Extra log lines (gifts, rejected addon messages) |
| `TrueCompanion.MaxActiveCompanionsPerPlayer` | 3 | AI companions active at once per player character (the named one answers; otherwise the one you talked to last) |
| `TrueCompanion.MaxAiCompanionsServer` | 50 | Server-wide cap on AI companions |
| `TrueCompanion.AllowLinkedAccounts` | 1 | Characters from accounts linked via `.playerbots account link` |
| `TrueCompanion.AllowInitialStageSelection` | 1 | Players may choose the starting relationship |
| `TrueCompanion.MaxInitialStage` | 7 | Highest selectable starting stage (0 strangers … 7 married) |
| **Ollama** | | |
| `TrueCompanion.Ollama.Enable` | 1 | Off = no conversation, but commands, lead mode, quests and the addon still work; she says what she did in plain words. The same happens while Ollama is unreachable. |
| `TrueCompanion.Ollama.Url` | `http://127.0.0.1:11434` | Ollama base URL (http only) |
| `TrueCompanion.Ollama.Model` | `qwen3.5:4b` | Default model |
| `TrueCompanion.Ollama.FallbackModel` | `qwen3:4b-instruct` | Used if the default model isn't installed |
| `TrueCompanion.Ollama.AllowPlayerModelChoice` | 1 | Players may pick an installed model |
| `TrueCompanion.Ollama.AllowedModels` | "" | Comma-separated allow-list ("" = any installed model) |
| `TrueCompanion.Ollama.Temperature` / `MinTemperature` / `MaxTemperature` | 0.8 / 0.2 / 1.2 | Default and the range players may choose from |
| `TrueCompanion.Ollama.ContextLength` | 8192 | `num_ctx` |
| `TrueCompanion.Ollama.MaxResponseTokens` | 300 | `num_predict` |
| `TrueCompanion.Ollama.MaxReplyChars` | 450 | Hard cap on characters spoken per reply |
| `TrueCompanion.Ollama.Thinking` | 0 | `think` flag; off for responsive chat |
| `TrueCompanion.Ollama.StructuredOutput` | 1 | JSON-schema `format`; retried without it if the server rejects it |
| `TrueCompanion.Ollama.RequestTimeoutMs` / `ConnectTimeoutMs` | 45000 / 3000 | Timeouts |
| `TrueCompanion.Ollama.MaxConcurrentRequests` | 2 | Worker threads |
| `TrueCompanion.Ollama.MaxQueuedRequests` | 16 | Global queue bound (backpressure) |
| `TrueCompanion.Ollama.MaxPendingPerCompanion` | 2 | In-flight requests per companion |
| `TrueCompanion.Ollama.MinResponseCooldownMs` | 1500 | Minimum time between two roleplay messages accepted for one companion (anti-spam; commands unaffected) |
| `TrueCompanion.Ollama.HealthCheckIntervalSec` | 30 | `/api/version` + `/api/tags` probe |
| `TrueCompanion.Ollama.KeepAlive` | `10m` | Keeps the model loaded |
| **Conversation** | | |
| `TrueCompanion.Chat.HistoryTurns` / `StoredTurns` | 12 / 80 | Turns sent to the model / kept in the DB |
| `TrueCompanion.Chat.MaxPlayerMessageChars` | 500 | Longer player messages are truncated |
| `TrueCompanion.Chat.MaxProfileFieldChars` / `MaxInstructionChars` | 600 / 1200 | Text limits for personality and profile |
| `TrueCompanion.Presets.MaxCustomPerAccount` | 20 | Custom personality presets a player account can save (0 = none) |
| `TrueCompanion.Control.Enable` | 1 | Natural-language commands and game knowledge (see [COMMANDS.md](COMMANDS.md)) |
| `TrueCompanion.Control.GiveMinStage` | 2 | Relationship stage before the companion hands over items or gold (2 = friends) |
| `TrueCompanion.Control.MaxGoldPerRequest` | 50 | Most gold per request (0 = no limit) |
| `TrueCompanion.Control.AutoSellJunk` / `AutoSellFreeSlots` | 1 / 2 | Sell gray items at a vendor when bags are nearly full |
| `TrueCompanion.Control.LeadMode` | 1 | "lead the way" walks the companion to one of their quests, then they fight the quest creatures |
| `TrueCompanion.Control.InteractRange` | 30 | Merchants and quest givers this close work without targeting them |
| `TrueCompanion.Control.QuestSync` | 1 | Companions take and turn in quests together with you |
| `TrueCompanion.Party.UnaddressedReplies` | all | Party lines without a name (bots-only group): `all` companions or the `last` one you talked to |
| `TrueCompanion.Control.CommandRange` | 50 | Yards to search for an enemy named in a command |
| `TrueCompanion.Estrangement.Enable` | 1 | The companion leaves when mistreated again and again |
| `TrueCompanion.Estrangement.Resentment` | 100 | Resentment at which the companion leaves |
| `TrueCompanion.Estrangement.CooldownHours` | 72 | Hours the companion refuses calls (x2 the second time, x4 after) |
| `TrueCompanion.Estrangement.ReconcilePerDay` | 25 | Reconciliation points per day after the companion returns (100 needed) |
| `TrueCompanion.Chat.PartyOnly` | 0 | Always answer in party chat |
| `TrueCompanion.Chat.Language` | `auto` | `auto` (EN/DE per message), `en`, `de` |
| `TrueCompanion.Chat.NaturalLanguageCommands` | 1 | "follow me" → Playerbots `follow` (allow-list) |
| `TrueCompanion.Chat.GameplayContext` | 1 | Observed world state in prompts |
| `TrueCompanion.Chat.QuietInInstances` | 0 | No proactive chatter in dungeons or raids |
| **Proactive** | | |
| `TrueCompanion.Proactive.Enable` | 1 | Spontaneous lines |
| `TrueCompanion.Proactive.MinIntervalSec` / `MaxIntervalSec` | 240 / 900 | Spacing, scaled by player frequency × personality initiative, doubled after unanswered lines |
| `TrueCompanion.Proactive.Chance` | 0.6 | Probability when an interval elapses |
| `TrueCompanion.Proactive.MaxPerHour` | 6 | Hard cap |
| `TrueCompanion.Proactive.IdleCheckInSec` | 600 | "Are you still there?" after this much silence (once) |
| `TrueCompanion.Proactive.EventReactionCooldownSec` / `EventReactionChance` | 90 / 0.5 | Ordinary events. Near-death, deaths, level-ups, summons and gifts always get a reaction. |
| **Memory** | | |
| `TrueCompanion.Memory.Enable` | 1 | Long-term memory |
| `TrueCompanion.Memory.MaxPerCompanion` | 400 | Bound. Pinned, milestone and player-edited entries are never pruned |
| `TrueCompanion.Memory.RetrievalCount` / `RetrievalChars` | 10 / 1400 | Prompt memory budget |
| `TrueCompanion.Memory.SummarizeEveryTurns` | 24 | Conversation summary interval (0 = off) |
| `TrueCompanion.Memory.RetentionDays` | 0 | Forget unimportant, unprotected memories after N days |
| `TrueCompanion.Memory.DedupeThreshold` | 0.72 | Similarity at which a memory is merged into an existing one |
| **Relationship** | | |
| `TrueCompanion.Relationship.ProgressionSpeed` | 1.0 | Global speed |
| `TrueCompanion.Relationship.GainMultiplier` / `LossMultiplier` | 1.0 / 1.0 | Affection gain and loss |
| `TrueCompanion.Relationship.MoodSensitivity` | 1.0 | Mood intensity and negative impact |
| `TrueCompanion.Relationship.RomanticInitiative` | 1.0 | Weight of flirt signals and affectionate proactive lines |
| `TrueCompanion.Relationship.DailyGainCap` | 12 | Positive points per day from conversation |
| `TrueCompanion.Relationship.MinDaysDatingBeforeEngagement` / `MinDaysEngagedBeforeMarriage` | 2 / 1 | Time requirements |
| `TrueCompanion.Relationship.RequireRingForEngagement` | 0 | A ring must have been traded within the last hour |
| `TrueCompanion.Relationship.NeglectDaysBeforeDecay` | 7 | Satisfaction and comfort fade slowly after long absences (memories never fade from this) |
| **Gifts** | | |
| `TrueCompanion.Gift.Multiplier` / `JewelryMultiplier` | 1.0 / 1.6 | Score scaling |
| `TrueCompanion.Gift.DailyCap` | 30 | Affection points per day from gifts |
| `TrueCompanion.Gift.RomanticItemEntries` | "" | Extra item entries treated as romantic |
| `TrueCompanion.Gift.RomanticKeywords` | "" | Override the built-in EN/DE romantic keyword list |
| **Privacy** | | |
| `TrueCompanion.Privacy.AllowExport` | 1 | Memory export in the addon |
| `TrueCompanion.Privacy.AllowFullReset` | 1 | "Full reset" in the addon |
| `TrueCompanion.Privacy.DebugTabForPlayers` | 1 | Debug details for non-GMs |

Related settings in other files:

- `worldserver.conf`: `AddonChannel = 1` (required), `ChatFlood.AddonMessageCount` (keep the default 100 or higher), `Updates.EnableDatabases` (include characters for automatic SQL).
- `playerbots.conf`: `Playerbots.AllowAccountBots = 1`, `Playerbots.AllowTrustedAccountBots = 1`, `Playerbots.MaxAddedBots`.

## Player preferences (addon → Settings / Personality / Relationship)

| Preference | Limits applied by the server |
|---|---|
| AI conversation on/off | Off = no Ollama requests for this companion; she still carries out commands (answered in plain words) and doesn't count toward `MaxActiveCompanionsPerPlayer` |
| Proactive on/off, frequency 0–100 | Off if the server disables proactive chat |
| Spoken commands | Off if the server disables them; allow-list only |
| Party chat mode / listen in party | Forced on if `Chat.PartyOnly = 1` |
| Model | Only if `AllowPlayerModelChoice`, and only from `AllowedModels` |
| Temperature | Clamped to `MinTemperature`…`MaxTemperature` |
| Max reply length | ≤ `MaxReplyChars` |
| Personality | Sanitized: age ≥ 18, text limits, known styles only, `\|` removed |
| Starting stage | ≤ `MaxInitialStage`; platonic companions ≤ Close Friends |

## Personality presets

`anime_girlfriend` (couple), `shy_sweetheart`, `longtime_partner` (married), `tsundere`, `mature_companion`, `playful_adventurer`, `loyal_friend` (platonic). Applying a preset sets traits and styles. It keeps the name, backstory and other identity fields you already entered.
