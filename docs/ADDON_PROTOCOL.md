# Addon protocol (version 1)

## Transport

WoW 3.3.5a addons can't open sockets, so communication uses addon messages.

* **Client → server:** `SendAddonMessage("ARMN", chunk, "WHISPER", UnitName("player"))`, a whisper to yourself in `LANG_ADDON`. The server intercepts it in `PlayerScript::OnPlayerCanUseChat` (inside `Player::Whisper`) and swallows it, so it's never relayed or echoed.
* **Server → client:** `ChatHandler::BuildChatPacket(CHAT_MSG_WHISPER, LANG_ADDON, player, player, "ARMN\t" + chunk)`. The client fires `CHAT_MSG_ADDON("ARMN", chunk, "WHISPER", <own name>)`. The addon ignores this prefix from any other sender.

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
| rate | token bucket 15 chunks/s, burst 60; each complete op costs 2 more tokens |

The addon sends at most 20 chunks/s and pauses 1.2 s after every 40 chunks, which keeps it below AzerothCore's `ChatFlood.AddonMessageCount` (default 100).

### Identity and security
The server **never** takes identity from the payload. The owner is always the session's logged-in character. Every op that names a companion `guid` first checks that the character belongs to the player's account, or to a linked account when `Romance.AllowLinkedAccounts = 1` and Playerbots' `IsAccountLinked` agrees. Otherwise it replies with `error/not_owner` and reveals nothing. Replaying a request can only repeat an action the player is already allowed to take on their own data.

## Operations (client → server)

| op | fields | reply |
|---|---|---|
| `hello` | `version`, `protocol` | `hello` {protocol, enabled, features{ollama, proactive, naturalCommands, initialStage, maxInitialStage, modelChoice, export, reset, debug, playerbots, memory}, presets[], stages[], styles[], status} |
| `list_chars` | | `chars` {chars[{guid, name, race, raceName, class, className, gender, level, linked, status, reason, active, configured, stageLabel}], maxActive} |
| `get_status` | `guid?` | `status` (see StatusJson) |
| `get_profile` | | `profile` {profile, accountDefault} |
| `save_profile` | `profile`, `accountDefault` | `ack` |
| `list_models` | | `models` {models[], default, choice} |
| `get_companion` | `guid` | `companion` {guid, name, configured, personality, settings, relationship, active, status, reason} |
| `save_personality` | `guid`, `personality` | `ack` {ok, notes[]}, then `companion` |
| `apply_preset` | `guid`, `preset` | `ack`, `companion` |
| `set_relationship` | `guid`, `stage`, `romantic`, `backstory` | `ack`, `companion` |
| `save_settings` | `guid`, `settings` | `ack`, `companion` |
| `summon` | `guid` | `ack` {ok, code: online_as_player, controlled_elsewhere, limit_reached, already_loading, not_owner, playerbots_not_installed ...} |
| `dismiss` | `guid`, `logout` | `ack` |
| `chat` | `guid`, `text` | pushes (`typing`, `chat_msg`) or `error` {busy, not_active, ai_disabled} |
| `get_history` | `guid`, `limit ≤ 50` | `history` {items[{from, text, ts}]} |
| `get_memories` | `guid`, `kind?`, `search?`, `offset`, `limit ≤ 25` | `memories` {items[MemoryEntry], total, offset} |
| `edit_memory` | `guid`, `id`, `text`, `importance?` | `ack` |
| `delete_memory` | `guid`, `id` | `ack` |
| `add_memory` | `guid`, `kind`, `text`, `importance` | `ack` |
| `pin_memory` | `guid`, `id`, `pinned` | `ack` |
| `clear_memories` | `guid`, `kind?` (absent = all) | `ack` {notes} |
| `clear_history` | `guid` | `ack` |
| `reset_companion` | `guid`, `confirm: "RESET"` | `ack`, `companion` |
| `export` | `guid` | `export_data` {data: JSON string} |
| `import` | `guid`, `data` (JSON string) | `ack` {notes}. Relationship values are never imported. |
| `get_debug` | `guid` | `debug` {status…, debug{promptChars, latencyMs, status, error, warnings[], raw, requests, failures}} or `error/forbidden` |

`MemoryEntry` = `{id, kind, source, importance, text, createdAt, updatedAt, lastRecalledAt, recallCount, pinned}`.
Kinds are `fact preference episode relationship promise gift summary`. Sources are `observed player_stated model_summary interpretation user_edited profile`.

## Pushes (server → client, no `req`)

| op | meaning |
|---|---|
| `chat_msg` {guid, from: player/companion, text, ts, mood?} | conversation line (whisper and addon input) |
| `typing` {guid, on} | the companion is generating a reply |
| `relationship` {guid, stage, stageIndex, stageLabel, romantic, values{…}, mood, moodIntensity, strained, labels[]} | values changed |
| `milestone` {guid, stage, label} | became a couple, got engaged, married, broke up |
| `memory_changed` {guid, id} | a memory was added or updated |
| `companion_active` {guid, active} / `chars_changed` | refresh the character list |
| `status` | Ollama went online or offline |
| `error` {code, message, for} | request failed |

## Character status values

`available`, `in_party`, `summoned`, `online_player`, `controlled_elsewhere`, `unavailable`, `offline`, `ai_disabled`.
