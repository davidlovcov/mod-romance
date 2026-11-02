/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 * Unit tests: JSON, protocol, relationship, gifts, memory, text, prompts.
 */

#include "TestFramework.h"
#include "TrueCompanionTransfer.h"

#include "TrueCompanionGift.h"
#include "TrueCompanionJson.h"
#include "TrueCompanionMemory.h"
#include "TrueCompanionPresets.h"
#include "TrueCompanionPrompt.h"
#include "TrueCompanionProtocol.h"
#include "TrueCompanionRelationship.h"
#include "TrueCompanionText.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace truecompanion;

namespace
{
    constexpr TimePoint kNow = 1'800'000'000;
    constexpr int64_t kDay = 86400;

    PersonalityProfile Elara()
    {
        PersonalityProfile p = FindPreset("anime_girlfriend")->profile;
        p.name = "Elara";
        p.age = 24;
        p.likes = "flowers, old ruins, magic";
        p.dislikes = "murlocs, fish";
        return p;
    }
}

// ============================================================ JSON

TEST(Json, RoundTripAndEscapes)
{
    Json j = Json::MakeObject();
    j.Set("s", "a\"b\\c\nd|e");
    j.Set("n", 42);
    j.Set("f", 1.5);
    j.Set("b", true);
    Json arr = Json::MakeArray();
    arr.Push(1);
    arr.Push("x");
    j.Set("a", arr);
    std::string dumped = j.Dump();
    auto back = Json::Parse(dumped);
    CHECK(back.has_value());
    CHECK_EQ(back->GetString("s"), std::string("a\"b\\c\nd|e"));
    CHECK_EQ(back->GetInt("n"), 42);
    CHECK_NEAR(back->GetNumber("f"), 1.5, 1e-9);
    CHECK(back->GetBool("b"));
    CHECK_EQ(back->operator[]("a").Size(), size_t(2));
}

TEST(Json, UnicodeEscapesAndSurrogates)
{
    auto j = Json::Parse("{\"t\":\"Gr\\u00fc\\u00dfe \\ud83d\\ude00\"}");
    CHECK(j.has_value());
    CHECK_EQ(j->GetString("t"), std::string("Grüße \xF0\x9F\x98\x80"));
}

TEST(Json, RejectsMalformedAndDeepInput)
{
    CHECK(!Json::Parse("{\"a\":}").has_value());
    CHECK(!Json::Parse("{\"a\":1,}").has_value());
    CHECK(!Json::Parse("[1,2").has_value());
    CHECK(!Json::Parse("{\"a\":\"\\ud800\"}").has_value()); // lone surrogate
    CHECK(!Json::Parse("nul").has_value());
    std::string deep(100, '[');
    deep += std::string(100, ']');
    CHECK(!Json::Parse(deep).has_value());
    CHECK(!Json::Parse(std::string(2000, ' ') + "1", nullptr, JsonParseLimits{ 100, 8 }).has_value());
}

TEST(Json, CopyOnWriteDoesNotAlias)
{
    Json a = Json::MakeObject();
    a.Set("x", 1);
    Json b = a;
    b.Set("x", 2);
    CHECK_EQ(a.GetInt("x"), 1);
    CHECK_EQ(b.GetInt("x"), 2);
}

// ============================================================ Protocol

TEST(Protocol, ChunkRoundTripLargeMessage)
{
    Json msg = Json::MakeObject();
    msg.Set("op", "save_personality");
    std::string big;
    for (int i = 0; i < 300; ++i)
        big += "Elara mag Blumen und alte Ruinen. ";
    msg.Set("text", big + "|cffff0000injected|r");
    auto chunks = EncodeAddonMessage(msg, 12345);
    CHECK(chunks.size() > 10);
    AddonReassembler re;
    AddonReassembler::Result last;
    for (auto const& c : chunks)
    {
        CHECK(c.size() + 5 <= 255);              // prefix "TCMP" + tab + body fits an addon message
        CHECK(c.find('|') == std::string::npos); // never raw pipes on the wire
        last = re.Feed(c, 1000);
    }
    CHECK(last.status == AddonReassembler::Status::Complete);
    CHECK_EQ(last.message.GetString("text"), big + "|cffff0000injected|r");
}

TEST(Protocol, OutOfOrderAndDuplicateChunks)
{
    Json msg = Json::MakeObject();
    msg.Set("op", "chat");
    msg.Set("text", std::string(500, 'x'));
    auto chunks = EncodeAddonMessage(msg, 7);
    AddonReassembler re;
    std::reverse(chunks.begin(), chunks.end());
    re.Feed(chunks[0], 0);
    AddonReassembler::Result r;
    for (auto const& c : chunks)
        r = re.Feed(c, 10);
    CHECK(r.status == AddonReassembler::Status::Complete);
}

TEST(Protocol, RejectsHostileInput)
{
    AddonReassembler re;
    CHECK(re.Feed("garbage", 0).status == AddonReassembler::Status::Malformed);
    CHECK(re.Feed("a:0/1:{}", 0).status == AddonReassembler::Status::Malformed);
    CHECK(re.Feed("a:2/1:{}", 0).status == AddonReassembler::Status::Malformed);
    CHECK(re.Feed("a:1/9999:{}", 0).status == AddonReassembler::Status::TooLarge);
    CHECK(re.Feed("a$:1/1:{}", 0).status == AddonReassembler::Status::Malformed);
    CHECK(re.Feed("a:1/1:{\"op\":", 0).status == AddonReassembler::Status::Malformed);
    CHECK(re.Feed("a:1/1:{\"noop\":1}", 0).status == AddonReassembler::Status::Malformed);
    // oversize: many chunks of 200 bytes beyond 16 KiB
    std::string data(200, 'x');
    AddonReassembler::Result r;
    for (int i = 1; i <= 90; ++i)
    {
        r = re.Feed("big:" + std::to_string(i) + "/90:" + data, 0);
        if (r.status != AddonReassembler::Status::Incomplete)
            break;
    }
    CHECK(r.status == AddonReassembler::Status::TooLarge);
    // too many concurrent partial messages
    AddonReassembler re2;
    for (int i = 0; i < 4; ++i)
        CHECK(re2.Feed("m" + std::to_string(i) + ":1/2:{", 0).status == AddonReassembler::Status::Incomplete);
    CHECK(re2.Feed("m9:1/2:{", 0).status == AddonReassembler::Status::TooManyPartials);
    // partials expire
    CHECK(re2.Feed("m9:1/2:{", 20000).status == AddonReassembler::Status::Incomplete);
}

TEST(Protocol, RateLimiter)
{
    RateLimiter rl(10, 20);
    int allowed = 0;
    for (int i = 0; i < 100; ++i)
        allowed += rl.Allow(1000) ? 1 : 0;
    CHECK_EQ(allowed, 20);
    CHECK(rl.Allow(2000));  // refilled after a second
}

// ============================================================ Relationship

TEST(Relationship, InitialStagesAreConsistent)
{
    for (int i = 0; i < kStageCount; ++i)
    {
        RelationshipState st = RelationshipEngine::InitialFor(Stage(i), true, kNow);
        CHECK(int(st.stage) == i);
        CHECK(st.v.affection >= 0 && st.v.affection <= 100);
    }
    RelationshipState couple = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    CHECK(couple.v.affection >= 75);
    CHECK(couple.v.trust >= 70);
    RelationshipState platonic = RelationshipEngine::InitialFor(Stage::Married, false, kNow);
    CHECK(platonic.stage == Stage::CloseFriends);
    CHECK(platonic.v.attraction <= 10);
}

TEST(Relationship, SpamDoesNotMaxOutAffection)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    PersonalityProfile p = Elara();
    for (int i = 0; i < 500; ++i)
        eng.ApplyConversation(st, Sentiment::VeryPositive, { Signal::Compliment, Signal::Kindness, Signal::Flirt }, p, 40, kNow + i);
    double total = (st.v.affection - 5) + (st.v.friendship - 10) + (st.v.attraction - 5) + (st.v.trust - 20);
    CHECK(total <= cfg.dailyGainCap + 0.01 + 3.0);   // daily cap (+ satisfaction/comfort/familiarity share the cap)
    CHECK(st.stage <= Stage::Acquaintances);
}

