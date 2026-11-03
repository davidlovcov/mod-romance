# Addon protocol (version 1)

## Transport

WoW 3.3.5a addons can't open sockets, so communication uses addon messages.

* **Client → server:** `SendAddonMessage("TCMP", chunk, "WHISPER", UnitName("player"))`, a whisper to yourself in `LANG_ADDON`. The server intercepts it in `PlayerScript::OnPlayerCanUseChat` (inside `Player::Whisper`) and swallows it, so it's never relayed or echoed.
* **Server → client:** `ChatHandler::BuildChatPacket(CHAT_MSG_WHISPER, LANG_ADDON, player, player, "TCMP\t" + chunk)`. The client fires `CHAT_MSG_ADDON("TCMP", chunk, "WHISPER", <own name>)`. The addon ignores this prefix from any other sender.

## Framing

```
<id>:<seq>/<total>:<data>
```
* `id`: 1–6 base36 characters chosen by the sender. `seq` is 1-based. `total` is the chunk count.
* `data`: a slice (≤ 200 bytes, never split inside a UTF-8 sequence) of one JSON object.
* `|` never appears on the wire: JSON writers escape it as `|`, because the WoW client treats `|` as an escape sequence.
* Every message is a JSON object with a string `op`. Requests may carry a numeric `req`, which the server copies into its reply.

### Limits (server side)
| Limit | Value |
|---|---|
| chunk body | ≤ 250 bytes |
| reassembled message | ≤ 16 KiB, ≤ 96 chunks, JSON depth ≤ 12 |
| concurrent partial messages per player | 4 (expire after 15 s) |
| rate | token bucket 20 chunks/s, burst 80; each complete op costs 2 more tokens |

The addon sends at most 10 chunks/s with a burst of 30, which keeps it below AzerothCore's `ChatFlood.AddonMessageCount` (default 100).

### Identity and security
The server **never** takes identity from the payload. The owner is always the session's logged-in character. Every op that names a companion `guid` first checks that the character belongs to the player's account, or to a linked account when `TrueCompanion.AllowLinkedAccounts = 1` and Playerbots' `IsAccountLinked` agrees. Otherwise it replies with `error/not_owner` and reveals nothing. Replaying a request can only repeat an action the player is already allowed to take on their own data.

## Operations (client → server)

