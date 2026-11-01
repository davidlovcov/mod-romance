# mod-romance

An AI companion and relationship module for **AzerothCore (WotLK 3.3.5a)** with **mod-playerbots** and a local **Ollama** model.

Turn one of your own characters into a companion who remembers you. She keeps her configured personality, reacts to fights, level-ups and gifts, sometimes starts conversations, and her relationship with you grows over time. Combat, movement and spellcasting stay with the normal Playerbots AI. Ollama only supplies her conversation, and **only for the companion you explicitly select**, never for your other bots.

The in-game interface is the companion addon: **[azerothcore-romance-addon](https://github.com/davidlovcov/azerothcore-romance-addon)**.

## Features

| Area | Summary |
|---|---|
| Companions | Your account's characters (plus Playerbots linked accounts) with their real status. They are called through `PlayerbotHolder::AddPlayerBot`, never twice, and only after an ownership check on the server. |
| Personality | Identity, story, 12 traits, response styles (anime, sweet, mature, teasing, tsundere, dandere, fantasy roleplay...), custom instructions, EN/DE/auto language, 7 presets including **Anime Girlfriend**. Adults only (enforced). |
| Memory | Short-term history plus long-term facts, preferences, episodes, gifts, promises, milestones and summaries. Each entry is tagged with its source; entries are de-duplicated, ranked by relevance and pruned within limits. They can be edited, deleted, exported and imported in the addon. |
| Relationship | Friendship, affection, attraction, trust, familiarity, comfort and satisfaction, plus mood. Eight stages from Strangers to Married, with a selectable starting stage. Progress is gradual and capped per day. Dating, engagement and marriage need an accepted proposal, which the server decides. |
| Gifts | Detected only after a **completed** trade, with the item confirmed in the companion's bags. The score weighs usefulness, upgrades, class fit, rarity, likes and dislikes, and romance (jewelry and rings count extra). Repeats count for less, there is a daily cap, and the same item instance is never rewarded twice. |
| World awareness | Zone, party, health, fights and near-death moments, deaths, level-ups, quests and notable loot. Only events the server observed are given to the model. |
| Initiative | Proactive lines with cooldowns, an hourly cap and a back-off when you don't reply; she stays quiet during combat. |
| Commands | Playerbots commands pass through untouched (`follow`, `co +dps`, `!stay`). Natural language ("Elara, follow me") maps to an allow-list: follow, stay, attack, flee. |
| Safety | Ollama requests run on a bounded worker pool with timeouts and cancellation, never on the world thread. The server keeps running when Ollama is down. The addon protocol is rate- and size-limited. Model output is sanitized and validated. |

## Requirements

- [mod-playerbots/azerothcore-wotlk](https://github.com/mod-playerbots/azerothcore-wotlk) (branch `Playerbot`) and [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)
- [Ollama](https://ollama.com) with `qwen3.5:4b` (fallback `qwen3:4b-instruct`), on the same machine or reachable over the network
- `AddonChannel = 1` in worldserver.conf (the default)

## Installation

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/davidlovcov/mod-romance.git
cd .. && ./acore.sh compiler build        # or your usual CMake build
```
Then copy `mod_romance.conf.dist` to `mod_romance.conf` in your `etc/modules` folder and set `Romance.Ollama.Url`.

**Updating:** run `cd modules/mod-romance && git pull`, then rebuild.

See [docs/INSTALLATION.md](docs/INSTALLATION.md) for Linux and Windows 11 details and troubleshooting.

## Module integration

| | |
|---|---|
| Tested core | mod-playerbots/azerothcore-wotlk `93333d6`, mod-playerbots `05a1f6f` |
| Configuration | `conf/mod_romance.conf.dist`, copied automatically by CMake. See [docs/CONFIGURATION.md](docs/CONFIGURATION.md). |
| SQL | characters database, 6 `romance_*` tables, applied automatically by the DB updater (`data/sql/db-characters/base`) |
| CMake hooks | `mod-romance.cmake` (links `ws2_32` on Windows, Threads) |
| WorldScript | `OnAfterConfigLoad`, `OnStartup`, `OnShutdown`, `OnUpdate` |
| PlayerScript | `OnPlayerCanUseChat` (whisper, group), `OnPlayerLogin`, `OnPlayerLogout`, `OnPlayerCreatureKill`, `OnPlayerCompleteQuest`, `OnPlayerStoreNewItem` |
| ServerScript | `CanPacketReceive` (`CMSG_ACCEPT_TRADE`), `CanPacketSend` (`SMSG_TRADE_STATUS`) |
| CommandScript | `.romance status \| summon \| dismiss \| on \| off` |
| Core changes | none |

## Documentation

- [docs/INSTALLATION.md](docs/INSTALLATION.md)
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md)
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/ADDON_PROTOCOL.md](docs/ADDON_PROTOCOL.md)
- [docs/TESTING.md](docs/TESTING.md)
- [CHANGELOG.md](CHANGELOG.md)

## Tests

```bash
scripts/run-tests.sh          # C++ unit, network and integration tests (no server build needed)
scripts/run-tests.sh --sql    # + SQL test against a local MySQL/MariaDB
```

## Project status

All automated tests pass. **In-game testing is still in progress.** Please report problems as GitHub issues and include your worldserver log.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Built for AzerothCore (GPL-2.0) and mod-playerbots (GPL-2.0-or-later); talks to Ollama over its HTTP API. No third-party code is bundled. Language models are downloaded separately; check each model's license.

World of Warcraft® is a trademark of Blizzard Entertainment, Inc. This project isn't affiliated with or endorsed by Blizzard.
