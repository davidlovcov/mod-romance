# Configuration

There are two layers:

1. **Server settings** (administrator): `mod_romance.conf` (template: `conf/mod_romance.conf.dist`). Reload with `.reload config`. The worker pool and Ollama backend are rebuilt, and in-flight requests are cancelled.
2. **Player preferences** (per companion, in the addon): stored server-side in `romance_companion.settings`. They are always clamped to the server limits, and a player can never change the Ollama URL, the limits or other server settings.

## Server settings

| Option | Default | Meaning |
|---|---|---|
| `Romance.Enable` | 1 | Master switch |
| `Romance.Debug` | 0 | Extra log lines (gifts, rejected addon messages) |
| `Romance.MaxActiveCompanionsPerPlayer` | 1 | AI companions active at once per player character |
| `Romance.MaxAiCompanionsServer` | 50 | Server-wide cap on AI companions |
| `Romance.AllowLinkedAccounts` | 1 | Characters from accounts linked via `.playerbots account link` |
| `Romance.AllowInitialStageSelection` | 1 | Players may choose the starting relationship |
| `Romance.MaxInitialStage` | 7 | Highest selectable starting stage (0 strangers … 7 married) |
| **Ollama** | | |
| `Romance.Ollama.Enable` | 1 | Off = companions behave as regular Playerbots |
| `Romance.Ollama.Url` | `http://127.0.0.1:11434` | Ollama base URL (http only) |
| `Romance.Ollama.Model` | `qwen3.5:4b` | Default model |
| `Romance.Ollama.FallbackModel` | `qwen3:4b-instruct` | Used if the default model isn't installed |
| `Romance.Ollama.AllowPlayerModelChoice` | 1 | Players may pick an installed model |
| `Romance.Ollama.AllowedModels` | "" | Comma-separated allow-list ("" = any installed model) |
| `Romance.Ollama.Temperature` / `MinTemperature` / `MaxTemperature` | 0.8 / 0.2 / 1.2 | Default and the range players may choose from |
| `Romance.Ollama.ContextLength` | 8192 | `num_ctx` |
| `Romance.Ollama.MaxResponseTokens` | 300 | `num_predict` |
| `Romance.Ollama.MaxReplyChars` | 450 | Hard cap on characters spoken per reply |
| `Romance.Ollama.Thinking` | 0 | `think` flag; off for responsive chat |
| `Romance.Ollama.StructuredOutput` | 1 | JSON-schema `format`; retried without it if the server rejects it |
| `Romance.Ollama.RequestTimeoutMs` / `ConnectTimeoutMs` | 45000 / 3000 | Timeouts |
| `Romance.Ollama.MaxConcurrentRequests` | 2 | Worker threads |
| `Romance.Ollama.MaxQueuedRequests` | 16 | Global queue bound (backpressure) |
| `Romance.Ollama.MaxPendingPerCompanion` | 2 | In-flight requests per companion |
| `Romance.Ollama.MinResponseCooldownMs` | 1500 | Minimum time between two roleplay messages accepted for one companion (anti-spam; commands unaffected) |
| `Romance.Ollama.HealthCheckIntervalSec` | 30 | `/api/version` + `/api/tags` probe |
| `Romance.Ollama.KeepAlive` | `10m` | Keeps the model loaded |
| **Conversation** | | |
| `Romance.Chat.HistoryTurns` / `StoredTurns` | 12 / 80 | Turns sent to the model / kept in the DB |
| `Romance.Chat.MaxPlayerMessageChars` | 500 | Longer player messages are truncated |
| `Romance.Chat.MaxProfileFieldChars` / `MaxInstructionChars` | 600 / 1200 | Text limits for personality and profile |
| `Romance.Chat.PartyOnly` | 0 | Always answer in party chat |
| `Romance.Chat.Language` | `auto` | `auto` (EN/DE per message), `en`, `de` |
| `Romance.Chat.NaturalLanguageCommands` | 1 | "follow me" → Playerbots `follow` (allow-list) |
| `Romance.Chat.GameplayContext` | 1 | Observed world state in prompts |
| `Romance.Chat.QuietInInstances` | 0 | No proactive chatter in dungeons or raids |
| **Proactive** | | |
| `Romance.Proactive.Enable` | 1 | Spontaneous lines |
| `Romance.Proactive.MinIntervalSec` / `MaxIntervalSec` | 240 / 900 | Spacing, scaled by player frequency × personality initiative, doubled after unanswered lines |
| `Romance.Proactive.Chance` | 0.6 | Probability when an interval elapses |
| `Romance.Proactive.MaxPerHour` | 6 | Hard cap |
| `Romance.Proactive.IdleCheckInSec` | 600 | "Are you still there?" after this much silence (once) |
| `Romance.Proactive.EventReactionCooldownSec` / `EventReactionChance` | 90 / 0.5 | Ordinary events. Near-death, deaths, level-ups, summons and gifts always get a reaction. |
| **Memory** | | |
| `Romance.Memory.Enable` | 1 | Long-term memory |
| `Romance.Memory.MaxPerCompanion` | 400 | Bound. Pinned, milestone and player-edited entries are never pruned |
| `Romance.Memory.RetrievalCount` / `RetrievalChars` | 10 / 1400 | Prompt memory budget |
| `Romance.Memory.SummarizeEveryTurns` | 24 | Conversation summary interval (0 = off) |
| `Romance.Memory.RetentionDays` | 0 | Forget unimportant, unprotected memories after N days |
| `Romance.Memory.DedupeThreshold` | 0.72 | Similarity at which a memory is merged into an existing one |
| **Relationship** | | |
| `Romance.Relationship.ProgressionSpeed` | 1.0 | Global speed |
| `Romance.Relationship.GainMultiplier` / `LossMultiplier` | 1.0 / 1.0 | Affection gain and loss |
| `Romance.Relationship.MoodSensitivity` | 1.0 | Mood intensity and negative impact |
| `Romance.Relationship.RomanticInitiative` | 1.0 | Weight of flirt signals and affectionate proactive lines |
| `Romance.Relationship.DailyGainCap` | 12 | Positive points per day from conversation |
| `Romance.Relationship.MinDaysDatingBeforeEngagement` / `MinDaysEngagedBeforeMarriage` | 2 / 1 | Time requirements |
| `Romance.Relationship.RequireRingForEngagement` | 0 | A ring must have been traded within the last hour |
| `Romance.Relationship.NeglectDaysBeforeDecay` | 7 | Satisfaction and comfort fade slowly after long absences (memories never fade from this) |
| **Gifts** | | |
| `Romance.Gift.Multiplier` / `JewelryMultiplier` | 1.0 / 1.6 | Score scaling |
| `Romance.Gift.DailyCap` | 30 | Affection points per day from gifts |
| `Romance.Gift.RomanticItemEntries` | "" | Extra item entries treated as romantic |
| `Romance.Gift.RomanticKeywords` | "" | Override the built-in EN/DE romantic keyword list |
| **Privacy** | | |
| `Romance.Privacy.AllowExport` | 1 | Memory export in the addon |
| `Romance.Privacy.AllowFullReset` | 1 | "Full reset" in the addon |
| `Romance.Privacy.DebugTabForPlayers` | 1 | Debug details for non-GMs |

Related settings in other files:

- `worldserver.conf`: `AddonChannel = 1` (required), `ChatFlood.AddonMessageCount` (keep the default 100 or higher), `Updates.EnableDatabases` (include characters for automatic SQL).
- `playerbots.conf`: `Playerbots.AllowAccountBots = 1`, `Playerbots.AllowTrustedAccountBots = 1`, `Playerbots.MaxAddedBots`.

## Player preferences (addon → Settings / Personality / Relationship)

| Preference | Limits applied by the server |
|---|---|
| AI conversation on/off | Off = no Ollama requests for this companion |
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
