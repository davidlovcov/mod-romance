/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Text handling: routing chat between Playerbots commands and roleplay,
 * deterministic intent detection (natural-language commands, proposals),
 * language detection and sanitizing model output before it reaches chat.
 */

#ifndef MOD_TRUE_COMPANION_TEXT_H
#define MOD_TRUE_COMPANION_TEXT_H

#include "TrueCompanionRelationship.h"
#include "TrueCompanionTypes.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace truecompanion
{
    enum class MessageRoute : uint8_t
    {
        Ignore = 0,        // empty / not for us
        PlayerbotCommand,  // leave untouched for mod-playerbots
        Roleplay,          // conversational message for the companion AI
    };

    // Raw proposal intent as said by the player; the service maps "marry" to
    // engagement or marriage depending on the current stage.
    enum class ProposalIntent : uint8_t { None = 0, Dating, Engage, Marry, Breakup };

    struct ClassifiedMessage
    {
        MessageRoute route = MessageRoute::Ignore;
        std::string text;                          // possibly rewritten (e.g. "!follow" -> "follow")
        std::optional<ActionType> action;          // deterministic natural-language command
        ProposalIntent proposal = ProposalIntent::None;
    };

    class MessageClassifier
    {
    public:
        MessageClassifier();

        // `extraCommands` lets administrators add Playerbots commands from their own strategies.
        void AddCommands(std::vector<std::string> const& extraCommands);

        ClassifiedMessage Classify(std::string const& rawText, std::string const& companionName, bool naturalCommands) const;

        bool LooksLikePlayerbotCommand(std::string const& lowerTrimmed) const;

        static std::optional<ActionType> DetectAction(std::string const& lowerText);
        static ProposalIntent DetectProposal(std::string const& lowerText);
        // True if the text talks about the relationship status at all (couple, marriage, breakup...).
        // Used to ignore proposals the model "detects" in unrelated messages.
        static bool MentionsRelationshipStatus(std::string const& lowerText);

    private:
        std::set<std::string> _commands;
    };

    // Returns "en", "de" or "" (unknown).
    std::string DetectLanguage(std::string const& text);

    // Cleans model output for WoW chat: removes reasoning blocks, UI escape
    // sequences ('|'), self-name prefixes, control characters; limits length at
    // a sentence boundary.
    std::string SanitizeReply(std::string const& text, std::string const& companionName, size_t maxChars);

    // Player input: strip characters that could forge server event markers or UI escapes.
    std::string SanitizePlayerText(std::string const& text, size_t maxChars);

    // Splits into chat-sized pieces (bytes), at word boundaries, never inside a UTF-8 sequence.
    std::vector<std::string> SplitForChat(std::string const& text, size_t maxBytes);

    // True if `reply` is nearly identical to one of the recent lines.
    bool IsRepetitive(std::string const& reply, std::vector<std::string> const& recentLines, double threshold = 0.8);

    // Counts occurrences of `name` (case-insensitive, whole word) in text.
    size_t CountName(std::string const& text, std::string const& name);

    // Turns a note written for the AI about what the companion just did ("You took the lead ...; they follow
    // you. Tell them where you are going.") into a line she can say herself without the AI ("I took the lead
    // ...; you follow me."): instructions to the AI are dropped, "Nothing." is dropped, and the persons are
    // swapped (you -> I/me, they/them -> you). Text in double quotes (quest and item names) is kept as is.
    std::string PlainCompanionLine(std::string const& note);
}

#endif
