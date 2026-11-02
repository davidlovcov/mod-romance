/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * DialogueContextBuilder: turns personality, player profile, relationship,
 * retrieved memories and a world snapshot into Ollama chat messages, plus the
 * JSON schema for the structured reply. Also parses/validates the model reply.
 *
 * The model output is untrusted: it may only suggest signals from fixed enums,
 * memory candidates (importance capped) and actions from an allowlist. The
 * relationship engine and the service decide what actually happens.
 */

#ifndef MOD_TRUE_COMPANION_PROMPT_H
#define MOD_TRUE_COMPANION_PROMPT_H

#include "TrueCompanionRelationship.h"
#include "TrueCompanionText.h"
#include "TrueCompanionTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace truecompanion
{
    struct ChatMessage
    {
        std::string role;     // system | user | assistant
        std::string content;
    };

    enum class TriggerKind : uint8_t
    {
        PlayerMessage = 0,
        GameEvent,
        Gift,
        Proactive,
        Summon,
    };

    struct PromptInput
    {
        TriggerKind trigger = TriggerKind::PlayerMessage;
        std::string triggerText;                 // player message or event description
        PersonalityProfile const* personality = nullptr;
        PlayerProfile const* player = nullptr;
        RelationshipState const* relationship = nullptr;
        std::string relationshipDescription;     // from RelationshipEngine::DescribeForPrompt
        std::vector<MemoryEntry> memories;
        WorldContext world;
        bool includeWorld = true;
        std::vector<ChatTurn> history;           // oldest first
        std::string companionName;               // the name she goes by (personality name)
        std::string worldName;                   // the bot character's name in the game world
        std::string playerName;                  // character name of the player
        std::string language;                    // en | de
        std::optional<ProposalDecision> proposal;
        std::optional<ActionType> requestedAction; // deterministic NL command already executed
        bool allowActions = true;
        size_t maxReplyChars = 450;
        bool shortReplyHint = false;
        TimePoint now = 0;
        std::string avoidRepeating;              // previous line the model must not repeat
    };

    struct MemoryCandidate
    {
        std::string text;
        MemoryKind kind = MemoryKind::Fact;
        std::string about;        // player | companion | both
        int importance = 40;
    };

    struct CompanionReply
    {
        bool valid = false;
        bool structured = false;     // came from valid JSON (vs plain-text fallback)
        std::string reply;
        Mood emotion = Mood::Relaxed;
        bool hasEmotion = false;
        Sentiment sentiment = Sentiment::Neutral;
        std::vector<Signal> signals;
        ProposalKind proposalKind = ProposalKind::None;
        ProposalOutcome proposalResponse = ProposalOutcome::None;
        std::vector<MemoryCandidate> memories;
        ActionType action = ActionType::None;
        std::vector<std::string> warnings;
    };

    struct SummaryReply
    {
        bool valid = false;
        std::string summary;
        std::vector<MemoryCandidate> facts;
    };

    class DialogueContextBuilder
    {
    public:
        static std::vector<ChatMessage> Build(PromptInput const& in);
        static std::string BuildSystemPrompt(PromptInput const& in);
        static Json ReplySchema();

        static std::vector<ChatMessage> BuildSummary(std::vector<ChatTurn> const& turns, std::string const& companionName,
                                                     std::string const& playerName, std::string const& language);
        static Json SummarySchema();

        // Parse and validate model output. Never throws; falls back to plain text.
        static CompanionReply ParseReply(std::string const& content, std::string const& companionName, size_t maxReplyChars);
        static SummaryReply ParseSummary(std::string const& content);

        static std::string RelativeTime(TimePoint then, TimePoint now);
        static std::string DescribeTraits(PersonalityProfile const& p);
        static std::string DescribeStyle(PersonalityProfile const& p);
        static std::string EventMarker() { return "<<GAME_EVENT>>"; }
        static std::string ProposalDirective(ProposalDecision const& d, std::string const& playerName);
    };

    // Extract the first balanced JSON object from text (models sometimes wrap JSON in prose or fences).
    std::optional<std::string> ExtractJsonObject(std::string const& text);
}

#endif