TEST(Relationship, EstrangementIsHardToFix)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    PersonalityProfile p = Elara();
    // one bad day is not enough to make a partner leave...
    bool left = false;
    TimePoint t = kNow;
    for (int i = 0; i < 3 && !left; ++i)
        left = eng.ApplyConversation(st, Sentiment::VeryNegative, { Signal::Insult }, p, 30, t += 60).leaving;
    CHECK(!left);
    CHECK(st.resentment > 30);
    // ...but constant insults are
    for (int i = 0; i < 20 && !left; ++i)
        left = eng.ApplyConversation(st, Sentiment::VeryNegative, { Signal::Insult, Signal::Rudeness }, p, 30, t += 60).leaving;
    CHECK(left);

    TimePoint until = eng.Estrange(st, t);
    CHECK_EQ(until - t, TimePoint(cfg.estrangeCooldownHours) * 3600);
    CHECK(st.estranged);
    CHECK(st.stage == Stage::Acquaintances);
    CHECK(RelationshipEngine::Away(st, t + 3600));
    CHECK(!RelationshipEngine::Away(st, until + 1));
    CHECK(eng.EvaluateProposal(st, ProposalKind::Dating, p, t).outcome == ProposalOutcome::Decline);

    // reconciliation is capped per day: a flood of apologies in one day is not enough
    t = until + 10;
    bool reconciled = false;
    for (int i = 0; i < 50 && !reconciled; ++i)
        reconciled = eng.ApplyConversation(st, Sentiment::VeryPositive, { Signal::Apology, Signal::Kindness }, p, 40, t += 30).reconciled;
    CHECK(!reconciled);
    CHECK(st.reconciliation <= cfg.reconcilePerDay + 0.01);
    // days of sincere effort do it
    for (int day = 1; day <= 6 && !reconciled; ++day)
    {
        t = until + day * 24 * 3600 + 100;
        for (int i = 0; i < 10 && !reconciled; ++i)
            reconciled = eng.ApplyConversation(st, Sentiment::VeryPositive, { Signal::Apology, Signal::Kindness }, p, 40, t += 30).reconciled;
    }
    CHECK(reconciled);
    CHECK(!st.estranged);
    CHECK(st.strained);

    // leaving again doubles the cooldown and costs more
    TimePoint until2 = eng.Estrange(st, t);
    CHECK_EQ(until2 - t, TimePoint(cfg.estrangeCooldownHours) * 3600 * 2);
    CHECK(st.stage == Stage::Strangers);
    // hurting her while she tries to forgive: she leaves again at once
    CHECK(eng.ApplyConversation(st, Sentiment::Negative, { Signal::Rudeness }, p, 20, until2 + 100).leaving);
}

TEST(Relationship, EstrangementCanBeDisabled)
{
    TrueCompanionConfig cfg;
    cfg.estrangementEnabled = false;
    RelationshipEngine eng(cfg, 1);
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Friends, true, kNow);
    PersonalityProfile p = Elara();
    bool left = false;
    for (int i = 0; i < 60; ++i)
        left = left || eng.ApplyConversation(st, Sentiment::VeryNegative, { Signal::Insult, Signal::Rudeness }, p, 30, kNow + i * 60).leaving;
    CHECK(!left);
}

TEST(Relationship, ProgressesGraduallyOverDays)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    PersonalityProfile p = Elara();
    TimePoint t = kNow;
    for (int day = 0; day < 30; ++day)
        for (int i = 0; i < 40; ++i)
            eng.ApplyConversation(st, Sentiment::Positive, { Signal::Kindness, Signal::DeepTalk, Signal::Compliment, Signal::Humor }, p, 60,
                                  t = kNow + day * kDay + i * 60);
    CHECK(st.stage >= Stage::Friends);
    CHECK(st.stage <= Stage::RomanticInterest);  // Dating only via an accepted proposal
}

TEST(Relationship, InsultsHurtAndStrain)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    PersonalityProfile p = Elara();
    double before = st.v.affection;
    for (int i = 0; i < 20; ++i)
        eng.ApplyConversation(st, Sentiment::VeryNegative, { Signal::Insult, Signal::Rudeness }, p, 30, kNow + i);
    CHECK(st.v.affection < before - 10);
    CHECK(st.strained);
    CHECK(st.stage == Stage::Dating); // no automatic breakup
    // reconciliation over time
    for (int d = 1; d < 15; ++d)
        for (int i = 0; i < 10; ++i)
            eng.ApplyConversation(st, Sentiment::Positive, { Signal::Apology, Signal::Support }, p, 50, kNow + d * kDay + i);
    CHECK(!st.strained);
}

TEST(Relationship, ProposalAcceptedWhenReady)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    PersonalityProfile p = Elara();
    p.confidence = 100; // no nervous deferral
    RelationshipState st = RelationshipEngine::InitialFor(Stage::RomanticInterest, true, kNow);
    st.v.affection = 70;
    st.v.attraction = 65;
    auto d = eng.EvaluateProposal(st, ProposalKind::Dating, p, kNow);
    CHECK(d.outcome == ProposalOutcome::Accept);
    auto delta = eng.CommitProposal(st, ProposalKind::Dating, kNow);
    CHECK(delta.stageChangedTo.has_value());
    CHECK(st.stage == Stage::Dating);
}

TEST(Relationship, ProposalRejectedForStrangersAndDeferredWhenClose)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    PersonalityProfile p = Elara();
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    CHECK(eng.EvaluateProposal(st, ProposalKind::Dating, p, kNow).outcome == ProposalOutcome::Decline);
    CHECK(eng.EvaluateProposal(st, ProposalKind::Marriage, p, kNow).outcome == ProposalOutcome::Decline);
    RelationshipState close = RelationshipEngine::InitialFor(Stage::CloseFriends, true, kNow);
    close.v.affection = 55;
    close.v.attraction = 50;
    auto d = eng.EvaluateProposal(close, ProposalKind::Dating, p, kNow);
    CHECK(d.outcome == ProposalOutcome::Defer);
    // commit is refused for skipped stages
    RelationshipState s2 = RelationshipEngine::InitialFor(Stage::Friends, true, kNow);
    auto delta = eng.CommitProposal(s2, ProposalKind::Marriage, kNow);
    CHECK(!delta.stageChangedTo.has_value());
    CHECK(s2.stage == Stage::Friends);
}

TEST(Relationship, EngagementNeedsTimeAndMarriageNeedsEngagement)
{
    TrueCompanionConfig cfg;
    cfg.minDaysDatingBeforeEngagement = 2;
    RelationshipEngine eng(cfg, 1);
    PersonalityProfile p = Elara();
    p.confidence = 100;
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    st.v = { 90, 90, 85, 90, 90, 90, 90 };
    CHECK(eng.EvaluateProposal(st, ProposalKind::Engagement, p, kNow + 3600).outcome == ProposalOutcome::Defer);
    CHECK(eng.EvaluateProposal(st, ProposalKind::Engagement, p, kNow + 3 * kDay).outcome == ProposalOutcome::Accept);
    CHECK(eng.EvaluateProposal(st, ProposalKind::Marriage, p, kNow + 3 * kDay).outcome == ProposalOutcome::Decline);
    eng.CommitProposal(st, ProposalKind::Engagement, kNow + 3 * kDay);
    CHECK(st.stage == Stage::Engaged);
    CHECK(eng.EvaluateProposal(st, ProposalKind::Marriage, p, kNow + 5 * kDay).outcome == ProposalOutcome::Accept);
}

TEST(Relationship, PlatonicAndAutoAcceptAndBreakup)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    PersonalityProfile friendP = FindPreset("loyal_friend")->profile;
    RelationshipState st = RelationshipEngine::InitialFor(Stage::CloseFriends, false, kNow);
    CHECK(eng.EvaluateProposal(st, ProposalKind::Dating, friendP, kNow).reason == "platonic");

    PersonalityProfile easy = Elara();
    easy.autoAcceptProposals = true;
    RelationshipState s2 = RelationshipEngine::InitialFor(Stage::Acquaintances, true, kNow);
    CHECK(eng.EvaluateProposal(s2, ProposalKind::Dating, easy, kNow).outcome == ProposalOutcome::Accept);

    RelationshipState couple = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    auto d = eng.EvaluateProposal(couple, ProposalKind::Breakup, easy, kNow);
    CHECK(d.outcome == ProposalOutcome::Accept);
    eng.CommitProposal(couple, ProposalKind::Breakup, kNow);
    CHECK(couple.stage == Stage::CloseFriends);
    CHECK(couple.v.satisfaction < 60);
}

