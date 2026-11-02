# Changelog

## Unreleased

### Added
- "Lead the way" without a quest: she leads you and the other companions to the quest givers around, everyone picks up what they can, and then she leads you to one of the new quests (kill and item-drop quests first). With nothing to pick up she turns in finished quests or grinds the creatures nearby.
- "Give me all your money" (gold, copper, coins): she hands over everything she has, up to `Romance.Control.MaxGoldPerRequest`.
- Companions know which quests they could pick up around you (quest givers within ~250 yards, with direction, distance and class-quest marker); ask them or see "Available nearby" on the Character tab.
- "Pick up your quests" / "go get your class quest": the companion walks to the nearest quest giver with something for them (e.g. their class trainer), picks the quests up and comes back.

### Fixed
- Companion commands that start with a Playerbots command word ("sell your junk", "Vendor, sell trash", "attack", "accept the quest", "go on") went to Playerbots instead: nothing was sold or accepted and the companion didn't answer. mod-romance now handles them; `!` still sends a line to Playerbots unchanged.
- "Give me 1 gold" said yes but gave nothing: gold is now handed over directly (within 30 yards), no trade window.
- "Turn in your quests" only worked right at the quest ender: she now walks to the quest ender (up to ~250 yards), turns the quests in and comes back.
- Lead mode: only one companion leads (the others follow the group), every bot of the group fights at the destination, quests that need item drops ("bring 8 Lynx Collars") send her to the creatures that drop them, and she no longer "arrives" at the turn-in marker instead of the quest area. A quest errand (fetching or turning in) is no longer mistaken for a lead.
- Clearing an area / attacking no longer whispers "rti set to: skull" (the raid icon is set directly).
- Companions in the same party were sometimes given the wrong gender: each companion now knows the others' gender, race and class.
- Duplicate memories that were stated by you or imported were never tidied up: identical ones are merged now, and an automatic memory that repeats one of yours is merged into yours.
- Large imports ran into "Too many requests, slow down": the addon now sends at a steady pace below the server's limit (and the limit is a bit higher).
- Characters of the other faction could be called as companions (a Horde player could take an Alliance character into the group). They are now shown as "Other faction" and can't be called, unless `AllowTwoSide.Interaction.Group` is on in worldserver.conf.
- A reply that was still queued (or in flight) when a companion was dismissed could appear in the next session after the companion was called again, and change the relationship. Every activation now gets its own generation, and cancelled requests never change anything.
- The addon Chat tab skipped companion commands (give, cast, sell, quests, ...) and game knowledge; whisper, party and addon chat now share one path.
- Export / import failed for larger memory sets (one message was limited to 16 KiB): exports and imports now travel in pages and an import is applied only when every part arrived.
- Build with clang and warnings as errors (AzerothCore CI): unused parameters.
- Tests include the standard headers they use (GCC 14).
- Party chat lines could open a trade window ("how many more to kill?"): that is Playerbots' `EnableAutoTradeOnItemMention`. mod-romance now warns about it at startup; set `Playerbots.EnableAutoTradeOnItemMention = 0` (see docs/INSTALLATION.md).
- mod-romance's own bot commands respect `Playerbots.CommandPrefix` when one is set.
- The Ollama connection no longer depends on exact header spacing or case in the server's reply (`Transfer-Encoding:chunked`, `CONTENT-LENGTH:  123`); a malformed `Content-Length` is ignored instead of misread.
- The Character tab lagged when opened (and hitched on every refresh): finding nearby quest givers walked every creature spawn in the world each time. The quest-giving and quest-ending spawns are now indexed once per map, which also speeds up "pick up your quests", "turn in your quests" and quest questions.
- "Lead the way" in party chat (to all companions) made two of them lead, to different places. One leads now; a quest giver or turn-in round counts as leading too. The other companions walk behind the leading one (not just after you), and pick up the same quests at each quest giver.
- A leading companion no longer drops the lead for a "pick up / turn in your quests" meant for another one; a companion that goes on her own errand leaves an area clearing first.
- "Attack all here" ignored creatures marked "civilian" in the world database (Springpaw Cubs, ...). Like Playerbots' grind it now attacks them too; friendly townsfolk are never attacked.
- Item-drop quests never finished: right after a kill the next enemy was marked, so the companions stayed in combat and never looted (no Lynx Collars). There is now a short pause after every kill and the bots are told to loot.
- Kicking a companion from the party (or disbanding it) left her summoned and following you; she is dismissed now, like with "Dismiss".
- Lead mode skipped the creatures some quests need: Springpaw Cubs and Lynxes (Lynx Collars) and other creatures marked "civilian" in the world database were never marked or attacked, so she walked from spot to spot and then went back to following. Civilian creatures are attacked now.
- Lead mode still ended when she reached the quest mobs if none of them was free (in busy starting areas they are often tagged by other players or bots): she now leads on to the next spawn spot, up to four times, before giving up. Every lead start and end and every "no target left" is written to the server log with the reason (`[mod-romance] ... stops leading: ...`).
- Walking closer to an enemy out of sight now goes through Playerbots' own movement ("go"), which its strategies don't cancel.
- Lead mode dropped back to following right when she reached the enemies (and sometimes right after "lead the way"): the AI's own reply could contain a "follow" action, which was passed to Playerbots. While she leads, runs an errand or clears an area, the AI's movement actions are ignored now; your own "follow me" ends a lead or an area clearing properly.
- Enemies out of sight: the companions now really walk closer (Playerbots' follow pulled them straight back before), also for a single "attack" on your target; they attack as soon as they see it.
- An import sent one "memory changed" message per memory, and the addon reloaded its lists for each one ("get_memories: Too many requests" flood). One message per import now.
- "Give me 5 copper" at a low relationship stage: she is not allowed to hand things over yet (`Romance.Control.GiveMinStage`, default: friends) but the AI sometimes said "here you go" anyway. The server now says so in the chat, and the prompt says plainly that nothing was given.
- "Springpaw Cub is not in my sight" / "attack rti target: failed": Playerbots refuses to attack what it cannot see (or what it is already attacking). Attack orders now go out only for enemies in line of sight and not twice; when the next enemy is out of sight, the companions walk closer first and skip it after 20 seconds.
- Lead mode and area clearing ignored quest mobs that count through a kill-credit creature (many "kill N wolves" quests), so she found no enemies at the destination and went back to following. These mobs are found now, also when choosing where to walk.
- When a quest she is leading you through is finished, she now walks to the quest giver, turns it in and comes back (before: she only said it was done).
- "Stop attacking" now also stops a single attack ("attack" on one enemy), drops her target and makes her follow again; before it only cancelled "attack all here".
- "Sell 3 of your linen cloth" sold the whole stack: the amount is respected now, the rest stays in the bag.
- "Pick up your quests" now goes to every quest giver around (not only one); "pick up a quest" takes the nearest quest only.
- "Give me 5 copper" is verified now: if the gold does not arrive, she says so and the server log has a line about it (`gold handover failed`).
- The server version is shown in the startup log and in the addon's Settings tab (`1.1.1-dev` while on develop).
- Opening the Character tab for the first time still lagged: the quest-giver index is now built at server start.
- Quests with a choice of rewards always gave the companion the first item. She now picks for herself: gear she can use that upgrades what she wears comes first (in her best armor type: cloth, leather, mail or plate), then other usable gear, then potions and bags, and last things she cannot use. Applies to turn-ins by command, quest sync and fetch errands.
- Selling an item by name reported the whole stack even if part of it was skipped (e.g. in a trade window); it now reports what was actually sold.

### Changed
- Documentation and config descriptions use neutral wording, since companions can be male or female.
- A companion's reply reaches the addon before the chat line, and lines that go to Playerbots without an AI reply are announced (`bot_feedback`), so the addon can hide Playerbots' command feedback (feature `botFeedback`).
- docs/INSTALLATION.md lists the recommended Playerbots settings for a quiet chat.
- Clearing an area with a healer companion spammed "attack rti target: useless": Playerbots never lets healers take attack orders. Healers now keep healing while the others fight (and say so), and attack orders are re-sent at most every 5 seconds.
- Clearing an area picks enemies she can see first; an enemy nobody reaches within 15 seconds (behind a wall, no path) is skipped.
- Lead mode: saying "wait" made her give up leading (the Playerbots stay held her in place and the lead controller took that for being stuck). "Wait" now pauses the lead, and "lead again", "go on" or "keep going" continues it.

## 1.1.0 — 2026-11-01

### Added
- **Game knowledge:** ask her about her bags, gold, XP, gear, spells, quests (with progress and which you share) or your target; she answers from real game data.
- **Companion control** in plain sentences: give items or gold (she opens the trade), cast a spell on you or your target, attack an enemy by name or creature type (skull mark), sell junk or an item at a vendor, share quests, lead the way to one of her quests (experimental). Auto-sells junk at a vendor when bags are nearly full. `Romance.Control.*` options. See [docs/COMMANDS.md](docs/COMMANDS.md).
- **Several companions** at once (default 3); party messages go to the companion you name, or to the one you talked to last when only bots are in your group.
- **Presets:** every built-in preset is a complete personality; 7 male presets next to the 7 female ones; custom presets per account (`romance_preset` table, `Romance.Presets.MaxCustomPerAccount`).
- **Estrangement:** constant mistreatment makes her leave; she refuses calls for 72 h (longer each time) and comes back cold; reconciliation takes several days. `Romance.Estrangement.*` options.
- `get_character_sheet`, `list_presets`, `save_preset`, `delete_preset` addon ops; failed acks carry a readable `message`.
- Party chat: every companion you name answers; "everyone"/"all of you"/"guys" reach all; unaddressed lines reach all companions when only bots share your group (`Romance.Party.UnaddressedReplies`). Each companion knows who else is travelling with her.
- "Attack all here" / "clear the area" / "kill all wolves": one enemy after another until the area is clear; "stop attacking". "Attack" without a target picks the nearest enemy.
- Quests without targeting: "accept the quest", "turn in your quests"; companions take and turn in quests together with you (`Romance.Control.QuestSync`).
- Selling works with a merchant within 30 yards, no targeting (`Romance.Control.InteractRange`).

### Fixed
- After renaming a companion and saving, she sometimes still introduced herself with the character's name: saves are now visible immediately (write-through cache), and the prompt says her chosen name wins over the name shown in the world.
- She talked about the player in the third person ("Devid seems tired"); she now speaks to you directly.
- "Significant moments" only refreshed after switching tabs: the server now pushes `memory_changed` after every memory edit, delete, clear and reset.
- Party chat: she didn't answer when you wrote to the group without her name, even when only bots were in it.
- Server texts no longer assume a female companion.
- Duplicate memories: the same thing in other words is merged across kinds (an "episode" and a "preference" saying the same), and a memory contained in another ("completed X" / "completed X together and sold items") is merged too. Her impressions of the current moment ("currently...", "they just...") are no longer stored, and the model is told not to remember quests, fights or loot (the server records those). Existing memories are tidied once when the companion is activated; pinned and player-written memories are never removed.
- Lead mode didn't move: Playerbots' RPG mode ignores bots that have a master. She now walks to the quest area herself (step by step with path finding), waits for you, and fights the quest creatures there.
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
