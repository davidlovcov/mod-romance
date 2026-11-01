/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 * Unit tests: JSON, protocol, relationship, gifts, memory, text, prompts.
 */

#include "TestFramework.h"

#include "RomanceGift.h"
#include "RomanceJson.h"
#include "RomanceMemory.h"
#include "RomancePresets.h"
#include "RomancePrompt.h"
#include "RomanceProtocol.h"
#include "RomanceRelationship.h"
#include "RomanceText.h"

using namespace romance;

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
        CHECK(c.size() + 5 <= 255);              // prefix "ARMN" + tab + body fits an addon message
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
    RomanceConfig cfg;
    RelationshipEngine eng(cfg, 1);
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Strangers, true, kNow);
    PersonalityProfile p = Elara();
    for (int i = 0; i < 500; ++i)
        eng.ApplyConversation(st, Sentiment::VeryPositive, { Signal::Compliment, Signal::Kindness, Signal::Flirt }, p, 40, kNow + i);
    double total = (st.v.affection - 5) + (st.v.friendship - 10) + (st.v.attraction - 5) + (st.v.trust - 20);
    CHECK(total <= cfg.dailyGainCap + 0.01 + 3.0);   // daily cap (+ satisfaction/comfort/familiarity share the cap)
    CHECK(st.stage <= Stage::Acquaintances);
}

TEST(Relationship, ProgressesGraduallyOverDays)
{
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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

TEST(Gift, CategoriesAndRingMeansMoreToPartner)
{
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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

TEST(Memory, RetrievalFindsRelevantMemory)
{
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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
    RomanceConfig cfg;
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

TEST(Prompt, SystemPromptContainsPersonaMemoriesAndRules)
{
    PersonalityProfile p = Elara();
    PlayerProfile pp;
    pp.preferredName = "David";
    pp.background = "A human adventurer who loves rare equipment.";
    RelationshipState st = RelationshipEngine::InitialFor(Stage::Dating, true, kNow);
    RomanceConfig cfg;
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
    mine.background = "Childhood friend.";
    ApplyPreset(mine, *ag);
    CHECK_EQ(mine.name, std::string("Elara"));
    CHECK_EQ(mine.background, std::string("Childhood friend."));
    CHECK_EQ(mine.preset, std::string("anime_girlfriend"));
}