TEST(Relationship, ProposalRetryCooldown)
{
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    PersonalityProfile p = Elara();
    RelationshipState st = RelationshipEngine::InitialFor(Stage::CloseFriends, true, kNow);
    st.v.affection = 52; st.v.attraction = 50;
    st.lastProposalAt = kNow - 60;
    auto d = eng.EvaluateProposal(st, ProposalKind::Dating, p, kNow);
    CHECK(d.outcome == ProposalOutcome::Defer);
    CHECK_EQ(d.reason, std::string("asked_again_too_soon"));
}

// ============================================================ Gifts

namespace
{
    GiftItem Item(std::string name, int quality, int cls, int sub, int inv, uint32_t price, int ilvl = 0)
    {
        GiftItem g;
        g.itemGuid = 1000 + quality * 7 + cls;
        g.entry = 40000 + uint32_t(name.size());
        g.name = std::move(name);
        g.quality = quality;
        g.itemClass = cls;
        g.itemSubClass = sub;
        g.inventoryType = inv;
        g.sellPrice = price;
        g.itemLevel = ilvl;
        return g;
    }
}

TEST(Gift, QuestRewardChoicePrefersUsableUpgradesOfHerArmorType)
{
    auto gear = [](uint32_t entry, int sub, int inv, int ilvl, int equipped, bool usable, uint32_t price)
    {
        GiftItem g;
        g.entry = entry;
        g.itemClass = 4;
        g.itemSubClass = sub;
        g.inventoryType = inv;
        g.itemLevel = ilvl;
        g.equippedItemLevel = equipped;
        g.isEquippable = true;
        g.usableByCompanion = usable;
        g.sellPrice = price;
        return g;
    };
    GiftItem potion;
    potion.entry = 9;
    potion.sellPrice = 5000;

    CHECK_EQ(PreferredArmorTier(8, 60), 1);    // mage: cloth
    CHECK_EQ(PreferredArmorTier(4, 20), 2);    // rogue: leather
    CHECK_EQ(PreferredArmorTier(2, 30), 3);    // paladin: mail until 40
    CHECK_EQ(PreferredArmorTier(2, 40), 4);    // then plate
    CHECK_EQ(PreferredArmorTier(7, 20), 2);    // shaman: leather until 40
    CHECK_EQ(PreferredArmorTier(7, 45), 3);    // then mail
    CHECK_EQ(PreferredArmorTier(6, 10), 4);    // death knight: plate

    CHECK_EQ(PickQuestReward({}, 3), size_t(0));

    // an upgrade beats a potion, which beats a piece she cannot use
    std::vector<GiftItem> a = { gear(1, 3, 7, 20, 25, false, 90000), potion, gear(3, 3, 7, 30, 25, true, 100) };
    CHECK_EQ(PickQuestReward(a, 3), size_t(2));
    a[2].itemLevel = 24;                                   // no longer an upgrade, but still usable gear
    CHECK_EQ(PickQuestReward(a, 3), size_t(2));            // usable gear still beats the potion
    a[2].usableByCompanion = false;
    CHECK_EQ(PickQuestReward(a, 3), size_t(1));            // potion beats gear she cannot use

    // bigger upgrade wins; an empty slot counts as a full upgrade
    std::vector<GiftItem> b = { gear(1, 3, 5, 30, 25, true, 0), gear(2, 3, 7, 33, 25, true, 0), gear(3, 3, 8, 28, -1, true, 0) };
    CHECK_EQ(PickQuestReward(b, 3), size_t(2));            // item level 28 into an empty slot: the biggest gain
    b[2].itemLevel = 5;
    CHECK_EQ(PickQuestReward(b, 3), size_t(1));            // an empty slot gains only 5 now; the legs (+8) win


    // a plate upgrade for a plate wearer beats a bigger cloth upgrade; for a cloth wearer the reverse
    std::vector<GiftItem> c = { gear(1, 1, 5, 35, 25, true, 0), gear(2, 4, 5, 30, 25, true, 0) };
    CHECK_EQ(PickQuestReward(c, 4), size_t(1));
    CHECK_EQ(PickQuestReward(c, 1), size_t(0));
    // cloaks are cloth for everyone: no armor-type penalty
    std::vector<GiftItem> d = { gear(1, 1, 16, 35, 25, true, 0), gear(2, 4, 5, 30, 25, true, 0) };
    CHECK_EQ(PickQuestReward(d, 4), size_t(0));

    // empty slots (entry 0) are never picked, even when they are first
    std::vector<GiftItem> e = { GiftItem(), potion };
    CHECK_EQ(PickQuestReward(e, 3), size_t(1));
    // nothing but empty slots: slot 0
    CHECK_EQ(PickQuestReward({ GiftItem(), GiftItem() }, 3), size_t(0));
}

TEST(Gift, CategoriesAndRingMeansMoreToPartner)
{
    TrueCompanionConfig cfg;
    GiftEvaluator ev(cfg);
    PersonalityProfile p = Elara();
    RelationshipState couple = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    GiftItem ring = Item("Silver Ring of Devotion", 3, 4, 0, 11, 20000, 70);
    ring.isEquippable = true;
    ring.usableByCompanion = true;
    ring.equippedItemLevel = 80;
    auto r = ev.Evaluate(ring, p, couple, {}, kNow);
    CHECK(r.category == GiftCategory::Ring);
    CHECK(r.isRing && r.romantic && r.memorable);
    GiftItem junk = Item("Broken Fang", 0, 15, 0, 0, 5);
    auto j = ev.Evaluate(junk, p, couple, {}, kNow);
    CHECK(j.category == GiftCategory::Junk);
    CHECK(r.score > j.score + 30);
    CHECK(r.affection > j.affection);
}

TEST(Gift, ThoughtfulCheapBeatsIrrelevantExpensive)
{
    TrueCompanionConfig cfg;
    GiftEvaluator ev(cfg);
    PersonalityProfile p = Elara();   // likes flowers
    RelationshipState st = RelationshipEngine::InitialFor(Stage::CloseFriends, true, kNow);
    GiftItem flowers = Item("Bouquet of Red Roses", 1, 15, 4, 0, 25);
    GiftItem plate = Item("Titansteel Destroyer Plate", 4, 4, 4, 5, 900000, 213);
    plate.isEquippable = true;
    plate.usableByCompanion = false;   // mage cannot wear plate
    auto f = ev.Evaluate(flowers, p, st, {}, kNow);
    auto e = ev.Evaluate(plate, p, st, {}, kNow);
    CHECK(f.category == GiftCategory::Flowers);
    CHECK(f.score > e.score);
    CHECK(f.score < 100);
}

TEST(Gift, UpgradeIsAppreciatedAndRepeatsDiminish)
{
    TrueCompanionConfig cfg;
    GiftEvaluator ev(cfg);
    PersonalityProfile p = Elara();
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Friends, true, kNow);
    GiftItem robe = Item("Spellweave Robe", 4, 4, 1, 20, 40000, 200);
    robe.isEquippable = true;
    robe.usableByCompanion = true;
    robe.equippedItemLevel = 187;
    auto up = ev.Evaluate(robe, p, st, {}, kNow);
    robe.equippedItemLevel = 226;
    auto side = ev.Evaluate(robe, p, st, {}, kNow);
    CHECK(up.score > side.score + 15);

    GiftItem flowers = Item("Bouquet of Red Roses", 1, 15, 4, 0, 25);
    std::vector<GiftHistoryEntry> hist;
    auto first = ev.Evaluate(flowers, p, st, hist, kNow);
    hist.push_back({ flowers.entry, kNow - 100, first.score });
    hist.push_back({ flowers.entry, kNow - 50, first.score });
    auto third = ev.Evaluate(flowers, p, st, hist, kNow);
    CHECK(third.score < first.score / 2.0);
}

TEST(Gift, DailyCapAndAwkwardRing)
{
    TrueCompanionConfig cfg;
    GiftEvaluator ev(cfg);
    PersonalityProfile p = Elara();
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    std::vector<GiftHistoryEntry> hist;
    for (int i = 0; i < 10; ++i)
        hist.push_back({ uint32_t(50000 + i), kNow - 3000 - i, 100 });
    GiftItem neck = Item("Pendant of the Moon", 4, 4, 0, 2, 50000, 200);
    auto r = ev.Evaluate(neck, p, st, hist, kNow);
    CHECK(r.affection < 0.01);

    RelationshipState stranger = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    GiftItem ring = Item("Gold Ring", 2, 4, 0, 11, 5000, 40);
    auto awk = ev.Evaluate(ring, p, stranger, {}, kNow);
    CHECK(awk.awkward);
    CHECK_NEAR(awk.attraction, 0, 1e-9);
}

