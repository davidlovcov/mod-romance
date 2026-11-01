/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceRelationship.h"
#include "RomanceGift.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace romance
{
    namespace
    {
        char const* const kSentimentIds[] = { "very_negative", "negative", "neutral", "positive", "very_positive" };
        char const* const kSignalIds[] = {
            "compliment", "kindness", "support", "flirt", "gratitude", "apology", "humor", "deep_talk",
            "shared_activity", "promise", "insult", "rudeness", "dismissive", "broken_promise", "jealousy_trigger" };
        char const* const kProposalIds[] = { "none", "dating", "engagement", "marriage", "breakup" };
        char const* const kOutcomeIds[] = { "none", "accept", "defer", "decline" };

        template <typename E, size_t N>
        std::optional<E> FromTable(std::string const& id, char const* const (&ids)[N])
        {
            for (size_t i = 0; i < N; ++i)
                if (id == ids[i])
                    return E(i);
            return std::nullopt;
        }

        double Clamp(double v) { return std::max(0.0, std::min(100.0, v)); }

        constexpr double kPerMessagePositiveCap = 4.0;
        constexpr double kPerMessageNegativeCap = 10.0;
        constexpr int64_t kDay = 24 * 3600;
        constexpr int64_t kProposalRetryCooldown = 10 * 60;

        // Stage templates used when the player chooses a starting relationship.
        struct Template { double fr, aff, att, tr, fam, com, sat; };
        Template const kTemplates[kStageCount] = {
            { 10,  5,  5, 20,  0, 20, 50 },   // strangers
            { 25, 15, 10, 30, 12, 35, 55 },   // acquaintances
            { 45, 30, 15, 40, 30, 50, 60 },   // friends
            { 70, 50, 25, 60, 55, 65, 65 },   // close friends
            { 72, 62, 58, 62, 58, 66, 66 },   // romantic interest
            { 80, 80, 74, 78, 72, 80, 76 },   // dating
            { 86, 88, 80, 86, 84, 86, 82 },   // engaged
            { 90, 92, 82, 92, 92, 90, 85 },   // married
        };
    }

    char const* SentimentId(Sentiment s) { return kSentimentIds[int(s)]; }
    std::optional<Sentiment> SentimentFromId(std::string const& id) { return FromTable<Sentiment>(id, kSentimentIds); }
    char const* SignalId(Signal s) { return kSignalIds[int(s)]; }
    std::optional<Signal> SignalFromId(std::string const& id) { return FromTable<Signal>(id, kSignalIds); }
    char const* ProposalKindId(ProposalKind k) { return kProposalIds[int(k)]; }
    std::optional<ProposalKind> ProposalKindFromId(std::string const& id) { return FromTable<ProposalKind>(id, kProposalIds); }
    char const* ProposalOutcomeId(ProposalOutcome o) { return kOutcomeIds[int(o)]; }
    std::optional<ProposalOutcome> ProposalOutcomeFromId(std::string const& id) { return FromTable<ProposalOutcome>(id, kOutcomeIds); }

    bool RelationshipDelta::Any() const
    {
        RelationshipValues const& c = change;
        return c.friendship != 0 || c.affection != 0 || c.attraction != 0 || c.trust != 0 || c.familiarity != 0 ||
               c.comfort != 0 || c.satisfaction != 0 || stageChangedTo.has_value() || strainedChanged;
    }

    Json RelationshipDelta::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("friendship", change.friendship);
        j.Set("affection", change.affection);
        j.Set("attraction", change.attraction);
        j.Set("trust", change.trust);
        j.Set("familiarity", change.familiarity);
        j.Set("comfort", change.comfort);
        j.Set("satisfaction", change.satisfaction);
        if (stageChangedTo)
        {
            j.Set("stageFrom", StageId(*stageChangedFrom));
            j.Set("stageTo", StageId(*stageChangedTo));
        }
        j.Set("capped", capped);
        return j;
    }

    RelationshipEngine::RelationshipEngine(RomanceConfig const& cfg, uint32_t seed) : _cfg(cfg), _rng(seed) { }

    double RelationshipEngine::Random01()
    {
        return std::uniform_real_distribution<double>(0.0, 1.0)(_rng);
    }

    RelationshipState RelationshipEngine::InitialFor(Stage stage, bool romantic, TimePoint now)
    {
        if (!romantic && stage > Stage::CloseFriends)
            stage = Stage::CloseFriends;
        Template const& t = kTemplates[int(stage)];
        RelationshipState st;
        st.v = { t.fr, t.aff, romantic ? t.att : std::min(t.att, 10.0), t.tr, t.fam, t.com, t.sat };
        st.stage = stage;
        st.romantic = romantic;
        st.stageSince = now;
        st.lastInteraction = now;
        st.mood = stage >= Stage::Dating ? Mood::EnjoyingCompany : Mood::Relaxed;
        st.moodIntensity = 40;
        st.dayWindowStart = now;
        return st;
    }

    void RelationshipEngine::ResetDayWindow(RelationshipState& st, TimePoint now)
    {
        if (now - st.dayWindowStart >= kDay || st.dayWindowStart > now)
        {
            st.dayWindowStart = now;
            st.dayPositiveGain = 0;
        }
    }

    // Diminishing returns near the top of the scale, scaled by progression speed and multipliers.
    double RelationshipEngine::AddPositive(RelationshipState& st, double& value, double amount, double& budget)
    {
        if (amount <= 0 || budget <= 0)
            return 0;
        double scaled = amount * _cfg.progressionSpeed * _cfg.affectionGainMultiplier * (1.0 - value / 125.0);
        double dayRoom = std::max(0.0, _cfg.dailyGainCap * std::max(0.1, _cfg.progressionSpeed) - st.dayPositiveGain);
        scaled = std::min({ scaled, budget, dayRoom });
        if (scaled <= 0)
            return 0;
        double before = value;
        value = Clamp(value + scaled);
        double applied = value - before;
        budget -= applied;
        st.dayPositiveGain += applied;
        return applied;
    }

    void RelationshipEngine::AddNegative(double& value, double amount)
    {
        if (amount <= 0)
            return;
        value = Clamp(value - amount * _cfg.affectionLossMultiplier);
    }

    RelationshipDelta RelationshipEngine::ApplyConversation(RelationshipState& st, Sentiment sentiment,
                                                            std::vector<Signal> const& signalsIn,
                                                            PersonalityProfile const& p, size_t messageLength, TimePoint now)
    {
        ResetDayWindow(st, now);
        RelationshipValues before = st.v;
        RelationshipDelta d;

        // De-duplicate signals so the model cannot stack "compliment" five times.
        std::vector<Signal> signals;
        for (Signal s : signalsIn)
            if (std::find(signals.begin(), signals.end(), s) == signals.end() && s < Signal::Count)
                signals.push_back(s);
        if (signals.size() > 4)
            signals.resize(4);

        bool romanticContext = st.romantic && p.romanceEnabled;
        bool closeEnoughForFlirt = st.stage >= Stage::Friends;
        double budget = kPerMessagePositiveCap;
        double negBudget = kPerMessageNegativeCap;
        double loyaltyDamp = 1.0 - (p.loyalty / 100.0) * 0.35;   // loyal companions forgive a bit more
        double sensitivity = std::max(0.2, _cfg.moodSensitivity);

        auto neg = [&](double& v, double amt)
        {
            double a = std::min(amt * sensitivity, negBudget);
            if (a <= 0) return;
            negBudget -= a;
            AddNegative(v, a);
        };

        // Familiarity grows from substantive conversation only, not from "hi" spam.
        if (messageLength >= 12)
            AddPositive(st, st.v.familiarity, 0.35, budget);

        for (Signal s : signals)
        {
            switch (s)
            {
                case Signal::Compliment:
                    AddPositive(st, st.v.affection, 1.2, budget);
                    if (romanticContext && closeEnoughForFlirt)
                        AddPositive(st, st.v.attraction, 0.8, budget);
                    AddPositive(st, st.v.comfort, 0.4, budget);
                    break;
                case Signal::Kindness:
                    AddPositive(st, st.v.friendship, 1.0, budget);
                    AddPositive(st, st.v.affection, 0.5, budget);
                    AddPositive(st, st.v.trust, 0.4, budget);
                    break;
                case Signal::Support:
                    AddPositive(st, st.v.trust, 1.0, budget);
                    AddPositive(st, st.v.comfort, 0.8, budget);
                    AddPositive(st, st.v.friendship, 0.5, budget);
                    break;
                case Signal::Flirt:
                    if (romanticContext && closeEnoughForFlirt)
                    {
                        AddPositive(st, st.v.attraction, 1.0 * _cfg.romanticInitiative, budget);
                        AddPositive(st, st.v.affection, 0.3, budget);
                    }
                    else if (st.stage <= Stage::Acquaintances)
                        neg(st.v.comfort, 0.8);    // too forward for a near stranger
                    break;
                case Signal::Gratitude:
                    AddPositive(st, st.v.friendship, 0.5, budget);
                    AddPositive(st, st.v.satisfaction, 0.5, budget);
                    break;
                case Signal::Apology:
                    if (st.strained)
                    {
                        AddPositive(st, st.v.trust, 2.0, budget);
                        AddPositive(st, st.v.satisfaction, 2.0, budget);
                    }
                    else
                        AddPositive(st, st.v.comfort, 0.3, budget);
                    break;
                case Signal::Humor:
                    AddPositive(st, st.v.friendship, 0.6, budget);
                    AddPositive(st, st.v.comfort, 0.4, budget);
                    break;
                case Signal::DeepTalk:
                    AddPositive(st, st.v.familiarity, 1.0, budget);
                    AddPositive(st, st.v.trust, 0.6, budget);
                    AddPositive(st, st.v.affection, 0.4, budget);
                    break;
                case Signal::SharedActivity:
                    AddPositive(st, st.v.friendship, 0.8, budget);
                    AddPositive(st, st.v.familiarity, 0.6, budget);
                    break;
                case Signal::Promise:
                    AddPositive(st, st.v.trust, 0.3, budget);
                    break;
                case Signal::Insult:
                    neg(st.v.affection, 3.0);
                    neg(st.v.trust, 2.0 * loyaltyDamp);
                    neg(st.v.satisfaction, 4.0);
                    neg(st.v.comfort, 2.0);
                    break;
                case Signal::Rudeness:
                    neg(st.v.affection, 1.5);
                    neg(st.v.satisfaction, 2.0);
                    neg(st.v.comfort, 1.0);
                    break;
                case Signal::Dismissive:
                    neg(st.v.satisfaction, 1.0);
                    neg(st.v.affection, 0.5);
                    break;
                case Signal::BrokenPromise:
                    neg(st.v.trust, 4.0 * loyaltyDamp);
                    neg(st.v.satisfaction, 3.0);
                    break;
                case Signal::JealousyTrigger:
                    neg(st.v.satisfaction, 3.0 * (p.jealousy / 100.0));
                    break;
                default:
                    break;
            }
        }

        switch (sentiment)
        {
            case Sentiment::VeryPositive: AddPositive(st, st.v.satisfaction, 0.5, budget); break;
            case Sentiment::Positive: AddPositive(st, st.v.satisfaction, 0.2, budget); break;
            case Sentiment::Negative: neg(st.v.satisfaction, 0.6); break;
            case Sentiment::VeryNegative: neg(st.v.satisfaction, 1.2); neg(st.v.comfort, 0.6); break;
            default: break;
        }

        if (budget <= 0.0001 || st.dayPositiveGain >= _cfg.dailyGainCap * std::max(0.1, _cfg.progressionSpeed))
            d.capped = true;

        // Strain: serious setback, cleared by apology/reconciliation.
        bool wasStrained = st.strained;
        if (st.stage >= Stage::Friends && (st.v.trust < 25 || st.v.satisfaction < 20))
            st.strained = true;
        else if (st.strained && st.v.trust >= 40 && st.v.satisfaction >= 40)
            st.strained = false;
        d.strainedChanged = wasStrained != st.strained;

        st.lastInteraction = now;
        st.lastPlayerMessage = now;
        ++st.messageCount;

        d.change = { st.v.friendship - before.friendship, st.v.affection - before.affection,
                     st.v.attraction - before.attraction, st.v.trust - before.trust,
                     st.v.familiarity - before.familiarity, st.v.comfort - before.comfort,
                     st.v.satisfaction - before.satisfaction };

        RelationshipDelta prog = CheckAutoProgression(st, p, now);
        d.stageChangedFrom = prog.stageChangedFrom;
        d.stageChangedTo = prog.stageChangedTo;
        return d;
    }

    RelationshipDelta RelationshipEngine::ApplyGift(RelationshipState& st, GiftEvaluation const& g, TimePoint now)
    {
        RelationshipValues before = st.v;
        RelationshipDelta d;
        // Gifts use their own (already capped) budget from the evaluator, but still saturate near 100.
        auto add = [&](double& v, double amt)
        {
            if (amt > 0)
                v = Clamp(v + amt * (1.0 - v / 125.0));
            else
                AddNegative(v, -amt);
        };
        add(st.v.affection, g.affection);
        if (st.romantic)
            add(st.v.attraction, g.attraction);
        add(st.v.friendship, g.friendship);
        add(st.v.trust, g.trust);
        add(st.v.satisfaction, g.satisfaction);
        if (g.isRing)
            st.lastGiftRingAt = now;
        st.lastInteraction = now;
        d.change = { st.v.friendship - before.friendship, st.v.affection - before.affection,
                     st.v.attraction - before.attraction, st.v.trust - before.trust, 0, 0,
                     st.v.satisfaction - before.satisfaction };
        return d;
    }

    RelationshipDelta RelationshipEngine::ApplyGameEvent(RelationshipState& st, GameEvent const& e,
                                                         PersonalityProfile const& p, TimePoint now)
    {
        ResetDayWindow(st, now);
        RelationshipValues before = st.v;
        RelationshipDelta d;
        double budget = 1.5;
        switch (e.type)
        {
            case GameEventType::CombatStart:
                SetMood(st, Mood::FocusedOnCombat, 60);
                break;
            case GameEventType::CombatEnd:
                // Fighting side by side builds friendship slowly.
                AddPositive(st, st.v.friendship, 0.3, budget);
                AddPositive(st, st.v.familiarity, 0.2, budget);
                SetMood(st, e.value >= 3 ? Mood::Excited : Mood::Relaxed, 45);
                break;
            case GameEventType::PlayerNearDeath:
            case GameEventType::PlayerDied:
                SetMood(st, Mood::Worried, 75);
                break;
            case GameEventType::CompanionDied:
                SetMood(st, Mood::Upset, 50);
                break;
            case GameEventType::CompanionLevelUp:
            case GameEventType::QuestComplete:
                AddPositive(st, st.v.friendship, 0.4, budget);
                SetMood(st, Mood::Happy, 60);
                break;
            case GameEventType::PlayerLevelUp:
                SetMood(st, Mood::Happy, 50);
                break;
            case GameEventType::ZoneChanged:
                SetMood(st, p.energy > 55 ? Mood::Excited : Mood::Curious, 40);
                break;
            case GameEventType::Summoned:
            case GameEventType::PlayerReturned:
                SetMood(st, st.stage >= Stage::Friends ? Mood::Happy : Mood::Relaxed, 50);
                break;
            default:
                break;
        }
        st.lastInteraction = std::max(st.lastInteraction, now);
        d.change = { st.v.friendship - before.friendship, 0, 0, 0, st.v.familiarity - before.familiarity, 0, 0 };
        RelationshipDelta prog = CheckAutoProgression(st, p, now);
        d.stageChangedFrom = prog.stageChangedFrom;
        d.stageChangedTo = prog.stageChangedTo;
        return d;
    }

    ProposalDecision RelationshipEngine::EvaluateProposal(RelationshipState const& st, ProposalKind kind,
                                                          PersonalityProfile const& p, TimePoint now)
    {
        ProposalDecision dec;
        dec.kind = kind;
        if (kind == ProposalKind::None)
            return dec;

        if (kind == ProposalKind::Breakup)
        {
            // The player's decision to end a romance is always respected.
            if (st.stage >= Stage::Dating)
            {
                dec.outcome = ProposalOutcome::Accept;
                dec.reason = "player_choice";
            }
            else
            {
                dec.outcome = ProposalOutcome::None;
                dec.reason = "not_together";
            }
            return dec;
        }

        if (!st.romantic || !p.romanceEnabled)
        {
            dec.outcome = ProposalOutcome::Decline;
            dec.reason = "platonic";
            return dec;
        }

        Stage target = kind == ProposalKind::Dating ? Stage::Dating : kind == ProposalKind::Engagement ? Stage::Engaged : Stage::Married;
        if (st.stage >= target)
        {
            dec.outcome = ProposalOutcome::Accept;
            dec.reason = "already";   // nothing to commit; reaffirmation
            return dec;
        }

        // Required predecessor stage
        Stage required = kind == ProposalKind::Dating ? Stage::Friends : kind == ProposalKind::Engagement ? Stage::Dating : Stage::Engaged;
        bool stageOk = st.stage >= required;

        if (p.autoAcceptProposals)
        {
            dec.outcome = stageOk || kind == ProposalKind::Dating ? ProposalOutcome::Accept : ProposalOutcome::Defer;
            dec.reason = stageOk ? "auto_accept" : "needs_previous_stage";
            return dec;
        }

        if (!stageOk)
        {
            dec.outcome = ProposalOutcome::Decline;
            dec.reason = "too_soon";
            return dec;
        }

        if (st.strained)
        {
            dec.outcome = ProposalOutcome::Decline;
            dec.reason = "relationship_strained";
            return dec;
        }

        struct Req { double aff, att, tr, fam; };
        Req req = kind == ProposalKind::Dating ? Req{ 60, 55, 50, 40 } : kind == ProposalKind::Engagement ? Req{ 80, 65, 75, 65 } : Req{ 85, 70, 85, 75 };
        double ratio = std::min({ st.v.affection / req.aff, st.v.attraction / req.att, st.v.trust / req.tr, st.v.familiarity / req.fam });

        if (kind == ProposalKind::Engagement || kind == ProposalKind::Marriage)
        {
            double minDays = kind == ProposalKind::Engagement ? _cfg.minDaysDatingBeforeEngagement : _cfg.minDaysEngagedBeforeMarriage;
            if (double(now - st.stageSince) < minDays * kDay)
            {
                dec.outcome = ProposalOutcome::Defer;
                dec.reason = "needs_more_time_together";
                return dec;
            }
            if (kind == ProposalKind::Engagement && _cfg.requireRingForEngagement && now - st.lastGiftRingAt > 3600)
            {
                dec.outcome = ProposalOutcome::Defer;
                dec.reason = "no_ring";
                return dec;
            }
        }

        bool recentlyAsked = st.lastProposalAt && now - st.lastProposalAt < kProposalRetryCooldown;

        if (ratio >= 1.0)
        {
            // Shy or insecure companions sometimes need a moment when it's borderline.
            bool borderline = ratio < 1.1;
            double hesitation = (100 - p.confidence) / 100.0 * 0.35;
            if (borderline && !recentlyAsked && Random01() < hesitation)
            {
                dec.outcome = ProposalOutcome::Defer;
                dec.reason = "nervous";
            }
            else
            {
                dec.outcome = ProposalOutcome::Accept;
                dec.reason = "ready";
            }
        }
        else if (ratio >= 0.8)
        {
            dec.outcome = ProposalOutcome::Defer;
            dec.reason = recentlyAsked ? "asked_again_too_soon" : "almost_ready";
        }
        else
        {
            dec.outcome = ProposalOutcome::Decline;
            if (st.v.trust < req.tr * 0.8)
                dec.reason = "low_trust";
            else if (st.v.attraction < req.att * 0.8)
                dec.reason = "not_attracted_yet";
            else
                dec.reason = "not_ready";
        }
        return dec;
    }

    RelationshipDelta RelationshipEngine::CommitProposal(RelationshipState& st, ProposalKind kind, TimePoint now)
    {
        RelationshipDelta d;
        Stage from = st.stage;
        Stage to = from;
        switch (kind)
        {
            case ProposalKind::Dating: if (from < Stage::Dating) to = Stage::Dating; break;
            case ProposalKind::Engagement: if (from == Stage::Dating) to = Stage::Engaged; break;
            case ProposalKind::Marriage: if (from == Stage::Engaged) to = Stage::Married; break;
            case ProposalKind::Breakup:
                if (from >= Stage::Dating)
                {
                    to = Stage::CloseFriends;
                    RelationshipValues b = st.v;
                    AddNegative(st.v.satisfaction, 25);
                    AddNegative(st.v.affection, 15);
                    AddNegative(st.v.attraction, 20);
                    AddNegative(st.v.trust, 10);
                    d.change = { 0, st.v.affection - b.affection, st.v.attraction - b.attraction, st.v.trust - b.trust, 0, 0,
                                 st.v.satisfaction - b.satisfaction };
                }
                break;
            default: break;
        }
        st.lastProposalAt = now;
        if (to == from)
            return d;
        st.stage = to;
        st.stageSince = now;
        if (kind != ProposalKind::Breakup)
        {
            // A "yes" is a big moment.
            RelationshipValues b = st.v;
            st.v.satisfaction = Clamp(st.v.satisfaction + 6);
            st.v.affection = Clamp(st.v.affection + 3);
            st.v.trust = Clamp(st.v.trust + 2);
            d.change = { 0, st.v.affection - b.affection, 0, st.v.trust - b.trust, 0, 0, st.v.satisfaction - b.satisfaction };
            st.strained = false;
            SetMood(st, Mood::Excited, 85);
        }
        else
            SetMood(st, Mood::Upset, 70);
        d.stageChangedFrom = from;
        d.stageChangedTo = to;
        return d;
    }

    RelationshipDelta RelationshipEngine::CheckAutoProgression(RelationshipState& st, PersonalityProfile const& p, TimePoint now)
    {
        RelationshipDelta d;
        if (st.stage >= Stage::Dating)
            return d;   // committed stages change only through proposals/breakups

        RelationshipValues const& v = st.v;
        bool romantic = st.romantic && p.romanceEnabled;
        Stage target = Stage::Strangers;
        if (v.familiarity >= 8 && v.friendship >= 15) target = Stage::Acquaintances;
        if (v.familiarity >= 25 && v.friendship >= 40 && v.trust >= 30) target = Stage::Friends;
        if (v.familiarity >= 45 && v.friendship >= 65 && v.trust >= 50) target = Stage::CloseFriends;
        if (romantic && target >= Stage::Friends && v.attraction >= 45 && v.affection >= 45 &&
            (target == Stage::CloseFriends || v.attraction >= 55))
            target = Stage::RomanticInterest;

        Stage newStage = st.stage;
        if (target > st.stage)
            newStage = Stage(int(st.stage) + 1);            // one step at a time
        else if (target < st.stage)
        {
            // Hysteresis: only regress when clearly below the current stage's requirements.
            RelationshipValues m = v;
            m.familiarity += 10; m.friendship += 12; m.trust += 12; m.attraction += 12; m.affection += 12;
            Stage relaxed = Stage::Strangers;
            if (m.familiarity >= 8 && m.friendship >= 15) relaxed = Stage::Acquaintances;
            if (m.familiarity >= 25 && m.friendship >= 40 && m.trust >= 30) relaxed = Stage::Friends;
            if (m.familiarity >= 45 && m.friendship >= 65 && m.trust >= 50) relaxed = Stage::CloseFriends;
            if (romantic && relaxed >= Stage::Friends && m.attraction >= 45 && m.affection >= 45) relaxed = Stage::RomanticInterest;
            if (relaxed < st.stage)
                newStage = Stage(int(st.stage) - 1);
        }
        if (newStage != st.stage)
        {
            d.stageChangedFrom = st.stage;
            d.stageChangedTo = newStage;
            st.stage = newStage;
            st.stageSince = now;
        }
        return d;
    }

    RelationshipDelta RelationshipEngine::ApplyNeglect(RelationshipState& st, TimePoint now)
    {
        RelationshipDelta d;
        if (st.lastInteraction == 0)
            return d;
        double days = double(now - st.lastInteraction) / kDay;
        if (days < _cfg.neglectDaysBeforeDecay)
            return d;
        RelationshipValues b = st.v;
        double over = std::min(30.0, days - _cfg.neglectDaysBeforeDecay);
        // Gentle: satisfaction and comfort fade a little, deep values stay.
        st.v.satisfaction = Clamp(st.v.satisfaction - over * 0.8);
        st.v.comfort = Clamp(st.v.comfort - over * 0.4);
        d.change = { 0, 0, 0, 0, 0, st.v.comfort - b.comfort, st.v.satisfaction - b.satisfaction };
        SetMood(st, st.stage >= Stage::Friends ? Mood::Lonely : Mood::Relaxed, 45);
        st.lastInteraction = now; // apply once per absence
        return d;
    }

    void RelationshipEngine::SetMood(RelationshipState& st, Mood m, double intensity)
    {
        st.mood = m;
        st.moodIntensity = Clamp(intensity * std::max(0.2, std::min(2.0, _cfg.moodSensitivity)));
    }

    void RelationshipEngine::DecayMood(RelationshipState& st, double seconds)
    {
        if (seconds <= 0)
            return;
        st.moodIntensity = std::max(0.0, st.moodIntensity - seconds / 20.0);
        if (st.moodIntensity < 10 && st.mood != Mood::Relaxed && st.mood != Mood::Lonely && st.mood != Mood::Bored)
        {
            st.mood = st.stage >= Stage::Dating ? Mood::EnjoyingCompany : Mood::Relaxed;
            st.moodIntensity = 25;
        }
    }

    std::string RelationshipEngine::Band(double v)
    {
        if (v < 12) return "none";
        if (v < 30) return "low";
        if (v < 50) return "moderate";
        if (v < 70) return "high";
        if (v < 88) return "very high";
        return "extremely high";
    }

    std::string RelationshipEngine::StageLabel(Stage s, PersonalityProfile const& p)
    {
        std::string const& custom = p.stageLabels[int(s)];
        return custom.empty() ? StageDefaultLabel(s) : custom;
    }

    std::string RelationshipEngine::DescribeForPrompt(RelationshipState const& st, PersonalityProfile const& p) const
    {
        std::ostringstream o;
        o << "Relationship stage: " << StageLabel(st.stage, p);
        if (!st.romantic || !p.romanceEnabled)
            o << " (platonic friendship; you do not want romance)";
        o << ".\n";
        o << "Your feelings: friendship " << Band(st.v.friendship) << ", affection " << Band(st.v.affection);
        if (st.romantic && p.romanceEnabled)
            o << ", romantic attraction " << Band(st.v.attraction);
        o << ", trust " << Band(st.v.trust) << ", how well you know them " << Band(st.v.familiarity)
          << ", comfort " << Band(st.v.comfort) << ", happiness with the relationship " << Band(st.v.satisfaction) << ".\n";
        switch (st.stage)
        {
            case Stage::Strangers: o << "You barely know this person; be polite but a little guarded.\n"; break;
            case Stage::Acquaintances: o << "You know each other a little; friendly but not intimate.\n"; break;
            case Stage::Friends: o << "You are friends and enjoy each other's company.\n"; break;
            case Stage::CloseFriends: o << "You are close friends who trust each other.\n"; break;
            case Stage::RomanticInterest: o << "You have feelings for them but you are not a couple yet.\n"; break;
            case Stage::Dating: o << "You are a couple. Affection is natural, but not in every single message.\n"; break;
            case Stage::Engaged: o << "You are engaged to be married and deeply committed.\n"; break;
            case Stage::Married: o << "You are married; your love is settled, warm and comfortable.\n"; break;
        }
        if (st.strained)
            o << "Things are tense between you after something hurtful; you need sincerity before you fully warm up again.\n";
        return o.str();
    }
}
