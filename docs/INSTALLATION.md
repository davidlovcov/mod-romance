# Installation

## Versions this release was built against

| Component | Repository | Branch | Commit (2026-10-30) |
|---|---|---|---|
| AzerothCore (Playerbot fork) | https://github.com/mod-playerbots/azerothcore-wotlk | `Playerbot` | `93333d6a9eb00fdf0179adde30da3c17618207d6` |
| mod-playerbots | https://github.com/mod-playerbots/mod-playerbots | `master` | `05a1f6fceb8a32aa879b13f5f48934c6e46a774c` |
| mod-ollama-chat (studied for reference only, not required) | https://github.com/DustinHendrickson/mod-ollama-chat | `main` | `906eb2a45f5b3b815a281977005371193517eb32` |

**mod-playerbots requires the Playerbot fork of AzerothCore.** The standard AzerothCore repository will not work with it. mod-romance itself only *needs* mod-playerbots for summoning companions. Without it, the module still compiles, but companions can't be called.

The module uses C++20, matching the fork's `CMAKE_CXX_STANDARD 20`. It needs no extra third-party library: the Ollama client uses plain sockets, and JSON is implemented in the module.

Newer commits of the fork and mod-playerbots will probably work. If the build fails on a newer revision, check out the commits above with `git checkout <hash>`.

---

## 1. Ollama

Install Ollama from https://ollama.com (Linux: `curl -fsSL https://ollama.com/install.sh | sh`; Windows: the installer). Then:

```bash
ollama pull qwen3.5:4b          # default model
ollama pull qwen3:4b-instruct   # fallback, used automatically if the default is missing
curl http://127.0.0.1:11434/api/version
```

The 4B models need about 4–8 GB of RAM or VRAM. On weaker hardware, `qwen3.5:2b` works (set `Romance.Ollama.Model`).
Keep Ollama on `127.0.0.1`, or on a LAN host you trust. The module only supports `http://`.

---

## 2. Linux (Ubuntu 22.04 / 24.04)