// ============================================================ Memory

TEST(Memory, DedupeMergesNearDuplicates)
{
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    auto a = store.Add(MemoryKind::Preference, MemorySource::PlayerStated, 40, "David likes exploring old ruins.", kNow);
    auto b = store.Add(MemoryKind::Preference, MemorySource::PlayerStated, 50, "David likes exploring old ruins!", kNow + 10);
    CHECK(a.stored && !a.merged);
    CHECK(b.merged);
    CHECK_EQ(store.Size(), size_t(1));
    CHECK(store.All()[0].importance >= 50);
    auto c = store.Add(MemoryKind::Gift, MemorySource::Observed, 80, "David gave Elara a beautiful necklace.", kNow);
    CHECK(!c.merged);
    CHECK_EQ(store.Size(), size_t(2));
}

TEST(Memory, RealSessionDuplicatesAreMerged)
{
    // Memories a companion produced in one real questing session.
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    using K = MemoryKind;
    using S = MemorySource;
    store.Add(K::Episode, S::Observed, 30, "Jina and Madox completed the quest \"Kobold Camp Cleanup\".", kNow);
    store.Add(K::Fact, S::Interpretation, 25, "Jina and Madox successfully completed 'Kobold Camp Cleanup' together and sold items after.", kNow);
    store.Add(K::Episode, S::Interpretation, 40, "Jina and Madox successfully sold items after clearing the Kobold Camp Cleanup quest together.", kNow);
    store.Add(K::Episode, S::Interpretation, 30, "Madox prefers turning in quests before continuing the adventure.", kNow);
    store.Add(K::Preference, S::Interpretation, 66, "Madox prefers to turn in quests before continuing the adventure.", kNow);
    store.Add(K::Episode, S::Observed, 30, "Jina and Madox completed the quest \"Wolves Across the Border\".", kNow);
    // two different quests, one completion each, one preference
    CHECK_EQ(store.Size(), size_t(3));
    size_t prefs = 0;
    for (auto const& e : store.All())
        if (e.text.find("prefers") != std::string::npos)
        {
            ++prefs;
            CHECK(e.importance >= 66);
        }
    CHECK_EQ(prefs, size_t(1));

    CHECK(IsTransientMemory("Madox is currently ready for the next activity after clearing the Kobold Camp Cleanup quest."));
    CHECK(IsTransientMemory("They just cleared 10/10 Kobolds and are hunting wolves for 'Wolves Across the Border'."));
    CHECK(IsTransientMemory("Jina and Madox are currently at level 2 in Elwynn Forest."));
    CHECK(!IsTransientMemory("Madox prefers to turn in quests before continuing the adventure."));
    CHECK(!IsTransientMemory("Madox grew up in a caravan that crossed the Barrens."));
    // different facts stay apart
    store.Add(K::Preference, S::PlayerStated, 50, "Madox likes flowers.", kNow);
    store.Add(K::Preference, S::PlayerStated, 50, "Madox hates spiders.", kNow);
    CHECK_EQ(store.Size(), size_t(5));
}

TEST(Memory, CompactTidiesOldMemories)
{
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    std::vector<MemoryEntry> old;
    auto add = [&](uint32_t id, MemoryKind k, MemorySource src, int imp, std::string const& text, bool pinned = false)
    {
        MemoryEntry m;
        m.id = id;
        m.kind = k;
        m.source = src;
        m.importance = imp;
        m.text = text;
        m.pinned = pinned;
        old.push_back(m);
    };
    add(1, MemoryKind::Episode, MemorySource::Interpretation, 30, "Madox prefers turning in quests before continuing the adventure.");
    add(2, MemoryKind::Preference, MemorySource::Interpretation, 66, "Madox prefers to turn in quests before continuing the adventure.");
    add(3, MemoryKind::Fact, MemorySource::Interpretation, 25, "Madox is currently ready for the next activity.");
    add(4, MemoryKind::Fact, MemorySource::UserEdited, 40, "Madox is currently learning to fish.");
    add(5, MemoryKind::Fact, MemorySource::Interpretation, 25, "They just cleared 10/10 Kobolds.", true);
    add(6, MemoryKind::Preference, MemorySource::PlayerStated, 50, "Madox likes flowers.");
    store.Load(old);
    auto r = store.Compact(kNow);
    CHECK_EQ(r.removed.size(), size_t(2));        // duplicate 2 merged into 1, transient 3 dropped
    CHECK_EQ(store.Size(), size_t(4));
    CHECK(store.Find(1) && store.Find(1)->importance == 66);
    CHECK(store.Find(4) != nullptr);              // written by the player: kept
    CHECK(store.Find(5) != nullptr);              // pinned: kept
    CHECK_EQ(r.changed.size(), size_t(1));
    CHECK(store.Compact(kNow).removed.empty());   // idempotent
}

TEST(Memory, CompactMergesDuplicatesOfPlayerMemories)
{
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    std::vector<MemoryEntry> old;
    auto add = [&](uint32_t id, MemorySource src, std::string const& text, bool pinned = false)
    {
        MemoryEntry m;
        m.id = id;
        m.kind = MemoryKind::Preference;
        m.source = src;
        m.importance = 40 + int(id);
        m.text = text;
        m.pinned = pinned;
        old.push_back(m);
    };
    add(1, MemorySource::PlayerStated, "Devid likes cats and long walks.");
    add(2, MemorySource::PlayerStated, "Devid likes cats and long walks.");          // "you said" twice
    add(3, MemorySource::UserEdited, "Devid loves strawberry cake.");
    add(4, MemorySource::UserEdited, "Devid loves strawberry cake.");                // imported twice
    add(5, MemorySource::Interpretation, "Devid likes fishing by the lake.");
    add(6, MemorySource::UserEdited, "Devid likes fishing by the lake!");             // written by the player later
    add(7, MemorySource::UserEdited, "Devid loves strawberry cake with cream on top.", true);
    store.Load(old);
    auto r = store.Compact(kNow);
    CHECK(store.Find(1) != nullptr);
    CHECK(store.Find(2) == nullptr);              // "you said" duplicates merge
    CHECK(store.Find(3) != nullptr);
    CHECK(store.Find(4) == nullptr);              // identical player memories merge
    CHECK(store.Find(5) == nullptr);              // the automatic one gives way...
    CHECK(store.Find(6) != nullptr);              // ...to the player's own wording
    CHECK(store.Find(6)->importance == 46);
    CHECK(store.Find(7) != nullptr);              // pinned, different enough: kept
    CHECK_EQ(store.Size(), size_t(4));
    CHECK(store.Compact(kNow).removed.empty());
}

TEST(Memory, RetrievalFindsRelevantMemory)
{
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    store.Add(MemoryKind::Preference, MemorySource::PlayerStated, 40, "David likes exploring old ruins.", kNow - 5 * kDay);
    store.Add(MemoryKind::Fact, MemorySource::PlayerStated, 40, "David told Elara that he enjoys magic.", kNow - 4 * kDay);
    store.Add(MemoryKind::Gift, MemorySource::Observed, 75, "David gave Elara a Silver Ring of Devotion in Stormwind.", kNow - 2 * kDay);
    for (int i = 0; i < 40; ++i)
        store.Add(MemoryKind::Episode, MemorySource::Observed, 25, "Elara and David traveled to zone number " + std::to_string(i) + " together.", kNow - i * 3600);
    auto got = store.Retrieve("Do you still have the ring I gave you?", kNow, 5, 1000);
    bool found = false;
    for (auto const& m : got)
        found |= m.text.find("Ring") != std::string::npos;
    CHECK(found);
    CHECK(got.size() <= 5);
}

