# Testing

## Run everything

```bash
scripts/run-tests.sh            # C++ unit, network and integration tests
scripts/run-tests.sh --sql      # + SQL test against a local MySQL/MariaDB (uses "sudo mysql")
scripts/run-tests.sh --sanitize # C++ tests under AddressSanitizer + UBSan
ADDON_DIR=../azerothcore-true-companion-addon scripts/run-tests.sh   # + C++ <-> Lua wire interop with the addon
```

Requirements: CMake ≥ 3.16, a C++20 compiler, Python 3 (for the SQL test) and `lua5.1` (for the interop test). The addon has its own tests in [azerothcore-true-companion-addon](https://github.com/davidlovcov/azerothcore-true-companion-addon).

| Suite | Location | What it exercises |
|---|---|---|
| C++ unit tests | `tests/test_core_units.cpp` | JSON parser hardening, wire protocol (chunking, out-of-order, oversize, hostile input, rate limiter), relationship rules, gift scoring, memory dedup/retrieval/pruning, routing and intent detection, sanitizers, prompt construction, structured-output validation, personality sanitizing and presets |
| C++ network tests | `tests/test_http.cpp` | real sockets against a local fake Ollama server (`tests/mock/MockHttpServer.h`): request body (`stream:false`, `think:false`, `format` schema), chunked responses, offline server, timeout, cancellation, missing model → fallback, "does not support thinking" → retry without `think`, `/api/tags`, `/api/version`, garbage bodies |
| C++ integration tests | `tests/test_service.cpp` | the full `TrueCompanionService` with a scripted mock LLM (`tests/mock/MockLlm.h`) and the in-memory repository, including the section-14 acceptance flow |
| Addon tests (addon repository) | `tests/test_addon.lua` + `wow_stub.lua` | loads every addon file in `.toc` order inside a strict 3.3.5a API stub (reading an undefined global is an error; `SetSize`/`SetShown` raise errors because 3.3.5 lacks them), then drives the UI with simulated server traffic over the real wire format |
| Interop | `tests/test_interop.lua` (addon repository) + `tests/wire_tool.cpp` | Lua-encoded messages decoded by the C++ server codec and vice versa (UTF-8, `\|`, quotes, arrays) |
| SQL | `tests/test_sql.py` | applies the migration twice (idempotent), then runs **every SQL template extracted from `TrueCompanionSqlRepository.cpp`** plus semantic checks (upserts, gift replay `UNIQUE`, chat pruning) |

## Coverage

| Area | Automated tests | Needs a running server |
|---|---|---|
| Protocol, JSON, chunking, rate limits | `Protocol.*`, `Json.*`, addon + interop tests | |
| Relationship rules, proposals, marriage, breakups | `Relationship.*`, `Service.Proposal*`, `Service.MarriageCeremony…` | |
| Gift scoring, replay protection (incl. restart and DB `UNIQUE`) | `Gift.*`, `Service.GiftAwardedOnce…`, `test_sql.py` | trade detection (`CMSG_ACCEPT_TRADE` / `SMSG_TRADE_STATUS`) |
| Memory: dedup, retrieval, pruning, edit/delete, restart, scoping | `Memory.*`, `Service.Memory*` | |
| Prompt building and structured-output validation | `Prompt.*`, `Service.ModelCannotInvent…` | response quality of the chosen model |
| Ollama offline, timeouts, cancellation, model fallback | `Http.*`, `Service.OllamaOffline…`, `Service.TimeoutRecovery` | |
| Worker pool, backpressure, concurrent companions, threading | `Service.WorkerQueueSaturation…`, `Service.Concurrent…`, TSan run | world-thread placement of hooks |
| Command routing, other bots unaffected, proactive limits | `Text.*`, `Service.OnlySelected…`, `Service.Proactive…` | Playerbots command execution |
| Ownership, character list, summoning, chat hooks | | in-game checklist below |

The section-14 scenario (couple → ring → restart → remembers the ring) runs as `Acceptance.ElaraCoupleRingRestartRemember` against a scripted model.

## In-game acceptance checklist (section 14 scenario)

Run this once on your server. Expected results are in italics.

1. Start worldserver. *Log: `[mod-true-companion] enabled | Ollama on … | Playerbots found`.* `.companion status` → *Ollama online*.
2. Log in with a level-80 Human Paladin and type `/tc`. *The window opens and lists your other characters with level, race, class and status. A character logged in elsewhere is grey, with a tooltip explaining why.*
3. Select Elara → **Personality**: preset *Anime Girlfriend*, age 24, shy (outgoing ≈ 25), sweet, slightly playful. Style: natural, mostly short. Backstory: "Elara and I have known each other for years." **Save** → *"Saved."*
4. **Relationship** → stage *Dating / Couple* → **Set relationship** → confirm. *Stage shows Dating / Couple; affection and trust bars high.*
5. **Settings**: proactive on, frequency 50, model `qwen3.5:4b`, save.
6. **Companion** → **Call companion**. *Elara logs in as your Playerbot, joins your party, greets you in a whisper; the typing indicator shows while she thinks.*
7. Whisper "Hey Elara, ready for an adventure?" *A natural, in-character, short reply, with no "As an AI…". Also mirrored in the Chat tab.*
8. Whisper `follow` and `!stay`. *Playerbots executes the commands; no AI reply.* "Elara, follow me." *She follows and acknowledges it in character.*
9. Kill a few enemies. *She fights through Playerbots' normal combat AI. After the fight (not during it) she sometimes comments; a near-death moment always gets a reaction.*
10. Trade her a ring and both press Trade. *After the trade completes: a grateful reaction; affection rises; Relationship → Memories shows "… gave Elara Silver Ring … (gift, seen in game)".* Cancel a second trade halfway. *No reaction, no reward.*
11. Log out, restart worldserver, log in the next day, call Elara. *Same stage, same personality.* Ask "Do you still have the ring I gave you?" *She remembers it.*
12. Open **Debug → Advanced Memory…** *The ring memory is listed;* delete it. *It disappears; after `/reload` and reopening, it stays deleted.*
13. Add a second character as a normal bot (`.playerbots bot add Lyra`) and whisper it. *Normal Playerbots behaviour; Debug shows no extra request; Ollama logs show no request.*
14. Stop Ollama and whisper Elara. *Friendly notice "Elara seems lost in thought…"; the server keeps running; the status light turns red. Start Ollama again → green within ~30 s.*
