# Commands

Everything you can say or type to control your companion. Examples use a female companion, **Elara**; male companions understand exactly the same. Use your companion's name (the one from the Personality tab, or the character name).

## Where you can talk to her

| Channel | When she answers |
|---|---|
| Whisper (`/w Elara ...`) | Always, if she is your active AI companion. |
| Addon chat (Chat tab) | Always. |
| Party / raid chat | Every companion you name answers ("Jina, ...", "Jina and Tristi, ..."). "Everyone", "all of you", "guys", "you two" reach all of them. Lines without a name reach all of them too when only bots share your group (`Romance.Party.UnaddressedReplies = last` makes only the last one you talked to answer). Orders work the same way: "everyone, attack all here". Can be turned off per companion on the Settings tab ("Listen in party chat"). |

## Chat commands

| Command | What it does |
|---|---|
| `.romance status` | Module and Ollama status, your active companions. |
| `.romance summon <name>` | Calls one of your characters as an AI companion (same as "Call companion" in the addon). |
| `.romance dismiss` | Sends all your active companions home (they log out). |
| `.romance on` / `.romance off` | Turns the AI on or off for the bot you have **targeted**. With the AI off, it is a normal Playerbot. |

## Addon slash commands

| Command | What it does |
|---|---|
| `/romance` or `/ar` | Opens or closes the window. |
| `/romance status` | Prints module and Ollama status. |
| `/romance minimap` | Shows or hides the minimap button. |
| `/romance reset window` | Moves the window back to the center of the screen. |

## Things to say in chat

You don't need exact wording. "can you", "please", her name and similar are ignored. Questions ("what spells do you have?") are answered and never run as commands. Negations ("don't attack") never trigger anything.

### Ask about her

She answers from **real game data**, never invented:

| Ask | Example |
|---|---|
| Bags / inventory | "Hey, what do you have in your inventory?", "Do you have any linen cloth?" |
| Gold | "How much gold do you have?" |
| Level / XP | "How much XP until you level up?" |
| Gear | "What armor are you wearing?" |
| Spells | "Which spells do you know?" |
| Quests | "What quests are you on?" (with progress, and which ones you share) |
| Your target | "Who am I looking at?", "What is that?" |

The **Character** tab in the addon shows the same data at a glance.

### Give orders

| Order | Example | What happens |
|---|---|---|
| Follow / stay | "Follow me", "Wait here" | Playerbots `follow` / `stay`. |
| Give an item | "Give me your linen cloth", "Can I have 5 copper ore?" | She opens a trade and puts the item in. You only accept. Soulbound items can't be traded. |
| Give gold | "Give me 10 gold", "I need 2g 50s" | Trade with the gold. Limited per request (`Romance.Control.MaxGoldPerRequest`). |
| Cast a spell | "Cast Power Word: Fortitude on me", "Use Frost Nova" | Casts on you ("on me"), on your target, or on herself. She tells you if she doesn't know the spell or it is on cooldown. |
| Attack | "Attack the wolf", "Kill those murlocs", "Attack beasts", "Attack my target", "Attack that" | Finds the nearest matching enemy by name or creature type (beast, undead, humanoid, demon, elemental, dragonkin, giant, mechanical, critter), marks it with a skull and attacks it. You don't need to target anything: without a target she takes the nearest enemy. |
| Clear the area | "Attack all here", "Kill everything nearby", "Clear the area", "Kill all wolves", "Everyone, kill them all" | One enemy after another (skull mark), until none are left around you; then she tells you. All companions you addressed fight the same target. Critters and other players' fights are left alone. Enemies she can see come first; one nobody can reach (behind a wall) is skipped after 15 seconds. **Healers** (bots with a heal strategy) don't attack; Playerbots doesn't allow it. They keep everyone alive instead. |
| Stop | "Stop attacking", "Stop fighting" | Stops clearing the area (the current fight still ends normally). |
| Retreat | "Run away", "Fall back" | Playerbots `flee`. |
| Sell | "Sell your junk", "Vendor, sell trash", "Sell your wolf meat" | Works when a merchant is within about 30 yards (`Romance.Control.InteractRange`); no need to target him or walk up to him. With nearly full bags she sells gray items by herself near a merchant (`Romance.Control.AutoSellJunk`). |
| Take quests | "Accept the quest", "Everyone, take the quests too", "Accept the quest Kobold Camp Cleanup" | Takes quests from quest givers within about 30 yards, no targeting needed: first the ones you have taken, otherwise everything offered there. She tells you if she can't take one (level, class, earlier quest missing). |
| Pick up her own quests | "Pick up your quests", "Go get your class quest", "Fetch your quests" | If no quest giver right here has something for her, she walks to the nearest one within about 250 yards that does (e.g. her class trainer), picks the quests up and comes back to you. "class quest" only takes class quests. |
| What's available | "What quests can you pick up here?" | She lists quest givers around you with quests *she* can take, with direction and distance; class quests are marked. The Character tab shows the same list under "Available nearby". |
| Turn in quests | "Turn in your quests", "Hand in the quest" | Turns in finished quests at quest enders nearby. |
| Automatic | (nothing to say) | When you take a quest, companions near you take it too; when you turn one in, they turn theirs in as well (`Romance.Control.QuestSync`). |
| Share quests | "Share your quests", "Share the quest Kobold Camp Cleanup" | Shares the quests you don't have yet and can take, one per second. |
| Lead | "Lead the way", "Lead me to Kobold Camp Cleanup", "Let's do the Fargodeep Mine quest" | She walks to the area of one of her quests and you follow (you don't need to make her group leader). Without a quest name she picks one you both have. She waits when you fall behind, fights the quest creatures when she arrives, and follows you again when the objectives are done, telling you to turn it in. Quests without creatures to kill: she takes you to the area and tells you what to look for. |
| Pause / continue | "Wait", "Hold on" … "Lead again", "Go on", "Keep going" | While leading, "wait" makes her stop where she is without giving up the lead; "lead again" (or "go on") continues to the same quest. When she isn't leading, "wait" is the normal Playerbots stay. |
| Stop leading | "I'll lead", "Stop leading" | She follows you again. |

Giving items or gold needs at least the relationship stage **Friends** (`Romance.Control.GiveMinStage`). Before that she politely refuses.

Everything she does is reported back to the model, so her answer matches what really happened ("I don't have any linen cloth", "There's no merchant here").

### Playerbots commands

Normal Playerbots commands still work and are passed through untouched, e.g. `follow`, `stay`, `co +dps`, `nc +loot`, `!stay`. A single-word command such as `attack` goes straight to Playerbots, while a sentence ("attack the wolf") is handled by the companion.

## Relationship

| Say | What happens |
|---|---|
| "Will you be my girlfriend?" / "Be my boyfriend?" | Dating proposal. She decides based on your relationship. |
| "Will you marry me?" | Engagement (or marriage when you are engaged). A ring gift helps. |
| "I want to break up" | Ends the romance. Always respected. |
| Gifts | Trade her an item (or gold). It only counts once the trade is completed. |

**Treat her well.** Insults, rudeness and broken promises build resentment. If it gets too bad, she leaves you: she says goodbye and logs out, and won't come back for 72 hours (longer if it happens again). After that she returns cold and distant. Only sincere apologies and kindness over several days win her back, and one more insult makes her leave again. See `Romance.Estrangement.*` in [CONFIGURATION.md](CONFIGURATION.md).