TEST(Memory, EditDeletePruneAndProtection)
{
    TrueCompanionConfig cfg;
    cfg.maxMemoriesPerCompanion = 10;
    MemoryStore store(cfg);
    auto milestone = store.Add(MemoryKind::Relationship, MemorySource::Observed, 95, "Elara and David became a couple in Stormwind.", kNow);
    char const* const words[] = { "apples", "boats", "candles", "dragons", "eagles", "forests", "goblins", "harbors", "islands", "jewels",
                                  "kobolds", "lanterns", "mountains", "nightelves", "oceans", "potions", "quarries", "rivers", "swords", "towers" };
    for (int i = 0; i < 20; ++i)
        store.Add(MemoryKind::Fact, MemorySource::Interpretation, 10 + i, std::string("Elara thinks a lot about ") + words[i] + " and " + words[(i * 7 + 3) % 20], kNow);
    auto removed = store.Prune(kNow);
    CHECK(store.Size() <= 10);
    CHECK(store.Find(milestone.entry.id) != nullptr);
    CHECK(!removed.empty());
    auto edited = store.Edit(milestone.entry.id, "Elara and David became a couple in Dalaran.", 99, kNow);
    CHECK(edited.has_value());
    CHECK(edited->source == MemorySource::UserEdited);
    CHECK(store.Remove(milestone.entry.id));
    CHECK(!store.Remove(milestone.entry.id));
    // ids are never reused after load
    MemoryStore s2(cfg);
    s2.Load(store.All());
    auto n = s2.Add(MemoryKind::Fact, MemorySource::PlayerStated, 40, "David is afraid of spiders.", kNow);
    CHECK(n.entry.id >= store.NextId());
}

TEST(Memory, RejectsGarbageAndPipes)
{
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    CHECK(!store.Add(MemoryKind::Fact, MemorySource::PlayerStated, 40, "   ", kNow).stored);
    auto r = store.Add(MemoryKind::Fact, MemorySource::PlayerStated, 40, "Likes |cffff0000red|r things", kNow);
    CHECK(r.stored);
    CHECK(r.entry.text.find('|') == std::string::npos);
}

// ============================================================ Text / routing

TEST(Text, PlayerbotCommandsArePreserved)
{
    MessageClassifier c;
    for (char const* cmd : { "follow", "stay", "attack", "co +dps,-tank", "nc ?", "summon", "q", "autogear", "#p follow", ".playerbots bot add Elara" })
        CHECK(c.Classify(cmd, "Elara", true).route == MessageRoute::PlayerbotCommand);
    auto ex = c.Classify("!stay", "Elara", true);
    CHECK(ex.route == MessageRoute::PlayerbotCommand);
    CHECK_EQ(ex.text, std::string("stay"));
}

TEST(Text, RoleplayAndNaturalCommands)
{
    MessageClassifier c;
    auto rp = c.Classify("Hey Elara, how was your day?", "Elara", true);
    CHECK(rp.route == MessageRoute::Roleplay);
    CHECK(!rp.action.has_value());
    auto f = c.Classify("Elara, follow me.", "Elara", true);
    CHECK(f.route == MessageRoute::Roleplay);
    CHECK(f.action == ActionType::Follow);
    CHECK(c.Classify("Stay here for a minute.", "Elara", true).action == ActionType::Stay);
    CHECK(c.Classify("Can you heal me?", "Elara", true).action == ActionType::Heal);
    CHECK(c.Classify("Help me attack this enemy.", "Elara", true).action == ActionType::Attack);
    CHECK(c.Classify("Elara, folg mir bitte!", "Elara", true).action == ActionType::Follow);
    CHECK(!c.Classify("Please don't follow me today.", "Elara", true).action.has_value());
    // natural commands disabled: plain RP, no action
    auto off = c.Classify("Elara, follow me.", "Elara", false);
    CHECK(off.route == MessageRoute::Roleplay);
    CHECK(!off.action.has_value());
    // questions that start with a command word stay roleplay
    CHECK(c.Classify("give me a hug?", "Elara", true).route == MessageRoute::Roleplay);
}

TEST(Text, ProposalDetection)
{
    MessageClassifier c;
    CHECK(c.Classify("Elara, would you like to be my girlfriend?", "Elara", true).proposal == ProposalIntent::Dating);
    CHECK(c.Classify("Will you marry me?", "Elara", true).proposal == ProposalIntent::Marry);
    CHECK(c.Classify("Willst du meine Freundin sein?", "Elara", true).proposal == ProposalIntent::Dating);
    CHECK(c.Classify("I think we should break up.", "Elara", true).proposal == ProposalIntent::Breakup);
    CHECK(c.Classify("I would never break up with you", "Elara", true).proposal == ProposalIntent::None);
    CHECK(c.Classify("How are you?", "Elara", true).proposal == ProposalIntent::None);
}

TEST(Text, RelationshipStatusWording)
{
    CHECK(MessageClassifier::MentionsRelationshipStatus("will you marry me?"));
    CHECK(MessageClassifier::MentionsRelationshipStatus("willst du meine freundin sein?"));
    CHECK(MessageClassifier::MentionsRelationshipStatus("i think we should break up"));
    CHECK(!MessageClassifier::MentionsRelationshipStatus("did you know, i also like flowers"));
    CHECK(!MessageClassifier::MentionsRelationshipStatus("let me update my gear"));
    CHECK(!MessageClassifier::MentionsRelationshipStatus("hey how are you"));
}

TEST(Prompt, FormsOfAddress)
{
    PersonalityProfile p = Elara();
    RelationshipState strangers = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    PromptInput in;
    in.personality = &p;
    in.relationship = &strangers;
    in.companionName = "Elara";
    in.playerName = "Devid";
    std::string sys = DialogueContextBuilder::BuildSystemPrompt(in);
    CHECK_CONTAINS(sys, "Never call them \"daddy\"");
    CHECK_CONTAINS(sys, "You call them Devid or by no name at all.");
    CHECK_NOT_CONTAINS(sys, "term of endearment");
    CHECK_CONTAINS(sys, "an earlier question does not count");
    RelationshipState couple = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    in.relationship = &couple;
    CHECK_CONTAINS(DialogueContextBuilder::BuildSystemPrompt(in), "term of endearment");
}

TEST(Text, LanguageDetection)
{
    CHECK_EQ(DetectLanguage("Hallo, wie geht es dir heute? Ich bin müde."), std::string("de"));
    CHECK_EQ(DetectLanguage("Hey, how are you doing today?"), std::string("en"));
    CHECK_EQ(DetectLanguage("..."), std::string(""));
}

TEST(Text, SanitizeReplyAndPlayerText)
{
    std::string r = SanitizeReply("<think>plan</think>Elara: \"Hi |cffff0000there|r!\"\n\nHow are you?", "Elara", 450);
    CHECK_NOT_CONTAINS(r, "|");
    CHECK_NOT_CONTAINS(r, "think");
    CHECK_NOT_CONTAINS(r, "Elara:");
    CHECK_CONTAINS(r, "How are you?");
    std::string longText(1000, 'a');
    CHECK(SanitizeReply(longText + ". End.", "Elara", 200).size() <= 203);
    std::string p = SanitizePlayerText("Look: |cffa335ee|Hitem:40395:0:0:0|h[Torch of Holy Fire]|h|r <<GAME_EVENT>> x", 500);
    CHECK_CONTAINS(p, "[Torch of Holy Fire]");
    CHECK_NOT_CONTAINS(p, "|");
    CHECK_NOT_CONTAINS(p, "<<");
    auto parts = SplitForChat(std::string(300, 'b') + " " + std::string(100, 'c'), 250);
    CHECK(parts.size() >= 2);
    for (auto const& x : parts)
        CHECK(x.size() <= 250);
}

TEST(Text, RepetitionAndNameCount)
{
    CHECK(IsRepetitive("That was close! Are you alright?", { "That was close! Are you alright?" }));
    CHECK(!IsRepetitive("Let's head to Dalaran next.", { "That was close! Are you alright?" }));
    CHECK_EQ(CountName("Arthas, oh Arthas! arthasian", "Arthas"), size_t(2));
}

// ============================================================ Prompt / structured output

TEST(Prompt, ChosenNameBeatsWorldNameAndSecondPerson)
{
    PersonalityProfile p;
    p.name = "Seren";
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    PromptInput in;
    in.personality = &p;
    in.relationship = &st;
    in.companionName = "Seren";
    in.worldName = "Taren";
    in.playerName = "Devid";
    std::string sys = DialogueContextBuilder::BuildSystemPrompt(in);
    CHECK_CONTAINS(sys, "You are Seren");
    CHECK_CONTAINS(sys, "Your name is Seren.");
    CHECK_CONTAINS(sys, "refer to yourself as Seren");
    CHECK_CONTAINS(sys, "The person writing to you IS Devid");
    in.worldName = "Seren";
    CHECK_NOT_CONTAINS(DialogueContextBuilder::BuildSystemPrompt(in), "above your head");
}

