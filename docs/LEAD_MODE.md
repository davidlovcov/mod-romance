# Lead mode and quest rounds

How a companion leads the group, does quests, turns them in and finds new ones. This is the reference for
the behaviour and the cases it handles; check new changes against it before implementing.

## Roles

- **Leader**: the one companion leading the player. There is at most one leader per player.
- **Followers**: the other companions in the group. While someone leads, they walk a few yards behind the
  leader (mod-romance moves them; Playerbots' own follow is switched off for that time).
- **Errand**: a companion walks alone to a quest giver ("pick up your quests", "turn in your quests") and
  comes back. An errand is not leading: another companion can lead meanwhile.

## Starting and stopping

| Said to her | Result |
|---|---|
| "lead the way" (no quest named) | A **round** (below). |
| "lead me to <quest>" | A single lead to that quest. The quest may be hers, the player's or another companion's. A finished quest: she leads to its quest giver to turn it in. |
| "lead ..." to everyone in party chat | One companion leads, the others say so and follow her. |
| "lead ..." to one companion directly (whisper, the addon, or her name in party chat) | She takes over; the old leader becomes a follower. |
| "lead ..." while she walks to a quest giver / merchant (errand) | Named quest or told directly: the errand is dropped and she leads (finished quests are turned in later). A plain "lead the way" to everyone: she finishes the errand first. |
| "lead me to <finished quest>" (or Take) while she leads to another quest | She leads to the quest giver, then back to the quest she was leading to (if someone still needs it). |
| "wait" / "go on" | Pause / continue (the lead is kept). |
| "stop leading", "follow me" | The lead ends; everyone follows the player. "follow me" to a follower: only she follows the player from then on. |

## A round, step by step

After every step (a quest finished, a quest giver visited, a merchant visited) the leader decides the next
one, in this order:

1. **Merchant**: a companion's bags are nearly full of junk (4 or fewer free slots, or 8+ gray items) -> lead
   everyone to the nearest merchant, all sell. (At most every 3 minutes.)
2. **Next quest**: the first quest in the list that someone in the group still needs and that she can lead to.
   - The list: the group's quest order from the addon (Quests tab); otherwise her quest log, then the player's,
     then the other companions'.
   - Left out: ignored quests, quests far above the group (average level +2/+3/+4) unless the player put them
     in the order, quests with nothing to lead to, and quests skipped earlier in this round.
   - A quest counts until everyone in the group who has it is done (each loots their own items).
3. **Turn in** (only when no quest is left): lead to the quest giver of a quest a companion has finished. There:
   everyone turns in, then everyone takes the new quests of that quest giver. Then the next quest giver.
4. **New quests**: lead to the quest givers around and pick up (also the followers; the player gets the quests
   shared).
5. **Grind**: nothing else to do -> fight the creatures around (not far above the group's level).

Quest givers and turn-in NPCs visited in the current phase are not visited again until a new quest starts, so a
quest that can't be taken or turned in never causes a loop.

## Quests tab actions

| Action | Behaviour |
|---|---|
| Share | Every companion who lacks the quest and can take it gets it; you get it shared (accept window) from a companion who has it. Quests that can't be shared: nothing happens, the tab says so. |
| Take | The companions who can take the quest take it: right away when its quest giver is within 25 yards, else the leading companion (or the selected one, or the first who can take it) leads everyone there and they take exactly that quest; you get it shared. A running round goes on afterwards. Taking an ignored quest removes it from the ignore list. |
| Lead | The selected companion leads to that quest now (a finished one: to turn it in). |
| Ignore | Never picked up or led to. |
| Abandon | Dropped by every companion (not by you). |

## Fighting and looting

- At the quest area the group fights the quest creatures (also ones marked "civilian", like Springpaw Cubs).
  Enemies out of sight: they walk closer first.
- After each kill: loot. No new target until the corpses are looted (12 s per corpse; with "Wait for me to loot",
  also the player's share, up to 60 s). A new fight interrupts, looting continues after it.
- No free creatures at a spot (tagged by others, all dead): up to 4 other spawn spots are tried.

## Cases

| Case | What happens |
|---|---|
| Quest has nothing to fight (use objects, talk to someone) | She leads to the area and says what needs doing. In a round she then goes on (the quest is skipped for this round). |
| No known place for the quest on this continent | Skipped; the next one in the list is tried. |
| No path / stuck, or no free creatures after 4 spots | Outside a round: she stops and follows. In a round: skipped for now, on to the next step. |
| The quest is abandoned (Quests tab) or turned in by someone else | The lead to it ends; in a round: on to the next step. |
| "Apply order" while she leads to a quest | She switches to the new first quest. While on the way to a quest giver or merchant: that finishes first. |
| Player hearths / teleports / enters an instance | The lead ends; Playerbots brings the companions along. |
| Player falls behind (> 40 yards) | The leader waits until they are within 20 yards. |
| Leader or player dies | The leader waits; the lead goes on after the resurrection. Errands give up after 4 minutes (10 in a round). |
| Leader is dismissed or kicked | The followers get Playerbots' follow back. |
| Companion called again (also after a server restart) | Playerbots' follow is switched on again. |
| Quest log full, a quest needs something first | Not picked up; the quest giver is not visited again in this phase. |
| AI reply contains "follow" / "stay" / "attack" while leading or fighting | Ignored. |

## Logs

Every decision is logged at INFO (`[mod-romance] ...`): `leads to "<quest>"`, `leads to quest giver`,
`leads to <NPC> to turn in`, `leads to merchant`, `no target left (...)`, `no free quest mobs here, moving on`,
`stops leading: <reason>`.