| op | fields | reply |
|---|---|---|
| `hello` | `version`, `protocol` | `hello` {protocol, enabled, features{ollama, proactive, naturalCommands, initialStage, maxInitialStage, modelChoice, export, reset, debug, playerbots, memory, botFeedback}, presets[{id, label, description, stage, gender, custom, profile}], maxCustomPresets, stages[], styles[], status} |
| `list_chars` | | `chars` {chars[{guid, name, race, raceName, class, className, gender, level, linked, status, reason, active, configured, stageLabel, estrangedUntil?, estranged?}], maxActive} |
| `get_status` | `guid?` | `status` (see StatusJson) |
| `get_profile` | | `profile` {profile, accountDefault} |
| `save_profile` | `profile`, `accountDefault` | `ack` |
| `list_models` | | `models` {models[], default, choice} |
| `get_companion` | `guid` | `companion` {guid, name, configured, personality, settings, relationship, active, status, reason} |
| `save_personality` | `guid`, `personality` | `ack` {ok, notes[]}, then `companion` |
| `apply_preset` | `guid`, `preset` (built-in id or `custom:<n>`) | `ack`, `companion`. Replaces the whole personality except name, nickname, language, stage labels and the proposal setting. |
| `list_presets` | | `presets` {presets[]} |
| `save_preset` | `label`, `description`, `personality`, `replace?` (`custom:<n>` to overwrite) | `presets` {ok, savedId, code?, message?, presets[]} |
| `delete_preset` | `preset` (`custom:<n>`) | `presets` {ok, code?, message?, presets[]} |
| `get_character_sheet` | `guid` | `character_sheet` {name, level, class, race, xp, xpNext, money, health, freeSlots, totalSlots, follow (follow/stay/lead), leadQuest?, items[{entry, name, count, quality, soulbound}], gear[{slot, entry, name, quality}], spells[{id, name}], learnable[{id, name, rank, cost, level}] (her class trainer can teach these now), learnableCost, learnSoon[...] (the next two levels), quests[{id, title, level, progress, complete, shared}]} or `ack` {ok: false, code: not_online} |
| `set_relationship` | `guid`, `stage`, `romantic`, `backstory` | `ack`, `companion` |
| `save_settings` | `guid`, `settings` | `ack`, `companion` |
| `summon` | `guid` | `ack` {ok, code: online_as_player, controlled_elsewhere, limit_reached, already_loading, not_owner, playerbots_not_installed, estranged ...} |
| `dismiss` | `guid`, `logout` | `ack` |
| `chat` | `guid`, `text` | pushes (`typing`, `chat_msg`) or `error` {busy, not_active, ai_disabled}. A line that goes to Playerbots instead (e.g. `follow`) gets `typing` {on: false}. |
| `get_lead` | | `lead_state` {leader?, leaderGuid?, you (nobody leads), kind (quest, turn_in, pick_up, take, merchant, grind), target, distance, direction, paused, errands[{name, kind, target}]}; also as `lead` in `group_quests` |
| `get_history` | `guid`, `limit ≤ 50` | `history` {items[{from, text, ts}]} |
| `get_memories` | `guid`, `kind?`, `search?`, `offset`, `limit ≤ 25` | `memories` {items[MemoryEntry], total, offset} |
| `edit_memory` | `guid`, `id`, `text`, `importance?` | `ack` |
| `delete_memory` | `guid`, `id` | `ack` {code: not_found} |
| `add_memory` | `guid`, `kind`, `text`, `importance` | `ack` {code: invalid_text, memory_limit_reached}. Your own memories count towards `TrueCompanion.Memory.MaxPerCompanion`; other memories are pruned to make room, but your own are never removed, so a full store refuses new ones. |
| `pin_memory` | `guid`, `id`, `pinned` | `ack` |
| `clear_memories` | `guid`, `kind?` (absent = all) | `ack` {notes} |
| `clear_history` | `guid` | `ack` |
| `reset_companion` | `guid`, `confirm: "RESET"` | `ack`, `companion` |
| `export` | `guid`, `part` (0-based, default 0) | `export_data` {data: JSON text of one page, part, parts}. Each page is at most 8 KiB; page 0 carries personality, settings and relationship, every page a slice of the memories. Ask for part 0, 1, … until `parts`, then join the pages. |
| `import_begin` | `guid`, `parts` (1–128) | `ack` |
| `import_part` | `guid`, `index`, `data` (JSON text of one page, same format as export pages) | `ack`. Each request must stay below 16 KiB (the addon keeps it under 14,000 bytes). |
| `import_end` | `guid` | `ack` {notes}, then `companion`. Applied only if every part arrived (`import_incomplete` otherwise; nothing changes). An unfinished import expires 60 s after its last part. Relationship values are never imported. |
| `import` | `guid`, `data` (JSON string) | Legacy single-message import for small exports (under 16 KiB). |
| `get_debug` | `guid` | `debug` {status…, debug{promptChars, latencyMs, status, error, warnings[], raw, requests, failures}} or `error/forbidden` |

`MemoryEntry` = `{id, kind, source, importance, text, createdAt, updatedAt, lastRecalledAt, recallCount, pinned}`.
Kinds are `fact preference episode relationship promise gift summary`. Sources are `observed player_stated model_summary interpretation user_edited profile`.

Every failed `ack` carries `code` (machine readable) and `message` (text for the player, e.g. "You already have the maximum number of AI companions (3). Send one home first.").

## Pushes (server → client, no `req`)

| op | meaning |
|---|---|
| `chat_msg` {guid, from: player/companion, text, ts, mood?} | conversation line (whisper and addon input). A companion reply is pushed **before** the visible whisper or party line (feature `botFeedback`), so the addon can tell it from Playerbots' command feedback |
| `bot_feedback` {guid, name} | a line went to Playerbots without an AI reply (plain command, AI off for that line); its answer in the next seconds is wanted and must not be hidden |
| `typing` {guid, on} | the companion is generating a reply |
| `relationship` {guid, stage, stageIndex, stageLabel, romantic, values{…}, mood, moodIntensity, strained, estranged, estrangedUntil, reconciliation, resentment, labels[]} | values changed |
| `milestone` {guid, stage, label} | became a couple, got engaged, married, broke up |
| `memory_changed` {guid, id?} | a memory was added, edited, pinned, deleted or cleared (no `id` = several changed) |
| `companion_active` {guid, active} / `chars_changed` | refresh the character list |
| `status` | Ollama went online or offline |
| `error` {code, message, for} | request failed |

## Character status values

`available`, `in_party`, `summoned`, `online_player`, `controlled_elsewhere`, `unavailable`, `offline`, `ai_disabled`.