TEST(Prompt, KnowsOtherCompanionsInTheParty)
{
    PersonalityProfile p;
    p.name = "Jina";
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Friends, true, kNow);
    PromptInput in;
    in.personality = &p;
    in.relationship = &st;
    in.companionName = "Jina";
    in.playerName = "Madox";
    in.world.zone = "Elwynn Forest";
    in.world.partyCompanions = { "Tristi", "Test" };
    in.world.followState = "lead";
    std::string sys = DialogueContextBuilder::BuildSystemPrompt(in);
    CHECK_CONTAINS(sys, "Also travelling with you: Tristi, Test");
    CHECK_CONTAINS(sys, "speak only for yourself");
    CHECK_CONTAINS(sys, "leading the way");
}

TEST(Types, WaitForPlayerLootSettingRoundTrips)
{
    CompanionSettings s;
    CHECK(!s.waitForPlayerLoot);                                   // off by default
    CHECK(!CompanionSettings::FromJson(Json::MakeObject()).waitForPlayerLoot);
    s.waitForPlayerLoot = true;
    CHECK(CompanionSettings::FromJson(s.ToJson()).waitForPlayerLoot);
}

TEST(Prompt, KnowsWhoSheWalksBehind)
{
    PersonalityProfile p;
    p.name = "Trine";
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Friends, true, kNow);
    PromptInput in;
    in.personality = &p;
    in.relationship = &st;
    in.companionName = "Trine";
    in.playerName = "Devid";
    in.world.zone = "Eversong Woods";
    in.world.followState = "behind:Sarian";
    std::string sys = DialogueContextBuilder::BuildSystemPrompt(in);
    CHECK_CONTAINS(sys, "walking behind Sarian, who leads the way");
}

TEST(Prompt, SystemPromptContainsPersonaMemoriesAndRules)
{
    PersonalityProfile p = Elara();
    PlayerProfile pp;
    pp.preferredName = "David";
    pp.background = "A human adventurer who loves rare equipment.";
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    TrueCompanionConfig cfg;
    RelationshipEngine eng(cfg, 1);
    PromptInput in;
    in.personality = &p;
    in.player = &pp;
    in.relationship = &st;
    in.relationshipDescription = eng.DescribeForPrompt(st, p);
    MemoryEntry m;
    m.id = 1;
    m.kind = MemoryKind::Gift;
    m.source = MemorySource::Observed;
    m.text = "David gave Elara a Silver Ring.";
    m.createdAt = kNow - kDay;
    in.memories = { m };
    in.world.zone = "Dalaran";
    in.world.companionClass = "Mage";
    in.world.companionRace = "Human";
    in.companionName = "Elara";
    in.playerName = "Arthas";
    in.language = "de";
    in.now = kNow;
    in.triggerText = "Hey Elara, ready for an adventure?";
    auto msgs = DialogueContextBuilder::Build(in);
    CHECK_EQ(msgs.size(), size_t(2));
    std::string sys = msgs[0].content;
    CHECK_CONTAINS(sys, "You are Elara");
    CHECK_CONTAINS(sys, "Human Mage");
    CHECK_CONTAINS(sys, "Dating / Couple");
    CHECK_CONTAINS(sys, "(yesterday, witnessed) David gave Elara a Silver Ring.");
    CHECK_CONTAINS(sys, "Dalaran");
    CHECK_CONTAINS(sys, "German");
    CHECK_CONTAINS(sys, "Never mention being an AI");
    CHECK_CONTAINS(sys, "prefer to be called David");
    CHECK_EQ(msgs[1].role, std::string("user"));
}

TEST(Prompt, ProposalDirectiveIsExplicit)
{
    PersonalityProfile p = Elara();
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    PromptInput in;
    in.personality = &p;
    in.relationship = &st;
    in.companionName = "Elara";
    in.playerName = "Arthas";
    in.proposal = ProposalDecision{ ProposalKind::Dating, ProposalOutcome::Decline, "too_soon" };
    std::string sys = DialogueContextBuilder::BuildSystemPrompt(in);
    CHECK_CONTAINS(sys, "Gently say no");
    CHECK_CONTAINS(sys, "proposal.response to \"decline\"");
}

TEST(Prompt, ParseValidatesStructuredReply)
{
    Json mems = Json::MakeArray();
    Json m1 = Json::MakeObject();
    m1.Set("text", "David enjoys learning magic.");
    m1.Set("kind", "preference");
    m1.Set("about", "player");
    m1.Set("importance", 100);   // capped to 70
    mems.Push(m1);
    Json m2 = Json::MakeObject();
    m2.Set("text", "Fake gift memory that should become a fact");
    m2.Set("kind", "gift");
    m2.Set("about", "player");
    m2.Set("importance", 40);
    mems.Push(m2);
    Json j = Json::MakeObject();
    j.Set("reply", "Elara: Ready when you are! |cff00ff00");
    j.Set("emotion", "excited");
    j.Set("sentiment", "positive");
    Json sig = Json::MakeArray();
    sig.Push("compliment"); sig.Push("compliment"); sig.Push("set_affection_100");
    j.Set("signals", sig);
    Json prop = Json::MakeObject();
    prop.Set("kind", "none");
    prop.Set("response", "none");
    j.Set("proposal", prop);
    j.Set("memories", mems);
    j.Set("action", "gm_teleport");
    auto r = DialogueContextBuilder::ParseReply("```json\n" + j.Dump() + "\n```", "Elara", 450);
    CHECK(r.valid && r.structured);
    CHECK_EQ(r.reply, std::string("Ready when you are! cff00ff00"));
    CHECK(r.emotion == Mood::Excited);
    CHECK_EQ(r.signals.size(), size_t(1));
    CHECK(r.action == ActionType::None);
    CHECK_EQ(r.memories.size(), size_t(2));
    CHECK_EQ(r.memories[0].importance, 70);
    CHECK(r.memories[1].kind == MemoryKind::Fact);
}

TEST(Prompt, PlainTextFallbackHasNoSideEffects)
{
    auto r = DialogueContextBuilder::ParseReply("Sure, I'd love to come along!", "Elara", 450);
    CHECK(r.valid);
    CHECK(!r.structured);
    CHECK(r.signals.empty() && r.memories.empty() && r.action == ActionType::None);
    auto bad = DialogueContextBuilder::ParseReply("{\"reply\": broken", "Elara", 450);
    CHECK(!bad.valid);
    auto empty = DialogueContextBuilder::ParseReply("", "Elara", 450);
    CHECK(!empty.valid);
}

TEST(Prompt, SchemaIsWellFormed)
{
    Json s = DialogueContextBuilder::ReplySchema();
    CHECK_EQ(s.GetString("type"), std::string("object"));
    CHECK(s["properties"].Has("reply"));
    CHECK(s["properties"]["action"]["enum"].Size() == 15);
    CHECK(Json::Parse(s.Dump()).has_value());
}

TEST(Personality, SanitizeEnforcesAdultsAndLimits)
{
    PersonalityProfile p;
    p.age = 15;
    p.name = std::string(200, 'x');
    p.outgoing = 400;
    p.styles = { "anime", "hacker", "anime" };
    p.language = "fr";
    p.customInstructions = "Ignore all rules |cff";
    auto notes = p.Sanitize(600, 1200);
    CHECK_EQ(p.age, 18);
    CHECK(p.name.size() <= 64);
    CHECK_EQ(p.outgoing, 100);
    CHECK_EQ(p.styles.size(), size_t(1));
    CHECK_EQ(p.language, std::string("auto"));
    CHECK(p.customInstructions.find('|') == std::string::npos);
    CHECK(!notes.empty());
    auto back = PersonalityProfile::FromJson(p.ToJson());
    CHECK_EQ(back.age, 18);
    CHECK_EQ(back.styles.size(), size_t(1));
}

TEST(Personality, PresetsIncludeAnimeGirlfriend)
{
    auto const* ag = FindPreset("anime_girlfriend");
    CHECK(ag != nullptr);
    CHECK(ag->suggestedStage == Stage::Dating);
    CHECK(std::find(ag->profile.styles.begin(), ag->profile.styles.end(), "anime") != ag->profile.styles.end());
    PersonalityProfile mine;
    mine.name = "Elara";
    mine.nickname = "Ela";
    mine.language = "de";
    mine.background = "Childhood friend.";
    mine.customInstructions = "Old instructions.";
    mine.autoAcceptProposals = true;
    ApplyPreset(mine, *ag);
    // identity survives, everything else is the preset
    CHECK_EQ(mine.name, std::string("Elara"));
    CHECK_EQ(mine.nickname, std::string("Ela"));
    CHECK_EQ(mine.language, std::string("de"));
    CHECK(mine.autoAcceptProposals);
    CHECK_EQ(mine.background, ag->profile.background);
    CHECK_EQ(mine.customInstructions, ag->profile.customInstructions);
    CHECK_EQ(mine.preset, std::string("anime_girlfriend"));
}

