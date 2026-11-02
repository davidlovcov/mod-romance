# Changelog

## Unreleased (2.0.0)

### Renamed: mod-romance is now mod-true-companion
- The module, its files, config keys (`TrueCompanion.*`, file `mod_true_companion.conf`), tables (`true_companion_*`), log prefix and GM command (`.companion`) are renamed. Romantic relationships stay a per-companion option. Version 2.0.0, since the config and the addon (now **TrueCompanion**) change with it.
- Upgrading keeps everything: a new SQL update renames the `romance_*` tables (only when the old ones exist, so it is safe to run again), `scripts/migrate-from-mod-romance.sh` converts your config, and memory exports made before the rename can still be imported. Steps: docs/INSTALLATION.md, "Upgrading from mod-romance".

### Added
- "Let's sell trash", "we should sell our junk", "go sell your stuff at the merchant" are understood.
- "Learn your spells" / "go to your trainer": she walks to the nearest trainer of her class, learns the new spells she can afford with her own gold and comes back (right away when the trainer is next to her).
- With her AI conversation off, a companion still carries out commands and answers them in plain words (before: a plain Playerbot). Such companions don't count toward the companion limit; turning the AI on checks the limit.
- Without the AI she still thanks you for a gift ("Thank you for the 5 Linen Cloth.").
- Who leads also says when you lead and the companions clear the area with you ("attack all here"): `fighting`.
- Spells to learn: the character sheet lists what her class trainer can teach her now (with the total cost) and what comes in the next two levels.
- Who leads (`get_lead`, also in the Quests tab data): the leading companion, where to and how far, or that you lead; companions on an errand.
- "Lead me to <quest>" for a quest only you or another companion has finished (e.g. a rogue's "Rogue Training" asked of the warlock): she leads everyone to its quest giver; companions turn in their copy and she tells you what you can hand in there. Before: "only needs to be turned in".
- Works without Ollama: with `TrueCompanion.Ollama.Enable = 0`, or while Ollama is unreachable or fails, commands, lead mode, quests and the addon keep working, and she says what she did in plain words ("I took the lead ...", "I turned in ..."). Only free conversation needs the AI.
- Quests tab actions: "Share" gives a quest to every companion who lacks it and can take it, and a companion who has it shares it with you; "Take" sends the companions to take a quest available around (right away when the quest giver is near, else the leading or selected companion leads everyone there and they take exactly that quest). The tab shows who could still get each quest and who can take each available one.
- Group quests (for the addon's new Quests tab): the party's quests with who has which, the quests available around, a quest order for the whole group ("Apply" switches the leading companion right away), ignored quests (never picked up or led to) and "abandon for all companions".
- "Lead me to <quest>" for a quest that is already done: she leads you to turn it in.
- A lead you give one companion directly (whisper, the addon, or her name in party chat) takes over from the companion leading now; "lead the way" to everyone still picks just one. It also takes over from her own walk to a quest giver or merchant (before, she answered "still on my way" until she was back).
- After leading to turn in a finished quest (or to take one from the Quests tab) in the middle of another quest lead, she goes back to that quest instead of stopping.
- Quest level: "lead the way" and "pick up your quests" leave quests far above the group for later (the group's average level +2, +3 with three, +4 with five or more); grinding leaves such enemies alone too. A quest you name ("lead me to ...") or put first in the addon's quest order is always done.
- Quest order (addon Character tab): she leads to the first unfinished quest of your order. "Lead me to <quest>" while she leads switches to that quest.
- Companion setting "Wait for me to loot" (off by default): after a fight she also waits for your loot; off, she only loots her own and goes on.
- `conf/playerbots-recommended.conf`: the recommended Playerbots settings (only the changed ones) to copy into playerbots.conf, now including `Playerbots.SyncQuestWithPlayer = 0`; mod-true-companion warns at startup while it is 1.
- "Lead the way" without a quest: she leads you and the other companions to the quest givers around, everyone picks up what they can, and then she leads you to one of the new quests (kill and item-drop quests first). With nothing to pick up she turns in finished quests or grinds the creatures nearby.
- "Give me all your money" (gold, copper, coins): she hands over everything she has, up to `TrueCompanion.Control.MaxGoldPerRequest`.
- Companions know which quests they could pick up around you (quest givers within ~250 yards, with direction, distance and class-quest marker); ask them or see "Available nearby" on the Character tab.
- "Pick up your quests" / "go get your class quest": the companion walks to the nearest quest giver with something for them (e.g. their class trainer), picks the quests up and comes back.

### Fixed
- Amounts: "give me 1.5 gold" gave 5 gold, "give me -5 gold" gave 5, "give me 1,000 gold" asked for an item called "000 gold", and "sell 0 linen cloth" sold all of them. Amounts that are not whole positive numbers are refused now, and nothing changes.
- Selling removed the items before the money was paid; at the gold cap (or a trial account's cap) they were gone without payment. The money is checked first; nothing is sold when she can't take it, and the reply says what she really got.
- Quest sharing (addon "Share") gave a quest to the companions even when nobody in the group had it. Now somebody must hold it, as with the game's own sharing; it reaches companions on the same map near that person.
- Class trainers: when two trainers teach the same spell, the one that suits her character is used (before, only one of them was kept, so a valid trainer could be missed); "learn your spells" walks only to a trainer that has something for her, and trainer changes are seen without a restart.
- Quests are picked up and turned in the way the core's quest handler does it: from the real quest giver she stands at (its accept scripts run: escort starts, events), with the chosen reward validated (it must fit in her bags, like for a player) and the quest ender's reward scripts afterwards. Quests shared within the group (quest sync, Share) stay shares without a quest giver.
- Dismissing a companion without logging her out left her lead, her place behind another leader or in an area clearing half set up (Playerbots' follow could stay off). Dismiss now ends all of it like a finished lead: followers and chasers get their follow back, her movement stops, she follows you as a plain Playerbot. Switching the AI conversation off does not end commands, leads or errands.
- "Near" checks (gold handover, quest sync, followers, turn-ins together, the lead's separation check) now require the same instance and phase and no teleport in progress; coordinates in another copy of a dungeon no longer count as near. A lead ends when she ends up in another instance of the same map.
- Export: page 0 freezes the export; later pages come from that snapshot (id sent with every page, 60 s lifetime, released after the last page), so memories that change in between can no longer produce a mixed export with duplicated or missing memories.
- A full reset during a memory summary stopped all later summaries for that session (the "summarizing" flag stayed set). Reset and reconfigure now share one cancellation that clears every in-flight flag.
- Protocol: an empty chunk sent twice was counted twice, so a message could be dispatched before its last part arrived.
- Chat without the AI (Ollama off, or her AI conversation off) answered with a bare "ai_disabled"; it now says why and that commands still work.
- Spells "soon": spells that need a talent she doesn't have (or a rank she can't learn yet) are left out.
- Companion commands that start with a Playerbots command word ("sell your junk", "Vendor, sell trash", "attack", "accept the quest", "go on") went to Playerbots instead: nothing was sold or accepted and the companion didn't answer. mod-true-companion now handles them; `!` still sends a line to Playerbots unchanged.
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
- Party chat lines could open a trade window ("how many more to kill?"): that is Playerbots' `EnableAutoTradeOnItemMention`. mod-true-companion now warns about it at startup; set `Playerbots.EnableAutoTradeOnItemMention = 0` (see docs/INSTALLATION.md).
- mod-true-companion's own bot commands respect `Playerbots.CommandPrefix` when one is set.
- The Ollama connection no longer depends on exact header spacing or case in the server's reply (`Transfer-Encoding:chunked`, `CONTENT-LENGTH:  123`); a malformed `Content-Length` is ignored instead of misread.
- The Character tab lagged when opened (and hitched on every refresh): finding nearby quest givers walked every creature spawn in the world each time. The quest-giving and quest-ending spawns are now indexed once per map, which also speeds up "pick up your quests", "turn in your quests" and quest questions.
- Lead mode edge cases (see docs/LEAD_MODE.md): a dismissed or kicked leader no longer leaves the followers without Playerbots' follow; "follow me" to a follower makes her follow you during a lead; a follower back from her own errand walks behind the leader again; a lead ends when you hearth / teleport / enter an instance; quests without a known place are skipped instead of blocking the round; quests with nothing to fight, unreachable spots and spots without free creatures are skipped for the round instead of ending it; quest givers that can't be used are not visited again and again; "Apply order" no longer interrupts turn-ins; "lead me to" works for quests only you (or another companion) have; the round uses the whole group's quests and turns in every companion's finished quests; follow is switched back on when a companion is called (a lead before a restart could leave it off).
- "Lead the way" round order: every quest first (in the order of her quest log as the Character tab shows it, or your quest order), and only when all are done she leads you to turn them in; at each quest giver she turns in first and then takes the new quests there, then the other quest givers around. Before, she went off to turn in after each quest, and her own priority ignored the list order.
- During a round she leads everyone to the nearest merchant when bags are filling up with junk (4 or fewer free slots, or 8+ gray items) and all sell there. When she isn't leading, she tells you her bags are almost full (from 4 free slots).
- Quests that scale with your level showed "[-1]"; they show her level now.
- Restarting "lead the way" when her quest was done but yours (or another companion's) not: she went to turn hers in. A quest counts until everyone in the party who has it is done, so she leads you back to it.
- She still flew up onto buildings: the height of every step was taken from far above her (roofs). It is now the ground near her own height.
- Looting: out of combat the companions now loot every corpse of theirs before the next enemy is marked (a new fight interrupts, looting goes on after it), instead of a fixed short pause.
- A companion walking to a quest giver up on a building flew there in a straight line and got stuck on the roof: movement now only follows real paths and stops as close as it gets; talking range (12 yards) is enough to pick up or turn in.
- Questing together: when you or the other companions have the quest she leads to, she keeps fighting there until everyone has finished it (also the items each of you still needs), then leads you all to turn it in. Alone she goes as soon as she is done.
- Quests she picks up during a "lead the way" round are shared with you (when you can take them; otherwise she just goes on). The companions who came along take them directly.
- A "lead the way" round stopped after the first quest: once it was turned in she went back to following. The round now goes on: the next quests (e.g. the follow-up from the same quest giver) are picked up and she leads you to them.
- After a quest she led you through, only she turned it in; the companions who came along now walk with her to the quest giver and turn in their finished quests too.
- A companion walking behind the leading one knows it ("walking behind Sarian, who leads the way") and the Character tab shows it.
- "Lead the way" in party chat (to all companions) made two of them lead, to different places. One leads now; a quest giver or turn-in round counts as leading too. The other companions walk behind the leading one (not just after you), and pick up the same quests at each quest giver.
- A leading companion no longer drops the lead for a "pick up / turn in your quests" meant for another one; a companion that goes on her own errand leaves an area clearing first.
- "Attack all here" ignored creatures marked "civilian" in the world database (Springpaw Cubs, ...). Like Playerbots' grind it now attacks them too; friendly townsfolk are never attacked.
- Item-drop quests never finished: right after a kill the next enemy was marked, so the companions stayed in combat and never looted (no Lynx Collars). There is now a short pause after every kill and the bots are told to loot.
- Kicking a companion from the party (or disbanding it) left her summoned and following you; she is dismissed now, like with "Dismiss".
- Lead mode skipped the creatures some quests need: Springpaw Cubs and Lynxes (Lynx Collars) and other creatures marked "civilian" in the world database were never marked or attacked, so she walked from spot to spot and then went back to following. Civilian creatures are attacked now.
- Lead mode still ended when she reached the quest mobs if none of them was free (in busy starting areas they are often tagged by other players or bots): she now leads on to the next spawn spot, up to four times, before giving up. Every lead start and end and every "no target left" is written to the server log with the reason (`[mod-true-companion] ... stops leading: ...`).
- Walking closer to an enemy out of sight now goes through Playerbots' own movement ("go"), which its strategies don't cancel.
- Lead mode dropped back to following right when she reached the enemies (and sometimes right after "lead the way"): the AI's own reply could contain a "follow" action, which was passed to Playerbots. While she leads, runs an errand or clears an area, the AI's movement actions are ignored now; your own "follow me" ends a lead or an area clearing properly.
- Enemies out of sight: the companions now really walk closer (Playerbots' follow pulled them straight back before), also for a single "attack" on your target; they attack as soon as they see it.
- An import sent one "memory changed" message per memory, and the addon reloaded its lists for each one ("get_memories: Too many requests" flood). One message per import now.
- "Give me 5 copper" at a low relationship stage: she is not allowed to hand things over yet (`TrueCompanion.Control.GiveMinStage`, default: friends) but the AI sometimes said "here you go" anyway. The server now says so in the chat, and the prompt says plainly that nothing was given.
- "Springpaw Cub is not in my sight" / "attack rti target: failed": Playerbots refuses to attack what it cannot see (or what it is already attacking). Attack orders now go out only for enemies in line of sight and not twice; when the next enemy is out of sight, the companions walk closer first and skip it after 20 seconds.
- Lead mode and area clearing ignored quest mobs that count through a kill-credit creature (many "kill N wolves" quests), so she found no enemies at the destination and went back to following. These mobs are found now, also when choosing where to walk.
- When a quest she is leading you through is finished, she now walks to the quest giver, turns it in and comes back (before: she only said it was done).
- "Stop attacking" now also stops a single attack ("attack" on one enemy), drops her target and makes her follow again; before it only cancelled "attack all here".
- "Sell 3 of your linen cloth" sold the whole stack: the amount is respected now, the rest stays in the bag.
- "Pick up your quests" now goes to every quest giver around (not only one); "pick up a quest" takes the nearest quest only.
- "Give me 5 copper" is verified now: if the gold does not arrive, she says so and the server log has a line about it (`gold handover failed`).
- The server version is shown in the startup log and in the addon's Settings tab (`2.0.0-dev` while on develop).
- Opening the Character tab for the first time still lagged: the quest-giver index is now built at server start.
- Quests with a choice of rewards always gave the companion the first item. She now picks for herself: gear she can use that upgrades what she wears comes first (in her best armor type: cloth, leather, mail or plate), then other usable gear, then potions and bags, and last things she cannot use. Applies to turn-ins by command, quest sync and fetch errands.
- Selling an item by name reported the whole stack even if part of it was skipped (e.g. in a trade window); it now reports what was actually sold.

- An error thrown inside a model request left the companion "still answering" forever, and a failing health check stopped all further checks.
- Your own memories (memory viewer, import) are never pruned, so they could grow without limit. They now count towards `TrueCompanion.Memory.MaxPerCompanion`; a full store refuses new ones with a clear message (`memory_limit_reached`).
- "Can you update me on the quest?" was taken as a dating proposal ("date me"); proposals now need whole words. Word forms such as "propose", "fiancée", "girlfriends", "heiraten" or "verloben" now count as talking about the relationship.
- Messages starting with "..." ("...I missed you") were passed to Playerbots as a dot command instead of reaching her.
- The chat tab kept showing "is thinking..." after a line that went to Playerbots (e.g. `follow`); the indicator also went off after the first answer although a second message was still being answered.
- Character names with umlauts or other non-ASCII letters were cut off when saved (the 12-letter limit was applied to bytes).
- Huge numbers in addon messages (e.g. `"age": 1e300`) caused undefined behaviour when converted; they are now clamped. Non-integer numbers are no longer rounded to 6 digits in JSON.
- Ollama URLs with an IPv6 address (`http://[::1]:11434`) did not work; ports with trailing characters (`11434x`) were accepted; absurd chunk sizes in HTTP answers are rejected.
- A trailing `|` in a player's message was passed on; a model answering with `"action": "leave"` is now flagged (leaving is only decided by the relationship engine).
- A completed quest without a title was remembered as `completed the quest ""`.
- A promise and an ordinary memory with the same words were merged into one, losing the promise.
- `delete_memory` for an unknown id answered without an error code.

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
- **Companion control** in plain sentences: give items or gold (she opens the trade), cast a spell on you or your target, attack an enemy by name or creature type (skull mark), sell junk or an item at a vendor, share quests, lead the way to one of her quests (experimental). Auto-sells junk at a vendor when bags are nearly full. `TrueCompanion.Control.*` options. See [docs/COMMANDS.md](docs/COMMANDS.md).
- **Several companions** at once (default 3); party messages go to the companion you name, or to the one you talked to last when only bots are in your group.
- **Presets:** every built-in preset is a complete personality; 7 male presets next to the 7 female ones; custom presets per account (`true_companion_preset` table, `TrueCompanion.Presets.MaxCustomPerAccount`).
- **Estrangement:** constant mistreatment makes her leave; she refuses calls for 72 h (longer each time) and comes back cold; reconciliation takes several days. `TrueCompanion.Estrangement.*` options.
- `get_character_sheet`, `list_presets`, `save_preset`, `delete_preset` addon ops; failed acks carry a readable `message`.
- Party chat: every companion you name answers; "everyone"/"all of you"/"guys" reach all; unaddressed lines reach all companions when only bots share your group (`TrueCompanion.Party.UnaddressedReplies`). Each companion knows who else is travelling with her.
- "Attack all here" / "clear the area" / "kill all wolves": one enemy after another until the area is clear; "stop attacking". "Attack" without a target picks the nearest enemy.
- Quests without targeting: "accept the quest", "turn in your quests"; companions take and turn in quests together with you (`TrueCompanion.Control.QuestSync`).
- Selling works with a merchant within 30 yards, no targeting (`TrueCompanion.Control.InteractRange`).

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
- `.companion status|summon|dismiss|on|off` chat commands and `mod_true_companion.conf.dist` with 70+ documented options.
- 75 C++ tests (including the full couple → ring → restart → remembers flow) and an SQL test against MySQL/MariaDB.

### Known limitations
- Gifts sent by mail aren't recognized; only completed trades count.
- If mod-ollama-chat is installed as well, it may also answer whispers to the companion.
- Party-chat roleplay lines are still visible to Playerbots' own party-command parser (whispers are not).
