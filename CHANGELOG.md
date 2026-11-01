# Changelog

## Unreleased

### Added
- **Game knowledge:** ask her about her bags, gold, XP, gear, spells, quests (with progress and which you share) or your target; she answers from real game data.
- **Companion control** in plain sentences: give items or gold (she opens the trade), cast a spell on you or your target, attack an enemy by name or creature type (skull mark), sell junk or an item at a vendor, share quests, lead the way to one of her quests (experimental). Auto-sells junk at a vendor when bags are nearly full. `Romance.Control.*` options. See [docs/COMMANDS.md](docs/COMMANDS.md).
- **Several companions** at once (default 3); party messages go to the companion you name, or to the one you talked to last when only bots are in your group.
- **Presets:** every built-in preset is a complete personality; 7 male presets next to the 7 female ones; custom presets per account (`romance_preset` table, `Romance.Presets.MaxCustomPerAccount`).
- **Estrangement:** constant mistreatment makes her leave; she refuses calls for 72 h (longer each time) and comes back cold; reconciliation takes several days. `Romance.Estrangement.*` options.
- `get_character_sheet`, `list_presets`, `save_preset`, `delete_preset` addon ops; failed acks carry a readable `message`.

### Fixed
- After renaming a companion and saving, she sometimes still introduced herself with the character's name: saves are now visible immediately (write-through cache), and the prompt says her chosen name wins over the name shown in the world.
- She talked about the player in the third person ("Devid seems tired"); she now speaks to you directly.
- "Significant moments" only refreshed after switching tabs: the server now pushes `memory_changed` after every memory edit, delete, clear and reset.
- Party chat: she didn't answer when you wrote to the group without her name, even when only bots were in it.
- Server texts no longer assume a female companion.
- Ordinary messages right after a proposal (e.g. "I also like flowers") could be treated as another proposal and answered with a stock refusal. The model's proposal label is now ignored unless the message actually talks about the relationship status, and the prompt states that earlier questions don't count.
- Stock fallback lines never repeat the previous line and have more variety.
- Inappropriate forms of address ("daddy" etc.): the prompt now sets how she may address the player for the current stage and forbids such terms.

## 1.0.0 — 2026-11-01

First release.

- Engine-independent core with relationship rules, gift evaluation, layered memory (dedup, retrieval, summaries, retention), prompt builder, structured-output validation, EN/DE language matching, intent detection, addon wire protocol, an HTTP client for Ollama, and a bounded worker pool with cancellation and backpressure.
- AzerothCore integration:
  - whisper/party/addon hooks;
  - ownership checks including Playerbots linked accounts;
  - summoning through `PlayerbotHolder::AddPlayerBot`, with polling until Playerbots has attached the bot's AI and master;
  - on-demand activation of an online companion (whisper, addon chat, "Activate AI");
  - completed-trade gift detection with inventory verification;
  - world-state snapshots and event polling (combat, near death, deaths, level-ups, zones), quest and loot hooks;
  - emotes, and allow-listed Playerbots commands.
- Characters-DB persistence (6 tables), asynchronous idempotent writes, schema check at startup.
- `.romance status|summon|dismiss|on|off` chat commands and `mod_romance.conf.dist` with 70+ documented options.
- 75 C++ tests (including the full couple → ring → restart → remembers flow) and an SQL test against MySQL/MariaDB.

### Known limitations
- Gifts sent by mail aren't recognized; only completed trades count.
- If mod-ollama-chat is installed as well, it may also answer whispers to the companion.
- Party-chat roleplay lines are still visible to Playerbots' own party-command parser (whispers are not).