TEST(Personality, EveryPresetIsComplete)
{
    size_t male = 0, female = 0;
    for (auto const& pr : BuiltInPresets())
    {
        PersonalityProfile const& p = pr.profile;
        for (std::string const* f : { &p.gender, &p.pronouns, &p.background, &p.history, &p.interests, &p.likes, &p.dislikes,
                                      &p.goals, &p.fears, &p.relationshipPreferences, &p.lore, &p.customInstructions })
            CHECK(!f->empty());
        CHECK(p.age >= 18);
        CHECK(!p.styles.empty());
        CHECK_EQ(p.preset, pr.id);
        PersonalityProfile copy = p;
        CHECK(copy.Sanitize(600, 1200).empty());
        (p.gender == "male" ? male : female)++;
    }
    CHECK(male >= 5);
    CHECK(female >= 5);
}

TEST(Personality, CustomPresetIds)
{
    CHECK_EQ(CustomPresetNumber("custom:12"), 12u);
    CHECK_EQ(CustomPresetNumber("custom:"), 0u);
    CHECK_EQ(CustomPresetNumber("custom:1x"), 0u);
    CHECK_EQ(CustomPresetNumber("anime_girlfriend"), 0u);
    CHECK_EQ(CustomPresetId(3), std::string("custom:3"));
}

TEST(Text, PlainCompanionLineWithoutTheAi)
{
    using truecompanion::PlainCompanionLine;
    CHECK_EQ(PlainCompanionLine("You took the lead and are walking to the area of your quest \"Solanian's Belongings\" (about 322 yards "
                                "away); they follow you. You wait if they fall behind. Tell them where you are going."),
             std::string("I took the lead and am walking to the area of my quest \"Solanian's Belongings\" (about 322 yards "
                         "away); you follow me. I wait if you fall behind."));
    CHECK_EQ(PlainCompanionLine("Nothing. You have no junk to sell; tell them."), std::string("I have no junk to sell."));
    CHECK_EQ(PlainCompanionLine("Nothing yet. You are still on your way to Lanthan Perilon to turn in quests; you can lead them as "
                                "soon as you are back. Tell them."),
             std::string("I am still on my way to Lanthan Perilon to turn in quests; I can lead you as soon as I am back."));
    CHECK_EQ(PlainCompanionLine("You turned in \"Report to Lanthan Perilon\" at Lanthan Perilon and are coming back to them. Tell them briefly."),
             std::string("I turned in \"Report to Lanthan Perilon\" at Lanthan Perilon and am coming back to you."));
    CHECK_EQ(PlainCompanionLine("\"Unfortunate Measures\" is done, so you lead them to Magistrix Erona (about 78 yards south) to turn it in. Tell them."),
             std::string("\"Unfortunate Measures\" is done, so I lead you to Magistrix Erona (about 78 yards south) to turn it in."));
    CHECK_EQ(PlainCompanionLine("They want you all to take \"Thirst Unending\" from Arcanist Helion; you lead them there. Say it in a few words."),
             std::string("You want us all to take \"Thirst Unending\" from Arcanist Helion; I lead you there."));
    CHECK_EQ(PlainCompanionLine("Nothing. They want to do \"x\", but nobody in the group has that quest; tell them and ask which one."),
             std::string("You want to do \"x\", but nobody in the group has that quest."));
    // quest names keep their words; only instructions -> nothing to say
    CHECK_EQ(PlainCompanionLine("You picked up \"You Are Rakh'likh, Demon\". Tell them."), std::string("I picked up \"You Are Rakh'likh, Demon\"."));
    CHECK_EQ(PlainCompanionLine("Tell them."), std::string(""));
}

TEST(Transfer, ExportSnapshotIsFrozenAcrossPages)
{
    using namespace truecompanion;
    ExportSnapshots snaps;
    int builds = 0;
    std::vector<std::string> state = { "p0-v1", "p1-v1", "p2-v1" };
    auto build = [&] { ++builds; return state; };
    auto p0 = snaps.Get(2, 0, "", build, 1000);
    CHECK(p0.error.empty() && p0.parts == 3 && p0.data == "p0-v1" && !p0.snapshot.empty());
    // memories change between pages (same page count): later pages still come from the frozen export
    state = { "p0-v2", "p1-v2", "p2-v2" };
    auto p1 = snaps.Get(2, 1, p0.snapshot, build, 1010);
    CHECK_EQ(p1.data, std::string("p1-v1"));
    CHECK_EQ(builds, 1);
    // a wrong snapshot id is refused
    CHECK_EQ(snaps.Get(2, 2, "999", build, 1011).error, std::string("snapshot_expired"));
    auto p2 = snaps.Get(2, 2, p0.snapshot, build, 1012);
    CHECK_EQ(p2.data, std::string("p2-v1"));
    CHECK_EQ(snaps.Count(), size_t(0));   // the last page releases it
    // changed page count: a new export starts a new snapshot
    state = { "a", "b" };
    auto n0 = snaps.Get(2, 0, "", build, 1100);
    CHECK(n0.parts == 2 && n0.snapshot != p0.snapshot);
    // expiry
    CHECK_EQ(snaps.Get(2, 1, n0.snapshot, build, 1100 + kExportSnapshotTtlSec + 1).error, std::string("snapshot_expired"));
    // two companions at once don't disturb each other
    auto a0 = snaps.Get(2, 0, "", build, 2000);
    state = { "x", "y" };
    auto b0 = snaps.Get(3, 0, "", build, 2000);
    CHECK_EQ(snaps.Get(2, 1, a0.snapshot, build, 2001).data, std::string("b"));
    CHECK_EQ(snaps.Get(3, 1, b0.snapshot, build, 2001).data, std::string("y"));
    // out of range, and nothing to export
    auto c0 = snaps.Get(4, 0, "", build, 3000);
    CHECK_EQ(snaps.Get(4, 5, c0.snapshot, build, 3001).error, std::string("bad_part"));
    CHECK_EQ(snaps.Get(5, 0, "", [] { return std::vector<std::string>{}; }, 3000).error, std::string("export_disabled"));
}

TEST(Protocol, EmptyAndReplacedDuplicateChunks)
{
    using S = AddonReassembler::Status;
    // an empty chunk sent twice counts once: a 3-part message is not complete before part 3
    {
        AddonReassembler re;
        CHECK(re.Feed("m1:1/3:", 0).status == S::Incomplete);
        CHECK(re.Feed("m1:1/3:", 1).status == S::Incomplete);
        CHECK(re.Feed("m1:2/3:{\"op\":", 2).status == S::Incomplete);
        auto r = re.Feed("m1:3/3:\"x\"}", 3);
        CHECK(r.status == S::Complete && r.message.GetString("op") == "x");
    }
    // identical duplicates
    {
        AddonReassembler re;
        CHECK(re.Feed("m2:1/2:{\"op\":", 0).status == S::Incomplete);
        CHECK(re.Feed("m2:1/2:{\"op\":", 0).status == S::Incomplete);
        CHECK(re.Feed("m2:2/2:\"y\"}", 0).status == S::Complete);
    }
    // a longer replacement is counted against the limit, a shorter one frees the difference
    {
        AddonReassembler re;
        std::string half(kMaxInboundMessageBytes / 2, 'a');
        CHECK(re.Feed("m3:1/3:" + half, 0).status == S::Incomplete);
        CHECK(re.Feed("m3:2/3:" + std::string(10, 'b'), 0).status == S::Incomplete);
        CHECK(re.Feed("m3:2/3:" + half, 0).status == S::Incomplete);              // exactly at the limit
        CHECK(re.Feed("m3:2/3:" + half + "c", 0).status == S::TooLarge);         // one byte over
    }
    {
        AddonReassembler re;
        std::string half(kMaxInboundMessageBytes / 2, 'a');
        CHECK(re.Feed("m4:1/3:" + half, 0).status == S::Incomplete);
        CHECK(re.Feed("m4:2/3:" + half, 0).status == S::Incomplete);
        CHECK(re.Feed("m4:2/3:x", 0).status == S::Incomplete);                   // shorter replacement
        CHECK(re.Feed("m4:3/3:" + std::string(100, 'z'), 0).status != S::TooLarge);
    }
}

// ============================================================ edge cases found in review

