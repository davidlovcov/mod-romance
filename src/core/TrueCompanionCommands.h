/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Natural-language companion commands and knowledge topics.
 *
 * ParseCompanionCommand turns sentences such as "give me 10 gold",
 * "attack the wolf" or "can you sell your junk?" into a structured command.
 * The game layer executes it against the real game state and hands the
 * outcome to the prompt, so the companion only claims what really happened.
 *
 * DetectKnowledgeTopics decides which game knowledge (bags, quests, spells,
 * gold, experience, the player's target) is added to the prompt, so the
 * prompt only grows when the player asks about it.
 *
 * Engine independent (no AzerothCore headers); covered by the unit tests.
 */

#ifndef MOD_TRUE_COMPANION_COMMANDS_H
#define MOD_TRUE_COMPANION_COMMANDS_H

#include <cstdint>
#include <optional>
#include <string>

namespace truecompanion
{
    enum class CompanionCommandKind : uint8_t
    {
        None = 0,
        Cast,          // arg = spell name; onMe = cast on the player
        GiveItem,      // arg = item name; amount = count (amountGiven false = all/one stack)
        GiveGold,      // amount = copper
        AttackNamed,   // arg = creature name or creature type ("wolf", "beasts", "undead")
        AttackTarget,  // the player's current target (nearest enemy if nothing is targeted)
        AttackAll,     // clear the area: one enemy after another; arg = optional name/type filter
        StopAttack,    // stop clearing the area
        SellJunk,      // all gray items
        SellItem,      // arg = item name; amount = count (amountGiven false = all of them)
        ShareQuests,   // share every quest the player could take
        ShareQuest,    // arg = quest title
        AcceptQuests,  // take quests from quest givers (walks to one nearby if needed); arg = title or "class"
        TurnInQuests,  // turn in finished quests at nearby quest enders; arg = optional title
        Lead,          // she leads; arg = quest title to do (optional)
        StopLead,      // back to following
        PauseLead,     // "wait" while she leads: stop and wait, keep the lead (else it is the normal "stay")
        ResumeLead,    // "go on", "lead again": continue a paused lead (ignored when she isn't leading)
        LearnSpells,   // "learn your spells": walk to her class trainer and learn what she can afford
    };

    struct CompanionCommand
    {
        CompanionCommandKind kind = CompanionCommandKind::None;
        std::string arg;
        uint64_t amount = 0;
        bool amountGiven = false;   // the player named a count ("sell 3 ..."), as opposed to none ("sell your ...")
        bool badAmount = false;     // the player named an amount that is not a whole positive number ("-5", "1.5 gold",
                                    // "0 linen cloth", "1,000 gold", too many digits): the command must not run
        bool onMe = false;
        bool all = false;      // AcceptQuests: "all your quests" / "your quests" (every giver around), not just the nearest quest
        bool direct = false;   // addressed to her alone (whisper, addon, or her name in party chat), not to everyone
    };

    char const* CompanionCommandId(CompanionCommandKind k);

    // `companionNames` = names she answers to (character name, chosen name), used to strip a leading address.
    std::optional<CompanionCommand> ParseCompanionCommand(std::string const& text, std::string const& companionName,
                                                          std::string const& otherName = "");

    // "10g", "5 gold 20 silver", "1g50s" -> copper; 0 if none, or if an amount is not a whole positive number
    // ("1.5 gold", "-5 gold", "1,000 gold") or has too many digits.
    uint64_t ParseMoney(std::string const& lowerText);

    namespace knowledge
    {
        enum : uint32_t
        {
            Inventory = 1u << 0,
            Quests    = 1u << 1,
            Spells    = 1u << 2,
            Gold      = 1u << 3,
            Xp        = 1u << 4,
            Target    = 1u << 5,
            Gear      = 1u << 6,
        };
    }
    uint32_t DetectKnowledgeTopics(std::string const& text);

    // True if the line addresses every companion ("everyone, ...", "all of you", "guys", ...).
    bool AddressesEveryone(std::string const& text);

    // Creature types players use in commands ("beast", "undead", ...), as the CreatureType id
    // used by the game (1 beast .. 10 not specified); 0 = not a creature type word.
    uint32_t CreatureTypeFromWord(std::string const& word);
}

#endif