### Prerequisites
```bash
sudo apt-get update
sudo apt-get install git cmake make gcc g++ clang libmysqlclient-dev libssl-dev libbz2-dev \
    libreadline-dev libncurses-dev libboost-all-dev mysql-server
```
(Follow the official [AzerothCore requirements](https://www.azerothcore.org/wiki/linux-requirements) if your distribution differs.)

### Sources
```bash
git clone https://github.com/mod-playerbots/azerothcore-wotlk.git --branch=Playerbot azerothcore
cd azerothcore/modules
git clone https://github.com/mod-playerbots/mod-playerbots.git --branch=master
git clone https://github.com/davidlovcov/mod-romance.git     # SSH: git@github.com:davidlovcov/mod-romance.git
cd ..
```

### Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/azeroth-server -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
         -DWITH_WARNINGS=1 -DTOOLS_BUILD=all -DSCRIPTS=static -DMODULES=static
make -j"$(nproc)"
make install
```
`scripts/install-linux.sh` runs the same steps (`--src`, `--prefix`, `--jobs`, `--pinned`).

### Configuration
```bash
cd $HOME/azeroth-server/etc
cp modules/mod_romance.conf.dist modules/mod_romance.conf
cp modules/playerbots.conf.dist modules/playerbots.conf     # if not done yet
```
Make sure Playerbots allows your own characters as bots (these are the defaults):
```
Playerbots.AllowAccountBots = 1
Playerbots.AllowTrustedAccountBots = 1   # for linked accounts
```

### Database
The module's SQL lives in `data/sql/db-characters/base/`. The AzerothCore DB updater applies it automatically on worldserver start, as long as the characters database is included in `Updates.EnableDatabases` (worldserver.conf, default `7` = all). To apply it manually:
```bash
mysql -u acore -p acore_characters < modules/mod-romance/data/sql/db-characters/base/mod_romance_2026_11_01_00.sql
```
On start, the module checks that its 6 tables exist. If they don't, it logs an error and disables itself; it doesn't crash.

### Run
```bash
cd $HOME/azeroth-server/bin && ./authserver & ./worldserver
```
Look for: `[mod-romance] enabled | Ollama on at http://127.0.0.1:11434 | model qwen3.5:4b | Playerbots found`.

---

## 3. Windows 11

### Prerequisites
- **Visual Studio 2022** with "Desktop development with C++"
- **CMake** ≥ 3.16 (bundled with VS or from cmake.org)
- **Git for Windows**
- **MySQL Server 8.x** (also install the C connector/dev files; note the path, e.g. `C:\Program Files\MySQL\MySQL Server 8.4`)
- **Boost** (prebuilt for MSVC 14.3, e.g. `C:\local\boost_1_84_0`) with the environment variable `BOOST_ROOT` set
- **OpenSSL 3.x** (Win64 full installer)

See the [AzerothCore Windows requirements](https://www.azerothcore.org/wiki/windows-requirements) for the authoritative list.

### Sources and build (PowerShell)
```powershell
git clone https://github.com/mod-playerbots/azerothcore-wotlk.git --branch=Playerbot C:\azerothcore
cd C:\azerothcore\modules
git clone https://github.com/mod-playerbots/mod-playerbots.git --branch=master
git clone https://github.com/davidlovcov/mod-romance.git
cd C:\azerothcore
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSCRIPTS=static -DMODULES=static -DTOOLS_BUILD=all
cmake --build build --config RelWithDebInfo --parallel
```
`scripts/install-windows.ps1` does this and copies the configs (`-Source`, `-Pinned`).

Copy `build\bin\RelWithDebInfo\configs\modules\mod_romance.conf.dist` to `mod_romance.conf` next to it. Database and run steps are the same as on Linux.

---

## 4. Addon

Get the addon from the separate repository **[azerothcore-romance-addon](https://github.com/davidlovcov/azerothcore-romance-addon)** (green *Code* button → *Download ZIP*, or a release ZIP). Copy the `AzerothRomance` folder from it to:

- Windows: `C:\...\World of Warcraft 3.3.5a\Interface\AddOns\AzerothRomance\`
- Linux/Wine: `<wine prefix>/drive_c/.../World of Warcraft/Interface/AddOns/AzerothRomance/`

The folder must contain `AzerothRomance.toc` directly. Enable "Load out of date AddOns" if your client complains. Type `/romance` (or `/ar`) in game, or click the heart on the minimap.

---

### Updating

```bash
cd azerothcore/modules/mod-romance && git pull
cd ../.. && ./acore.sh compiler build      # or your usual cmake build
```
Database changes in future versions are applied automatically on the next worldserver start.

## 5. First steps in game

1. Log in with your main character (e.g. Arthas) and run `/romance`.
2. Pick a character (e.g. Elara). Greyed-out characters can't be used right now; hover over them to see why.
3. **Personality** tab: choose a preset (e.g. *Anime Girlfriend*), then adjust it and save.
4. **Relationship** tab: choose a starting relationship (e.g. *Dating / Couple*) with an optional backstory.
5. **Companion** tab: **Call companion**. She logs in as your Playerbot and joins your party.
6. Whisper her, or use the **Chat** tab. Trade her an item to give a gift.

Chat-command alternatives: `.romance summon Elara`, `.romance dismiss`, `.romance status`, `.romance on|off` (on a targeted bot of yours).

---

## 6. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Addon says "Waiting for the server..." | The module isn't loaded (check the worldserver log for `[mod-romance]`), `Romance.Enable = 0`, or `AddonChannel = 0` in worldserver.conf (it must be 1). |
| `Table romance_... is missing` in the log | The SQL wasn't applied. Apply it manually (see Database above). |
| "seems lost in thought (the AI service is offline)" | Ollama isn't running or not reachable at `Romance.Ollama.Url`. Test with `curl <url>/api/version`. |
| "the configured model is not installed" | Run `ollama pull qwen3.5:4b`, or set `Romance.Ollama.Model` to an installed model (`ollama list`). |
| Replies take very long or time out | The model is too big for your hardware. Use a smaller model, keep `Romance.Ollama.Thinking = 0`, or raise `RequestTimeoutMs`. The first request after idle loads the model; `KeepAlive` keeps it loaded. |
| "Cannot call: limit_reached" | `Romance.MaxActiveCompanionsPerPlayer` is reached. Send the other companion home first. |
| Character shows "Online as a player" | That character is logged in with a real client. Log it out. |
| Bot joins but doesn't talk with AI | AI is disabled for that companion (Companion tab → Enable AI), or `Romance.Ollama.Enable = 0`. |
| Gift not recognized | Only completed trades count, and each item instance is rewarded once. Mail isn't treated as a gift. The bot must be your active companion. |
| Two bots answer the same whisper | **mod-ollama-chat** is also installed and answers bot whispers. Disable it, or don't use both on the same character. |
| Muted for chat flood | Very large imports. The addon paces its traffic below `ChatFlood.AddonMessageCount`; if you lowered that setting, raise it again. |
| Linking error mentioning `ws2_32` on Windows | Delete the CMake cache and re-run CMake. `mod-romance.cmake` adds `ws2_32` automatically. |