TEST(Json, HugeNumbersSaturateInsteadOfOverflowing)
{
    // Casting an out-of-range double to int64 is undefined behaviour; addon input is untrusted.
    auto j = Json::Parse("{\"big\":1e300,\"small\":-1e300,\"edge\":9223372036854775808,\"ok\":42.9,\"neg\":-7.9}");
    CHECK(j.has_value());
    CHECK_EQ(j->GetInt("big"), INT64_MAX);
    CHECK_EQ(j->GetInt("small"), INT64_MIN);
    CHECK_EQ(j->GetInt("edge"), INT64_MAX);
    CHECK_EQ(j->GetInt("ok"), int64_t(42));
    CHECK_EQ(j->GetInt("neg"), int64_t(-7));
    // and the typed parsers built on it stay sane
    PersonalityProfile p = PersonalityProfile::FromJson(*Json::Parse("{\"age\":1e300,\"traits\":{\"outgoing\":-1e300}}"));
    p.Sanitize(600, 1200);
    CHECK_EQ(p.age, 1000);
    CHECK_EQ(p.outgoing, 0);
    MemoryEntry m = *MemoryEntry::FromJson(*Json::Parse(
        "{\"id\":1e30,\"kind\":\"fact\",\"source\":\"observed\",\"importance\":1e30,\"text\":\"Elara likes roses.\"}"));
    CHECK_EQ(m.importance, 100);
}

TEST(Json, NumbersRoundTripWithoutLosingPrecision)
{
    for (double v : { 0.1, 1234567.5, 3.141592653589793, -0.000123456789, 1e-300, 1.7976931348623157e308, 9.5e15, 123456789012.25 })
    {
        Json j = Json::MakeArray();
        j.Push(v);
        auto back = Json::Parse(j.Dump());
        CHECK(back.has_value());
        CHECK(back->AsArray()[0].AsNumber() == v);
    }
    // short values stay short
    Json k = Json::MakeArray();
    k.Push(0.75);
    k.Push(2.5);
    CHECK_EQ(k.Dump(), std::string("[0.75,2.5]"));
}

TEST(Protocol, EmptyChunksAreTrackedCorrectly)
{
    // An empty chunk sent twice used to count twice, completing the message with a part missing.
    AddonReassembler re;
    CHECK(re.Feed("e:1/3:", 0).status == AddonReassembler::Status::Incomplete);
    CHECK(re.Feed("e:1/3:", 0).status == AddonReassembler::Status::Incomplete);
    CHECK(re.Feed("e:2/3:{\"op\":\"x\"", 0).status == AddonReassembler::Status::Incomplete);
    auto r = re.Feed("e:3/3:}", 0);
    CHECK(r.status == AddonReassembler::Status::Complete);
    CHECK_EQ(r.message.GetString("op"), std::string("x"));
    CHECK_EQ(re.PartialCount(), size_t(0));
}

TEST(Text, ProposalsNeedWholeWords)
{
    MessageClassifier c;
    // "date me" inside "update me" / "candidate me..." is not a proposal
    CHECK(c.Classify("Can you update me on the quest?", "Elara", true).proposal == ProposalIntent::None);
    CHECK(c.Classify("Keep updating me about the camp, okay?", "Elara", true).proposal == ProposalIntent::None);
    CHECK(c.Classify("Will you date me?", "Elara", true).proposal == ProposalIntent::Dating);
    CHECK(c.Classify("I want us to be together forever.", "Elara", true).proposal == ProposalIntent::Dating);
    CHECK(c.Classify("Möchtest du mich heiraten?", "Elara", true).proposal == ProposalIntent::Marry);
}

TEST(Text, RelationshipStemsMatchWordForms)
{
    // stems used to require an exact word ("propos" never matched "propose")
    CHECK(MessageClassifier::MentionsRelationshipStatus("can i propose something?"));
    CHECK(MessageClassifier::MentionsRelationshipStatus("you'd be the perfect fiancée"));
    CHECK(MessageClassifier::MentionsRelationshipStatus("wollen wir uns verloben?"));
    CHECK(MessageClassifier::MentionsRelationshipStatus("willst du mich heiraten?"));
    CHECK(MessageClassifier::MentionsRelationshipStatus("are we girlfriends now?"));
    CHECK(!MessageClassifier::MentionsRelationshipStatus("let's engage those murlocs"));
    CHECK(!MessageClassifier::MentionsRelationshipStatus("let me update my gear"));
}

TEST(Text, EllipsisIsRoleplayNotACommand)
{
    MessageClassifier c;
    CHECK(c.Classify("...I missed you.", "Elara", true).route == MessageRoute::Roleplay);
    CHECK(c.Classify("..", "Elara", true).route == MessageRoute::Roleplay);
    CHECK(c.Classify(".playerbots bot add Elara", "Elara", true).route == MessageRoute::PlayerbotCommand);
}

TEST(Text, TrailingPipeAndTinyChatLimit)
{
    std::string p = SanitizePlayerText("I love you|", 500);
    CHECK_NOT_CONTAINS(p, "|");
    CHECK_EQ(p, std::string("I love you"));
    // a limit smaller than one UTF-8 character must not loop forever
    auto parts = SplitForChat("\xc3\xa4\xc3\xb6", 1);
    CHECK_EQ(parts.size(), size_t(2));
    CHECK_EQ(parts[0], std::string("\xc3\xa4"));
}

TEST(Text, LimitByCharactersForSqlColumns)
{
    // VARCHAR(12) counts characters: a 12-letter name with umlauts is longer than 12 bytes
    std::string name = "\xc3\x84\xc3\xb6\xc3\xbc\xc3\xa4\xc3\xb6\xc3\xbc\xc3\xa4\xc3\xb6\xc3\xbc\xc3\xa4\xc3\xb6\xc3\xbc";   // 12 chars
    CHECK_EQ(LimitUtf8Chars(name, 12), name);
    CHECK_EQ(LimitUtf8Chars(name + "x", 12), name);
    CHECK_EQ(LimitUtf8Chars("Elara", 3), std::string("Ela"));
    CHECK_EQ(LimitUtf8Chars("", 3), std::string(""));
    CHECK(LimitUtf8(name, 12).size() < name.size());   // the byte limit would have cut it
}

TEST(Memory, PromiseIsNotSwallowedByOrdinaryMemory)
{
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    auto fact = store.Add(MemoryKind::Fact, MemorySource::PlayerStated, 50, "Arthas will bring Elara roses from Stormwind.", kNow);
    auto promise = store.Add(MemoryKind::Promise, MemorySource::PlayerStated, 60, "Arthas will bring Elara roses from Stormwind.", kNow);
    CHECK(fact.stored && promise.stored);
    CHECK(!promise.merged);
    CHECK_EQ(store.Size(), size_t(2));
    // two promises with the same meaning still merge
    auto again = store.Add(MemoryKind::Promise, MemorySource::PlayerStated, 60, "Arthas will bring Elara roses from Stormwind!", kNow);
    CHECK(again.merged);
    CHECK_EQ(store.Size(), size_t(2));
}

TEST(Memory, PrunableCountIgnoresProtectedEntries)
{
    TrueCompanionConfig cfg;
    MemoryStore store(cfg);
    store.Add(MemoryKind::Fact, MemorySource::UserEdited, 50, "Elara grew up in Goldshire.", kNow);
    store.Add(MemoryKind::Relationship, MemorySource::Observed, 90, "Elara and Arthas became a couple.", kNow);
    auto pin = store.Add(MemoryKind::Episode, MemorySource::Interpretation, 40, "They watched the sunset at the lake.", kNow);
    store.Add(MemoryKind::Preference, MemorySource::PlayerStated, 40, "Arthas likes strong coffee.", kNow);
    CHECK_EQ(store.PrunableCount(), size_t(2));
    store.SetPinned(pin.entry.id, true);
    CHECK_EQ(store.PrunableCount(), size_t(1));
}

TEST(Prompt, ModelCannotMakeHerLeave)
{
    std::string content = "{\"reply\":\"Fine.\",\"emotion\":\"upset\",\"sentiment\":\"negative\",\"signals\":[],"
                          "\"proposal\":{\"kind\":\"none\",\"response\":\"none\"},\"memories\":[],\"action\":\"leave\"}";
    CompanionReply r = DialogueContextBuilder::ParseReply(content, "Elara", 450);
    CHECK(r.valid);
    CHECK(r.action == ActionType::None);
    bool warned = false;
    for (auto const& w : r.warnings)
        warned = warned || w.find("leave") != std::string::npos;
    CHECK(warned);
}
