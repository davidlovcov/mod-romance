# Commands

Everything you can say or type to control your companion. Examples use the companion **Elara**; use your companion's name (the one you gave her on the Personality tab or her character name).

## Where you can talk to her

| Channel | When she answers |
|---|---|
| Whisper (`/w Elara ...`) | Always, if she is your active AI companion. |
| Addon chat (Chat tab) | Always. |
| Party / raid chat | When you address her by name ("Elara, ..."). If only bots are in your group with you, she also answers lines without a name. With several companions, the one you talked to last answers. Can be turned off on the Settings tab ("Listen in party chat"). |

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
| Attack | "Attack the wolf", "Kill those murlocs", "Attack beasts", "Attack my target" | Finds the nearest matching enemy by name or creature type (beast, undead, humanoid, demon, elemental, dragonkin, giant, mechanical, critter), marks it with a skull and attacks it. |
| Retreat | "Run away", "Fall back" | Playerbots `flee`. |
| Sell | "Sell your junk", "Sell your wolf meat" | Only works while she stands at a merchant. With nearly full bags she sells gray items by herself at a vendor (`Romance.Control.AutoSellJunk`). |
| Share quests | "Share your quests", "Share the quest Kobold Camp Cleanup" | Shares the quests you don't have yet and can take, one per second. |
| Lead | "Lead the way", "Lead me to Kobold Camp Cleanup", "Let's do the Fargodeep Mine quest" | **Experimental.** She walks to one of her quests and you follow. Without a quest name she picks one you both have. When the objectives are done, she follows you again and asks what's next. |
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
