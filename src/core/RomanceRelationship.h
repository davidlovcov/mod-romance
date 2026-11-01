/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Deterministic, server-authoritative relationship rules. The language model
 * only *suggests* signals (compliment, insult, ...) and an in-character answer
 * to proposals; this engine decides what those signals are worth, enforces
 * per-message and per-day caps, and alone decides stage transitions.
 */

#ifndef MOD_ROMANCE_RELATIONSHIP_H
#define MOD_ROMANCE_RELATIONSHIP_H

#include "RomanceConfig.h"
#include "RomanceTypes.h"

#include <functional>
#include <random>
#include <string>
#include <vector>

namespace romance
{
    enum class Sentiment : uint8_t { VeryNegative = 0, Negative, Neutral, Positive, VeryPositive };
    char const* SentimentId(Sentiment s);
    std::optional<Sentiment> SentimentFromId(std::string const& id);

    enum class Signal : uint8_t
    {
        Compliment = 0, Kindness, Support, Flirt, Gratitude, Apology, Humor, DeepTalk, SharedActivity,
        Promise, Insult, Rudeness, Dismissive, BrokenPromise, JealousyTrigger,
        Count
    };
    char const* SignalId(Signal s);
    std::optional<Signal> SignalFromId(std::string const& id);

    enum class ProposalKind : uint8_t { None = 0, Dating, Engagement, Marriage, Breakup };
    char const* ProposalKindId(ProposalKind k);
    std::optional<ProposalKind> ProposalKindFromId(std::string const& id);

    enum class ProposalOutcome : uint8_t { None = 0, Accept, Defer, Decline };
    char const* ProposalOutcomeId(ProposalOutcome o);
    std::optional<ProposalOutcome> ProposalOutcomeFromId(std::string const& id);

    struct ProposalDecision
    {
        ProposalKind kind = ProposalKind::None;
        ProposalOutcome outcome = ProposalOutcome::None;
        std::string reason;   // short machine-readable reason ("ready", "too_soon", "low_trust", ...)
    };

    struct RelationshipDelta
    {
        RelationshipValues change{0, 0, 0, 0, 0, 0, 0};
        std::optional<Stage> stageChangedFrom;
        std::optional<Stage> stageChangedTo;
        bool strainedChanged = false;
        bool capped = false;  // per-message or daily cap applied

        bool Any() const;
        Json ToJson() const;
    };

    struct GiftEvaluation;

    class RelationshipEngine
    {
    public:
        explicit RelationshipEngine(RomanceConfig const& cfg, uint32_t seed = std::random_device{}());

        // Values a relationship starts with when the player picks an initial stage.
        static RelationshipState InitialFor(Stage stage, bool romantic, TimePoint now);

        RelationshipDelta ApplyConversation(RelationshipState& st, Sentiment sentiment, std::vector<Signal> const& signals,
                                            PersonalityProfile const& p, size_t messageLength, TimePoint now);
        RelationshipDelta ApplyGift(RelationshipState& st, GiftEvaluation const& g, TimePoint now);
        RelationshipDelta ApplyGameEvent(RelationshipState& st, GameEvent const& e, PersonalityProfile const& p, TimePoint now);

        ProposalDecision EvaluateProposal(RelationshipState const& st, ProposalKind kind, PersonalityProfile const& p, TimePoint now);
        // Persist an accepted proposal. Returns the delta (stage change) or an empty delta if not allowed.
        RelationshipDelta CommitProposal(RelationshipState& st, ProposalKind kind, TimePoint now);

        // Gradual automatic progression (and regression) up to Romantic Interest. Dating and above
        // are reached only through accepted proposals.
        RelationshipDelta CheckAutoProgression(RelationshipState& st, PersonalityProfile const& p, TimePoint now);

        // Long absence: slow decay of satisfaction/comfort, never of memories.
        RelationshipDelta ApplyNeglect(RelationshipState& st, TimePoint now);

        void SetMood(RelationshipState& st, Mood m, double intensity);
        void DecayMood(RelationshipState& st, double seconds);

        // Qualitative descriptions for prompts and UI.
        static std::string Band(double v);
        std::string DescribeForPrompt(RelationshipState const& st, PersonalityProfile const& p) const;
        static std::string StageLabel(Stage s, PersonalityProfile const& p);

        double Random01();

    private:
        double AddPositive(RelationshipState& st, double& value, double amount, double& budget);
        void AddNegative(double& value, double amount);
        void ResetDayWindow(RelationshipState& st, TimePoint now);

        RomanceConfig const& _cfg;
        std::mt19937 _rng;
    };
}

#endif
