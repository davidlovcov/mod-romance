/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Integration tests of TrueCompanionService with the mocked LLM backend and the
 * in-memory repository: conversation flow, proposals, gifts, persistence across
 * "restarts", offline/timeout handling, saturation, proactive limits, isolation
 * between companions, and thread-safety of state mutation.
 */

#include "TestFramework.h"

#include "MockLlm.h"
#include "TrueCompanionPresets.h"
#include "TrueCompanionProtocol.h"
#include "TrueCompanionService.h"
#include "TrueCompanionTransfer.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace truecompanion;
using namespace truecompanion::test;

namespace
{
    constexpr Guid kArthas = 1;      // the player's main
    constexpr Guid kElara = 2;       // companion on the same account
    constexpr Guid kLyra = 3;
    constexpr Guid kOtherPlayer = 10;
    constexpr uint32_t kAccount = 100;

    struct Harness
    {
        TrueCompanionConfig cfg;
        InMemoryRepository repo;
        std::shared_ptr<MockLlm> llm = std::make_shared<MockLlm>();
        std::unique_ptr<TrueCompanionService> svc;
        WorldContext ctx;
        TimePoint now = 1'800'000'000;
        std::vector<OutgoingAction> out;

        Harness()
        {
            cfg.proactiveEnabled = false;   // individual tests opt in
            cfg.minResponseCooldownMs = 0;  // tested separately
            cfg.minDaysDatingBeforeEngagement = 0;
            cfg.minDaysEngagedBeforeMarriage = 0;
            Start();
            ctx.zone = "Elwynn Forest";
            ctx.companionClass = "Mage";
            ctx.companionRace = "Human";
            ctx.companionLevel = 80;
            ctx.playerName = "Arthas";
            ctx.playerClass = "Paladin";
            ctx.playerRace = "Human";
            ctx.playerLevel = 80;
            ctx.sameGroup = true;
            ctx.companionInWorld = true;
            ctx.partySize = 2;
        }

        void Start() { svc = std::make_unique<TrueCompanionService>(cfg, repo, llm, 42); }
        void Restart()
        {
            svc->Shutdown();
            svc.reset();
            out.clear();
            Start();
        }

        CompanionKey Key(Guid owner = kArthas, Guid comp = kElara) { return { owner, comp }; }

        void Activate(CompanionKey k, std::string const& name = "Elara")
        {
            auto r = svc->Activate(k, kAccount, name, "Arthas", now);
            CHECK(r.ok);
        }

        // Ticks until `pred` holds over collected outgoing actions or timeout.
        bool PumpUntil(std::function<bool(std::vector<OutgoingAction> const&)> pred, int timeoutMs = 3000)
        {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline)
            {
                svc->Tick(now, [this](CompanionKey const&) { return std::optional<WorldContext>(ctx); });
                for (auto& a : svc->TakeOutgoing())
                    out.push_back(a);
                if (pred(out))
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        }

        std::vector<std::string> Spoken(CompanionKey k)
        {
            std::vector<std::string> s;
            for (auto const& a : out)
                if (a.type == OutgoingAction::Type::Speak && a.key == k)
                    s.push_back(a.text);
            return s;
        }

        bool WaitSpeak(CompanionKey k, size_t count = 1, int timeoutMs = 3000)
        {
            return PumpUntil([&](auto const&) { return Spoken(k).size() >= count; }, timeoutMs);
        }

        bool Has(OutgoingAction::Type t, ActionType a = ActionType::None)
        {
            for (auto const& x : out)
                if (x.type == t && (a == ActionType::None || x.action == a))
                    return true;
            return false;
        }
    };

    std::string LastUserMessage(LlmRequest const& r) { return r.messages.empty() ? "" : r.messages.back().content; }
}

TEST(Service, OnlySelectedCompanionsReachTheModel)
{
    Harness h;
    // Not activated: every message passes through untouched and no LLM call happens.
    auto r = h.svc->OnPlayerMessage(h.Key(), "Hey Elara, how was your day?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::NotForUs);
    h.Activate(h.Key());
    // Another bot (Lyra) of the same player is not an AI companion.
    auto other = h.svc->OnPlayerMessage(h.Key(kArthas, kLyra), "Hi Lyra", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(other.result == TrueCompanionService::MessageResult::NotForUs);
    h.PumpUntil([](auto const&) { return false; }, 100);
    CHECK_EQ(h.llm->CallCount(), size_t(0));
}

TEST(Service, ChatRoundTripOnWorkerThreadAppliedOnGameThread)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->QueueReply(ReplyJson("Pretty good! Although I think I've had enough of fighting murlocs for one day. What about you?",
                                "happy", "positive", { "kindness" }));
    RelationshipValues before = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.v;
    auto r = h.svc->OnPlayerMessage(h.Key(), "Hey Elara, how was your day?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    // Nothing is applied before Tick runs on the game thread
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    RelationshipValues mid = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.v;
    CHECK_NEAR(mid.friendship, before.friendship, 1e-9);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_CONTAINS(h.Spoken(h.Key())[0], "murlocs");
    auto calls = h.llm->Calls();
    CHECK_EQ(calls.size(), size_t(1));
    CHECK(calls[0].thread != std::this_thread::get_id());
    CHECK_EQ(LastUserMessage(calls[0].req), std::string("Hey Elara, how was your day?"));
    RelationshipValues after = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.v;
    CHECK(after.friendship > before.friendship);
    // history persisted
    auto chat = h.repo.LoadRecentChat(h.Key(), 10);
    CHECK_EQ(chat.size(), size_t(2));
    CHECK(chat[1].role == ChatTurn::Role::Companion);
    // typing indicator pushed on and off
    int typingOn = 0, typingOff = 0;
    for (auto const& a : h.out)
        if (a.type == OutgoingAction::Type::UiPush && a.payload.GetString("op") == "typing")
            (a.payload.GetBool("on") ? typingOn : typingOff)++;
    CHECK(typingOn >= 1 && typingOff >= 1);
}

TEST(Service, AddonGetsTheReplyBeforeTheChatLine)
{
    // The addon hides Playerbots' feedback lines and shows the companion's own words; it can only tell them
    // apart if the reply text arrives before the visible chat line.
    Harness h;
    h.Activate(h.Key());
    h.llm->QueueReply(ReplyJson("Of course, right behind you.", "happy", "neutral", {}));
    h.out.clear();
    h.svc->OnPlayerMessage(h.Key(), "Elara, come with me", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    int push = -1, speak = -1;
    for (size_t i = 0; i < h.out.size(); ++i)
    {
        auto const& a = h.out[i];
        if (push < 0 && a.type == OutgoingAction::Type::UiPush && a.payload.GetString("op") == "chat_msg"
            && a.payload.GetString("from") == "companion")
            push = int(i);
        if (speak < 0 && a.type == OutgoingAction::Type::Speak)
            speak = int(i);
    }
    CHECK(push >= 0);
    CHECK(speak >= 0);
    CHECK(push < speak);
}

TEST(Service, CompanionCommandsWinOverPlayerbotsCommandWords)
{
    Harness h;
    h.Activate(h.Key());
    // these start with Playerbots command words but are companion commands
    for (char const* line : { "sell your junk", "Vendor, sell trash", "attack", "accept the quest", "Elara, attack all here",
                              "give me 1 gold" })
    {
        auto req = h.svc->PrepareControl(h.Key(), line, "Elara");
        CHECK(req.command.has_value());
    }
    CHECK(h.svc->PrepareControl(h.Key(), "sell your junk", "Elara").command->kind == CompanionCommandKind::SellJunk);
    CHECK(h.svc->PrepareControl(h.Key(), "Vendor, sell trash", "Elara").command->kind == CompanionCommandKind::SellJunk);
    CHECK(h.svc->PrepareControl(h.Key(), "attack", "Elara").command->kind == CompanionCommandKind::AttackTarget);
    CHECK(h.svc->PrepareControl(h.Key(), "accept the quest", "Elara").command->kind == CompanionCommandKind::AcceptQuests);
    // real Playerbots commands still pass through
    for (char const* line : { "!sell your junk", "stats", "co +dps", "nc ?" })
    {
        auto r = h.svc->OnPlayerMessage(h.Key(), line, ChatChannel::Whisper, h.ctx, h.now);
        CHECK(r.result == TrueCompanionService::MessageResult::PassToPlayerbots);
        CHECK(!h.svc->PrepareControl(h.Key(), line, "Elara").command.has_value());
    }
    // and the companion command gets an AI answer instead of going to Playerbots
    h.llm->QueueReply(ReplyJson("Sold it all!", "happy", "neutral", {}));
    auto r = h.svc->OnPlayerMessage(h.Key(), "sell your junk", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(!r.command.has_value());
}

TEST(Service, PlayerbotCommandsPassThroughAndNaturalCommandsExecute)
{
    Harness h;
    h.Activate(h.Key());
    auto cmd = h.svc->OnPlayerMessage(h.Key(), "follow", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(cmd.result == TrueCompanionService::MessageResult::PassToPlayerbots);
    CHECK_EQ(cmd.rewritten, std::string("follow"));
    auto bang = h.svc->OnPlayerMessage(h.Key(), "!co +dps", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(bang.result == TrueCompanionService::MessageResult::PassToPlayerbots);
    CHECK_EQ(bang.rewritten, std::string("co +dps"));
    CHECK_EQ(h.llm->CallCount(), size_t(0));

    h.llm->QueueReply(ReplyJson("Right behind you!", "happy", "neutral", {}, "none", "none", "follow"));
    auto nl = h.svc->OnPlayerMessage(h.Key(), "Elara, follow me.", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(nl.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(nl.command == ActionType::Follow);    // executed immediately via Playerbots by the caller
    CHECK(h.WaitSpeak(h.Key()));
    // model's duplicate "follow" action is not executed a second time
    CHECK(!h.Has(OutgoingAction::Type::Command));
    CHECK_CONTAINS(h.llm->Calls()[0].req.messages[0].content, "They asked you to follow");
}

TEST(Service, PauseRequestsBecomeAStay)
{
    Harness h;
    h.Activate(h.Key());
    // "wait" is not a Playerbots command: it used to be swallowed as a companion pause and do nothing at all
    auto wait = h.svc->OnPlayerMessage(h.Key(), "wait", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(wait.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(wait.command == ActionType::Stay);
    CHECK(h.WaitSpeak(h.Key()));
    h.now += 10;
    auto please = h.svc->OnPlayerMessage(h.Key(), "please hold on", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(please.command == ActionType::Stay);
    CHECK(h.WaitSpeak(h.Key(), 2));
    h.now += 10;
    // "stay" is a Playerbots command word; the companion now handles the pause, but the stay itself must still run
    auto stay = h.svc->OnPlayerMessage(h.Key(), "stay", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(stay.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(stay.command == ActionType::Stay);
    CHECK(h.WaitSpeak(h.Key(), 3));
    h.now += 10;
    // a remark about waiting is not an order
    h.llm->QueueReply(ReplyJson("I said nothing, I was just thinking.", "relaxed", "neutral", {}));
    auto remark = h.svc->OnPlayerMessage(h.Key(), "wait, what did you say?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(remark.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(!remark.command.has_value());
}

TEST(Service, ModelCannotInventActionsOrExceedAllowlist)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->QueueReply(ReplyJson("Hmm, okay.", "relaxed", "neutral", {}, "none", "none", "attack"));
    h.svc->OnPlayerMessage(h.Key(), "I really don't like murlocs, you know?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK(!h.Has(OutgoingAction::Type::Command));   // no attack without a request in the player's words
    h.llm->QueueReply(ReplyJson("*kisses you*", "happy", "positive", {}, "none", "none", "emote_kiss"));
    h.svc->OnPlayerMessage(h.Key(), "You look lovely today.", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK(!h.Has(OutgoingAction::Type::Emote, ActionType::EmoteKiss));   // strangers don't kiss
}

TEST(Service, ProposalDeclinedForStrangersEvenIfModelSaysYes)
{
    Harness h;
    h.Activate(h.Key());
    // The model ignores the directive twice -> server uses its own fallback and commits nothing.
    h.llm->QueueReply(ReplyJson("Yes!! Of course!", "excited", "very_positive", {}, "dating", "accept"));
    h.llm->QueueReply(ReplyJson("Yes, I will be your girlfriend!", "excited", "very_positive", {}, "dating", "accept"));
    h.svc->OnPlayerMessage(h.Key(), "Elara, would you like to be my girlfriend?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_EQ(h.llm->CallCount(), size_t(2));   // one retry with a stronger instruction
    CHECK_CONTAINS(h.llm->Calls()[0].req.messages[0].content, "Gently say no");
    std::string said = h.Spoken(h.Key())[0];
    CHECK_NOT_CONTAINS(said, "girlfriend");
    auto rec = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now);
    CHECK(rec.relationship.stage == Stage::Strangers);
}

TEST(Service, ModelProposalLabelOnUnrelatedMessageIsIgnored)
{
    // Regression: after a declined proposal the model labelled "I also like flowers" as another
    // proposal, the server forced a refusal and replaced the real reply with a fallback line.
    Harness h;
    h.Activate(h.Key());
    h.llm->QueueReply(ReplyJson("Ehehe... that's a bit too soon for us.", "happy", "positive", {}, "dating", "decline"));
    h.svc->OnPlayerMessage(h.Key(), "do you wanna be my girlfriend", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    h.llm->ClearCalls();
    h.llm->QueueReply(ReplyJson("Flowers? Me too! Which ones do you like best?", "happy", "positive", {}, "dating", "accept"));
    h.svc->OnPlayerMessage(h.Key(), "did you know, I also like flowers", ChatChannel::Whisper, h.ctx, h.now + 5);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK_EQ(h.Spoken(h.Key())[1], std::string("Flowers? Me too! Which ones do you like best?"));
    CHECK_EQ(h.llm->CallCount(), size_t(1));   // no forced retry
    CHECK(h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.stage == Stage::Strangers);
}

TEST(Service, FallbackLinesDoNotRepeat)
{
    Harness h;
    h.Activate(h.Key());
    for (int i = 0; i < 3; ++i)
    {
        // the model insists on accepting a proposal the server declines -> fallback after one retry
        h.llm->QueueReply(ReplyJson("Yes!!", "excited", "very_positive", {}, "dating", "accept"));
        h.llm->QueueReply(ReplyJson("Yes!!", "excited", "very_positive", {}, "dating", "accept"));
        h.svc->OnPlayerMessage(h.Key(), "will you be my girlfriend?", ChatChannel::Whisper, h.ctx, h.now + i * 700);
        CHECK(h.WaitSpeak(h.Key(), size_t(i + 1)));
    }
    auto said = h.Spoken(h.Key());
    CHECK(said[0] != said[1]);
    CHECK(said[1] != said[2]);
}

TEST(Service, ProposalAcceptedAndPersistedWhenReady)
{
    Harness h;
    h.Activate(h.Key());
    PersonalityProfile p = FindPreset("anime_girlfriend")->profile;
    p.name = "Elara";
    p.confidence = 100;
    h.svc->SavePersonality(h.Key(), kAccount, "Elara", p, h.now);
    h.svc->SetInitialRelationship(h.Key(), kAccount, "Elara", Stage::RomanticInterest, true, "", h.now);
    h.llm->QueueReply(ReplyJson("I... yes. Yes, I'd love that!", "excited", "very_positive", { "deep_talk" }, "dating", "accept"));
    h.svc->OnPlayerMessage(h.Key(), "Elara, would you like to be my girlfriend?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    auto rec = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now);
    CHECK(rec.relationship.stage == Stage::Dating);
    bool milestone = false;
    for (auto const& m : h.repo.LoadMemories(h.Key()))
        milestone |= m.kind == MemoryKind::Relationship && m.source == MemorySource::Observed && m.text.find("became a couple") != std::string::npos;
    CHECK(milestone);
    CHECK(h.Has(OutgoingAction::Type::Emote, ActionType::EmoteHug));
    bool logged = false;
    for (auto const& e : h.repo.Events())
        logged |= e.type == "stage_change";
    CHECK(logged);
}

TEST(Service, MarriageCeremonyAfterEngagement)
{
    Harness h;
    h.Activate(h.Key());
    PersonalityProfile p = FindPreset("longtime_partner")->profile;
    p.name = "Elara";
    p.confidence = 100;
    h.svc->SavePersonality(h.Key(), kAccount, "Elara", p, h.now);
    h.svc->SetInitialRelationship(h.Key(), kAccount, "Elara", Stage::Engaged, true, "", h.now);
    h.llm->SetResponder([](LlmRequest const& r)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        bool vow = r.messages.back().content.find("wedding vow") != std::string::npos;
        res.content = vow ? ReplyJson("Before the Light and all of Azeroth, I am yours.", "happy")
                          : ReplyJson("Yes. A thousand times yes.", "excited", "very_positive", {}, "marriage", "accept");
        return res;
    });
    h.svc->OnPlayerMessage(h.Key(), "Will you marry me?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK(h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.stage == Stage::Married);
    CHECK_CONTAINS(h.Spoken(h.Key())[1], "I am yours");
}

TEST(Service, GiftAwardedOnceAndRemembered)
{
    Harness h;
    h.Activate(h.Key());
    h.svc->SetInitialRelationship(h.Key(), kAccount, "Elara", Stage::Dating, true, "", h.now);
    GiftItem ring;
    ring.itemGuid = 777001;
    ring.entry = 43250;
    ring.name = "Silver Ring of Devotion";
    ring.quality = 3;
    ring.itemClass = 4;
    ring.inventoryType = 11;
    ring.sellPrice = 15000;
    ring.isEquippable = true;
    ring.usableByCompanion = true;
    ring.itemLevel = 80;
    ring.equippedItemLevel = 70;
    double before = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.v.affection;
    h.llm->QueueReply(ReplyJson("Wait... this is for me? It's beautiful. Thank you! I'll treasure it.", "excited"));
    auto g = h.svc->OnGift(h.Key(), ring, h.ctx, h.now);
    CHECK(g.rewarded);
    CHECK(g.evaluation.isRing);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_CONTAINS(h.llm->Calls()[0].req.messages.back().content, "Silver Ring of Devotion");
    double after = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.v.affection;
    CHECK(after > before);
    // replaying the same item instance is rejected
    auto again = h.svc->OnGift(h.Key(), ring, h.ctx, h.now + 5);
    CHECK(!again.rewarded);
    CHECK_EQ(again.reason, std::string("duplicate"));
    bool remembered = false;
    for (auto const& m : h.repo.LoadMemories(h.Key()))
        remembered |= m.kind == MemoryKind::Gift && m.text.find("Silver Ring of Devotion") != std::string::npos;
    CHECK(remembered);
    // replay protection survives a restart
    h.Restart();
    h.Activate(h.Key());
    CHECK(!h.svc->OnGift(h.Key(), ring, h.ctx, h.now + 100).rewarded);
}

TEST(Service, MemoryPersistsAcrossRestartAndIsRetrieved)
{
    Harness h;
    h.Activate(h.Key());
    Json mems = Json::MakeArray();
    Json m = Json::MakeObject();
    m.Set("text", "David enjoys learning magic.");
    m.Set("kind", "preference");
    m.Set("about", "player");
    m.Set("importance", 60);
    mems.Push(m);
    h.llm->QueueReply(ReplyJson("Magic? Then we'll get along wonderfully.", "happy", "positive", {}, "none", "none", "none", mems));
    h.svc->OnPlayerMessage(h.Key(), "I really enjoy learning magic, you know.", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    auto stored = h.repo.LoadMemories(h.Key());
    CHECK_EQ(stored.size(), size_t(1));
    CHECK(stored[0].source == MemorySource::PlayerStated);   // overlaps with what the player actually said

    h.Restart();                       // worldserver restart
    h.now += 86400;                    // next day
    h.Activate(h.Key());
    h.llm->QueueReply(ReplyJson("Of course I remember, you love magic.", "happy"));
    h.svc->OnPlayerMessage(h.Key(), "Do you remember what I like to learn? Magic!", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    auto calls = h.llm->Calls();
    CHECK_CONTAINS(calls.back().req.messages[0].content, "David enjoys learning magic.");
    // short-term history also restored
    bool hasOldTurn = false;
    for (auto const& msg : calls.back().req.messages)
        hasOldTurn |= msg.content.find("we'll get along wonderfully") != std::string::npos;
    CHECK(hasOldTurn);
}

TEST(Service, InterpretationsAreNotStoredAsPlayerFacts)
{
    Harness h;
    h.Activate(h.Key());
    Json mems = Json::MakeArray();
    Json m = Json::MakeObject();
    m.Set("text", "David secretly owns a dragon in Northrend.");   // never said
    m.Set("kind", "fact");
    m.Set("about", "player");
    m.Set("importance", 65);
    mems.Push(m);
    h.llm->QueueReply(ReplyJson("Haha, sure.", "relaxed", "neutral", {}, "none", "none", "none", mems));
    h.svc->OnPlayerMessage(h.Key(), "What's up?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    auto stored = h.repo.LoadMemories(h.Key());
    CHECK_EQ(stored.size(), size_t(1));
    CHECK(stored[0].source == MemorySource::Interpretation);
}

TEST(Service, MemoryEditingDeletionAndReset)
{
    Harness h;
    h.Activate(h.Key());
    auto added = h.svc->AddUserMemory(h.Key(), MemoryKind::Promise, "David promised to visit Dalaran together.", 70, h.now);
    CHECK(added.has_value());
    auto edited = h.svc->EditMemory(h.Key(), added->id, "David promised to visit Dalaran and Shattrath together.", 75, h.now);
    CHECK(edited.has_value());
    CHECK_CONTAINS(h.repo.LoadMemories(h.Key())[0].text, "Shattrath");
    size_t total = 0;
    auto list = h.svc->ListMemories(h.Key(), MemoryKind::Promise, "dalaran", 0, 10, &total);
    CHECK_EQ(total, size_t(1));
    CHECK(h.svc->DeleteMemory(h.Key(), added->id));
    CHECK(h.repo.LoadMemories(h.Key()).empty());
    CHECK(!h.svc->DeleteMemory(h.Key(), 9999));
    h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, "David hates spiders.", 40, h.now);
    h.svc->SetInitialRelationship(h.Key(), kAccount, "Elara", Stage::Married, true, "", h.now);
    CHECK(h.svc->ResetCompanion(h.Key(), h.now));
    CHECK(h.repo.LoadMemories(h.Key()).empty());
    CHECK(h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.stage == Stage::Strangers);
}

TEST(Service, MemoriesAreScopedPerPlayer)
{
    Harness h;
    h.Activate(h.Key());
    h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, "Arthas told Elara a secret about his past.", 70, h.now);
    size_t total = 0;
    auto foreign = h.svc->ListMemories(h.Key(kOtherPlayer, kElara), std::nullopt, "", 0, 50, &total);
    CHECK_EQ(total, size_t(0));
    CHECK(foreign.empty());
}

TEST(Service, InitialCoupleStageSeedsBackstory)
{
    Harness h;
    auto r = h.svc->SetInitialRelationship(h.Key(), kAccount, "Elara", Stage::Dating, true,
                                           "Elara and I have known each other for years.", h.now);
    CHECK(r.ok);
    auto rec = h.repo.LoadCompanion(h.Key());
    CHECK(rec.has_value());
    CHECK(rec->relationship.stage == Stage::Dating);
    CHECK(rec->relationship.v.affection >= 75);
    auto mems = h.repo.LoadMemories(h.Key());
    CHECK_EQ(mems.size(), size_t(2));
    for (auto const& m : mems)
        CHECK(m.source == MemorySource::Profile);
    h.cfg.maxInitialStage = 3;
    CHECK(!h.svc->SetInitialRelationship(h.Key(), kAccount, "Elara", Stage::Married, true, "", h.now).ok);
}

TEST(Service, ActivationLimitsAndConflicts)
{
    Harness h;
    h.cfg.maxActiveCompanionsPerPlayer = 1;
    h.Activate(h.Key());
    auto second = h.svc->Activate(h.Key(kArthas, kLyra), kAccount, "Lyra", "Arthas", h.now);
    CHECK(!second.ok);
    CHECK_EQ(second.error, std::string("limit_reached"));
    auto conflict = h.svc->Activate(h.Key(kOtherPlayer, kElara), 200, "Elara", "Jaina", h.now);
    CHECK(!conflict.ok);
    CHECK_EQ(conflict.error, std::string("companion_in_use"));
    h.svc->Deactivate(h.Key(), h.now);
    CHECK(h.svc->Activate(h.Key(kArthas, kLyra), kAccount, "Lyra", "Arthas", h.now).ok);
}

TEST(Service, ConcurrentCompanionsAreIsolated)
{
    Harness h;
    h.Activate(h.Key(kArthas, kElara), "Elara");
    h.Activate(h.Key(kOtherPlayer, kLyra), "Lyra");
    h.llm->SetDelayMs(30);
    h.llm->SetResponder([](LlmRequest const& r)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        bool elara = r.messages[0].content.find("You are Elara") != std::string::npos;
        res.content = ReplyJson(elara ? "Elara here." : "Lyra here.");
        return res;
    });
    h.svc->OnPlayerMessage(h.Key(kArthas, kElara), "Hello there friend", ChatChannel::Whisper, h.ctx, h.now);
    h.svc->OnPlayerMessage(h.Key(kOtherPlayer, kLyra), "Hello there friend", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.PumpUntil([&](auto const&) { return h.Spoken(h.Key(kArthas, kElara)).size() == 1 && h.Spoken(h.Key(kOtherPlayer, kLyra)).size() == 1; }));
    CHECK_EQ(h.Spoken(h.Key(kArthas, kElara))[0], std::string("Elara here."));
    CHECK_EQ(h.Spoken(h.Key(kOtherPlayer, kLyra))[0], std::string("Lyra here."));
}

TEST(Service, OllamaOfflineDegradesGracefully)
{
    Harness h;
    h.llm->SetOnline(false);
    h.Activate(h.Key());
    auto r = h.svc->OnPlayerMessage(h.Key(), "Are you there?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(h.PumpUntil([&](auto const&) { return h.Has(OutgoingAction::Type::Notice); }));
    CHECK(h.Spoken(h.Key()).empty());
    CHECK(h.PumpUntil([&](auto const&) { return h.svc->Status().checked; }));
    CHECK(!h.svc->Status().online);
    // recovers when Ollama comes back
    h.llm->SetOnline(true);
    h.svc->RequestHealthCheck();
    CHECK(h.PumpUntil([&](auto const&) { return h.svc->Status().online; }));
    h.svc->OnPlayerMessage(h.Key(), "Are you there now?", ChatChannel::Whisper, h.ctx, h.now + 1);
    CHECK(h.WaitSpeak(h.Key()));
}

TEST(Service, TimeoutRecovery)
{
    Harness h;
    h.Activate(h.Key());
    LlmResult timeout;
    timeout.status = LlmResult::Status::Timeout;
    timeout.error = "request timed out";
    h.llm->QueueResult(timeout);
    h.svc->OnPlayerMessage(h.Key(), "Tell me a story", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.PumpUntil([&](auto const&) { return h.Has(OutgoingAction::Type::Notice); }));
    CHECK(h.svc->Debug(h.Key())->failures == 1);
    h.llm->QueueReply(ReplyJson("Once upon a time in Goldshire..."));
    h.svc->OnPlayerMessage(h.Key(), "Tell me a story please", ChatChannel::Whisper, h.ctx, h.now + 2);
    CHECK(h.WaitSpeak(h.Key()));
}

TEST(Service, WorkerQueueSaturationGivesBackpressure)
{
    Harness h;
    h.cfg.maxConcurrentRequests = 1;
    h.cfg.maxQueuedRequests = 1;
    h.cfg.maxPendingPerCompanion = 5;
    h.cfg.maxActiveCompanionsPerPlayer = 5;
    h.Restart();
    h.llm->SetDelayMs(300);
    h.Activate(h.Key(kArthas, kElara));
    h.Activate(h.Key(kArthas, kLyra), "Lyra");
    h.Activate(h.Key(kArthas, 4), "Sylvana");
    int accepted = 0, busy = 0;
    for (Guid c : { kElara, kLyra, Guid(4) })
    {
        auto r = h.svc->OnPlayerMessage(h.Key(kArthas, c), "Hello, how are you today?", ChatChannel::Whisper, h.ctx, h.now);
        if (r.result == TrueCompanionService::MessageResult::Accepted) ++accepted;
        if (r.result == TrueCompanionService::MessageResult::Busy) ++busy;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(accepted >= 1);
    CHECK(busy >= 1);
    // per-companion pending limit
    Harness h2;
    h2.cfg.maxPendingPerCompanion = 1;
    h2.Restart();
    h2.llm->SetDelayMs(300);
    h2.Activate(h2.Key());
    CHECK(h2.svc->OnPlayerMessage(h2.Key(), "First message here", ChatChannel::Whisper, h2.ctx, h2.now).result == TrueCompanionService::MessageResult::Accepted);
    CHECK(h2.svc->OnPlayerMessage(h2.Key(), "Second message here", ChatChannel::Whisper, h2.ctx, h2.now).result == TrueCompanionService::MessageResult::Busy);
}

TEST(Service, MinimumResponseCooldown)
{
    Harness h;
    h.cfg.minResponseCooldownMs = 400;
    h.cfg.maxPendingPerCompanion = 5;
    h.Activate(h.Key());
    CHECK(h.svc->OnPlayerMessage(h.Key(), "First message here", ChatChannel::Whisper, h.ctx, h.now).result == TrueCompanionService::MessageResult::Accepted);
    CHECK(h.svc->OnPlayerMessage(h.Key(), "Spam spam spam", ChatChannel::Whisper, h.ctx, h.now).result == TrueCompanionService::MessageResult::Busy);
    // commands are never throttled by the conversation cooldown
    CHECK(h.svc->OnPlayerMessage(h.Key(), "follow", ChatChannel::Whisper, h.ctx, h.now).result == TrueCompanionService::MessageResult::PassToPlayerbots);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    CHECK(h.svc->OnPlayerMessage(h.Key(), "Okay, now a real question?", ChatChannel::Whisper, h.ctx, h.now).result == TrueCompanionService::MessageResult::Accepted);
}

TEST(Service, DismissCancelsInFlightRequests)
{
    Harness h;
    h.llm->SetDelayMs(2000);
    h.Activate(h.Key());
    h.svc->OnPlayerMessage(h.Key(), "Say something nice", ChatChannel::Whisper, h.ctx, h.now);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto t0 = std::chrono::steady_clock::now();
    h.svc->Deactivate(h.Key(), h.now);
    h.svc->Shutdown();   // joins workers: must not wait for the 2 s mock delay
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);
    CHECK(h.Spoken(h.Key()).empty());
}

TEST(Service, ProactiveRespectsCombatCooldownsAndBackoff)
{
    Harness h;
    h.cfg.proactiveEnabled = true;
    h.cfg.proactiveChance = 1.0;
    h.cfg.proactiveMinIntervalSec = 60;
    h.cfg.proactiveMaxIntervalSec = 60;
    h.cfg.proactiveMaxPerHour = 3;
    h.Restart();
    h.Activate(h.Key());
    h.llm->SetResponder([](LlmRequest const&)
    {
        LlmResult r;
        r.status = LlmResult::Status::Ok;
        static int n = 0;
        r.content = ReplyJson("It's actually nice just traveling together like this. (" + std::to_string(++n) + ")", "relaxed", "neutral");
        return r;
    });
    // In combat: never
    h.ctx.playerInCombat = true;
    for (int i = 0; i < 20; ++i)
    {
        h.now += 120;
        h.PumpUntil([](auto const&) { return false; }, 10);
    }
    CHECK(h.Spoken(h.Key()).empty());
    // Out of combat: speaks, but unanswered lines back off and stop after two
    h.ctx.playerInCombat = false;
    for (int i = 0; i < 60; ++i)
    {
        h.now += 120;
        h.PumpUntil([](auto const&) { return false; }, 15);
    }
    size_t spoken = h.Spoken(h.Key()).size();
    CHECK(spoken >= 1);
    CHECK(spoken <= 2);
    // disabled entirely
    Harness off;
    off.cfg.proactiveEnabled = false;
    off.Restart();
    off.Activate(off.Key());
    for (int i = 0; i < 30; ++i)
    {
        off.now += 600;
        off.PumpUntil([](auto const&) { return false; }, 5);
    }
    CHECK_EQ(off.llm->CallCount(), size_t(0));
}

TEST(Service, BattleAndLevelUpReactions)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->SetResponder([](LlmRequest const& r)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        std::string last = r.messages.back().content;
        res.content = ReplyJson(last.find("close") != std::string::npos ? "That was way too close. Be careful!"
                                : last.find("level") != std::string::npos ? "Another level! I think I'm finally getting stronger."
                                                                           : "Hm.");
        return res;
    });
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatStart, "", 0, h.now }, h.now);
    h.svc->OnGameEvent(h.Key(), { GameEventType::PlayerNearDeath, "", 12, h.now }, h.now);
    h.ctx.playerInCombat = true;
    h.PumpUntil([](auto const&) { return false; }, 50);
    CHECK(h.Spoken(h.Key()).empty());   // no chatter mid-fight
    h.ctx.playerInCombat = false;
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatEnd, "Murloc Tidehunter", 3, h.now }, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_CONTAINS(h.Spoken(h.Key())[0], "too close");
    h.now += 10;
    h.svc->OnGameEvent(h.Key(), { GameEventType::CompanionLevelUp, "", 71, h.now }, h.now);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK_CONTAINS(h.Spoken(h.Key())[1], "Another level");
}

TEST(Service, RepetitionTriggersOneRegeneration)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->QueueReply(ReplyJson("I'm so glad you're here with me."));
    h.svc->OnPlayerMessage(h.Key(), "Hi there my dear", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    h.llm->QueueReply(ReplyJson("I'm so glad you're here with me."));
    h.llm->QueueReply(ReplyJson("Let's go see the fountains in Dalaran."));
    h.svc->OnPlayerMessage(h.Key(), "What should we do now?", ChatChannel::Whisper, h.ctx, h.now + 5);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK_EQ(h.Spoken(h.Key())[1], std::string("Let's go see the fountains in Dalaran."));
}

TEST(Service, PartyChatOnlyWhenAddressed)
{
    Harness h;
    h.Activate(h.Key());
    auto r = h.svc->OnPlayerMessage(h.Key(), "pull the next pack", ChatChannel::Party, h.ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::NotForUs);
    h.llm->QueueReply(ReplyJson("On it!"));
    auto r2 = h.svc->OnPlayerMessage(h.Key(), "Elara, what do you think of this place?", ChatChannel::Party, h.ctx, h.now);
    CHECK(r2.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(h.WaitSpeak(h.Key()));
    for (auto const& a : h.out)
        if (a.type == OutgoingAction::Type::Speak)
            CHECK(a.channel == ChatChannel::Party);
}

TEST(Service, PartyFallbackAnswersUnaddressedLines)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->QueueReply(ReplyJson("Sounds good to me."));
    // The game layer sets partyTarget when only bots share the group with the player.
    auto r = h.svc->OnPlayerMessage(h.Key(), "nice weather today", ChatChannel::Party, h.ctx, h.now, true);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(h.WaitSpeak(h.Key()));
}

TEST(Service, SeveralCompanionsPerPlayer)
{
    Harness h;
    CHECK_EQ(h.cfg.maxActiveCompanionsPerPlayer, 3u);
    h.Activate(h.Key(kArthas, kElara), "Elara");
    h.Activate(h.Key(kArthas, kLyra), "Lyra");
    CHECK(h.svc->IsAddressed(h.Key(kArthas, kLyra), "Lyra, come here"));
    CHECK(!h.svc->IsAddressed(h.Key(kArthas, kElara), "Lyra, come here"));
    CHECK(h.svc->IsAddressed(h.Key(kArthas, kElara), "hey elara!"));
    h.llm->QueueReply(ReplyJson("Coming!"));
    h.svc->OnPlayerMessage(h.Key(kArthas, kLyra), "Hi Lyra", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key(kArthas, kLyra)));
    CHECK(h.svc->LastActivity(h.Key(kArthas, kLyra)) >= h.svc->LastActivity(h.Key(kArthas, kElara)));
}

TEST(Service, MemoryEditsNotifyTheAddon)
{
    Harness h;
    h.Activate(h.Key());
    auto pushes = [&]()
    {
        size_t n = 0;
        for (auto const& a : h.svc->TakeOutgoing())
            if (a.type == OutgoingAction::Type::UiPush && a.payload["op"].AsString() == "memory_changed")
                ++n;
        return n;
    };
    pushes();
    auto m = h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, "I grew up in Goldshire.", 50, h.now);
    CHECK(m.has_value());
    CHECK_EQ(pushes(), size_t(1));
    h.svc->PinMemory(h.Key(), m->id, true);
    CHECK_EQ(pushes(), size_t(1));
    h.svc->ClearMemories(h.Key(), std::nullopt);
    CHECK_EQ(pushes(), size_t(1));
}

TEST(Service, SavedNameSurvivesReactivation)
{
    Harness h;
    PersonalityProfile p;
    p.name = "Seren";
    CHECK(h.svc->SavePersonality(h.Key(), kAccount, "Taren", p, h.now).ok);
    auto r = h.svc->Activate(h.Key(), kAccount, "Taren", "Arthas", h.now);
    CHECK(r.ok);
    std::string sys;
    h.llm->SetResponder([&](LlmRequest const& req)
    {
        sys = req.messages[0].content;
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        res.content = ReplyJson("Hello there.");
        return res;
    });
    h.svc->OnPlayerMessage(h.Key(), "What is your name?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_CONTAINS(sys, "You are Seren");
    CHECK_CONTAINS(sys, "not what you are called");
}

TEST(Service, CustomPresetsPerAccount)
{
    Harness h;
    PersonalityProfile mine;
    mine.name = "Elara";
    mine.nickname = "Ela";
    mine.background = "A herbalist from Ashenvale.";
    mine.customInstructions = "Talks about plants a lot.";
    mine.outgoing = 80;
    auto r = h.svc->SaveCustomPreset(kAccount, "Herbalist", "Plant lover", mine, "");
    CHECK(r.ok);
    CHECK_EQ(r.id, std::string("custom:1"));
    CHECK(!h.svc->SaveCustomPreset(kAccount, "   ", "", mine, "").ok);

    auto list = h.svc->ListPresets(kAccount);
    CHECK_EQ(list.size(), BuiltInPresets().size() + 1);
    CHECK(list.back().custom);
    CHECK(list.back().profile.name.empty());          // name is not part of a template
    CHECK_EQ(h.svc->ListPresets(999).size(), BuiltInPresets().size()); // other accounts don't see it

    // apply the custom preset to a companion that has a different story
    PersonalityProfile lyra;
    lyra.name = "Lyra";
    lyra.background = "Something else.";
    h.svc->SavePersonality(h.Key(kArthas, kLyra), kAccount, "Lyra", lyra, h.now);
    auto ap = h.svc->ApplyPresetTo(h.Key(kArthas, kLyra), kAccount, "Lyra", "custom:1", h.now);
    CHECK(ap.ok);
    auto rec = h.repo.LoadCompanion(h.Key(kArthas, kLyra));
    CHECK_EQ(rec->personality.name, std::string("Lyra"));
    CHECK_EQ(rec->personality.background, std::string("A herbalist from Ashenvale."));
    CHECK_EQ(rec->personality.outgoing, 80);
    CHECK_EQ(rec->personality.preset, std::string("custom:1"));

    // overwrite, foreign delete, delete
    CHECK_EQ(h.svc->SaveCustomPreset(kAccount, "Herbalist v2", "", mine, "custom:1").id, std::string("custom:1"));
    CHECK(!h.svc->DeleteCustomPreset(999, "custom:1"));
    CHECK(!h.svc->ApplyPresetTo(h.Key(kArthas, kLyra), 999, "Lyra", "custom:1", h.now).ok);
    CHECK(h.svc->DeleteCustomPreset(kAccount, "custom:1"));
    CHECK_EQ(h.svc->ListPresets(kAccount).size(), BuiltInPresets().size());

    h.cfg.maxCustomPresets = 1;
    CHECK(h.svc->SaveCustomPreset(kAccount, "A", "", mine, "").ok);
    CHECK_EQ(h.svc->SaveCustomPreset(kAccount, "B", "", mine, "").error, std::string("preset_limit_reached"));
}

TEST(Service, GameKnowledgeAndCommandOutcomeReachThePrompt)
{
    Harness h;
    h.Activate(h.Key());
    std::string sys;
    h.llm->SetResponder([&](LlmRequest const& req)
    {
        sys = req.messages[0].content;
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        res.content = ReplyJson("Here you go!");
        return res;
    });
    WorldContext ctx = h.ctx;
    ctx.knowledge = { "Gold: 12g 30s", "Bags: 14 of 64 slots free" };
    ctx.commandOutcome = "You opened a trade with them and put 10 gold in it.";
    ctx.commandHandled = true;
    // "attack" would normally trigger the movement command; the handled command wins
    auto r = h.svc->OnPlayerMessage(h.Key(), "give me 10 gold and attack", ChatChannel::Whisper, ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(!r.command.has_value());
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_CONTAINS(sys, "Gold: 12g 30s");
    CHECK_CONTAINS(sys, "What you just did");
    CHECK_CONTAINS(sys, "put 10 gold in it");
}

TEST(Service, SheLeavesAfterTooMuchAbuseAndRefusesToComeBack)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->SetResponder([](LlmRequest const&)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        res.content = ReplyJson("How dare you.", "upset", "very_negative", { "insult", "rudeness" });
        return res;
    });
    bool left = false;
    for (int i = 0; i < 30 && !left; ++i)
    {
        h.now += 30;
        h.svc->OnPlayerMessage(h.Key(), "you are useless and stupid " + std::to_string(i), ChatChannel::Whisper, h.ctx, h.now);
        h.PumpUntil([&](auto const& out)
        {
            for (auto const& a : out)
                if (a.type == OutgoingAction::Type::Command && a.action == ActionType::Leave)
                    return true;
            return h.Spoken(h.Key()).size() >= size_t(i + 1);
        });
        left = h.Has(OutgoingAction::Type::Command, ActionType::Leave);
    }
    CHECK(left);
    CHECK(h.Has(OutgoingAction::Type::Notice));
    auto rec = h.repo.LoadCompanion(h.Key());
    CHECK(rec->relationship.estranged);

    // the game layer dismisses her; calling her back during the cooldown fails
    h.svc->Deactivate(h.Key(), h.now);
    auto r = h.svc->Activate(h.Key(), kAccount, "Elara", "Arthas", h.now + 3600);
    CHECK(!r.ok);
    CHECK_EQ(r.error, std::string("estranged"));
    // after the cooldown she comes back, cold
    TimePoint later = rec->relationship.estrangedUntil + 60;
    CHECK(h.svc->Activate(h.Key(), kAccount, "Elara", "Arthas", later).ok);
    std::string sys;
    h.llm->SetResponder([&](LlmRequest const& req)
    {
        sys = req.messages[0].content;
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        res.content = ReplyJson("...What do you want.", "upset", "neutral");
        return res;
    });
    h.now = later + 120;
    size_t before = h.Spoken(h.Key()).size();
    h.svc->OnPlayerMessage(h.Key(), "I'm sorry for everything.", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key(), before + 1));
    CHECK_CONTAINS(sys, "You left them because of how they treated you");
}

TEST(Service, QueuedResultNeverReachesAReactivatedSession)
{
    Harness h;
    h.Activate(h.Key());
    auto before = h.repo.LoadCompanion(h.Key())->relationship.v;
    h.llm->QueueReply(ReplyJson("OLD SESSION REPLY", "happy", "very_positive", { "compliment", "kindness" }));
    CHECK(h.svc->OnPlayerMessage(h.Key(), "You are wonderful", ChatChannel::Whisper, h.ctx, h.now).result ==
          TrueCompanionService::MessageResult::Accepted);
    // the backend finishes, but the service is not ticked: the result waits in the queue
    for (int i = 0; i < 200 && h.llm->CallCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    h.svc->Deactivate(h.Key(), h.now);
    h.Activate(h.Key());
    h.PumpUntil([](auto const&) { return false; }, 200);

    CHECK(h.Spoken(h.Key()).empty());
    for (auto const& t : h.svc->RecentChat(h.Key(), 50))
        CHECK(t.text.find("OLD SESSION REPLY") == std::string::npos);
    auto after = h.repo.LoadCompanion(h.Key())->relationship.v;
    CHECK_EQ(after.affection, before.affection);
    CHECK_EQ(after.friendship, before.friendship);

    // the new session works normally and is not blocked by a leftover pending slot
    h.llm->QueueReply(ReplyJson("New session reply."));
    CHECK(h.svc->OnPlayerMessage(h.Key(), "Hello again", ChatChannel::Whisper, h.ctx, h.now + 60).result ==
          TrueCompanionService::MessageResult::Accepted);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_EQ(h.Spoken(h.Key()).back(), std::string("New session reply."));
}

TEST(Service, InFlightRequestIsCancelledAcrossDismissal)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->SetDelayMs(150);
    h.llm->QueueReply(ReplyJson("IN FLIGHT REPLY"));
    h.svc->OnPlayerMessage(h.Key(), "Tell me a story", ChatChannel::Whisper, h.ctx, h.now);
    for (int i = 0; i < 200 && h.llm->CallCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    h.svc->Deactivate(h.Key(), h.now);
    h.Activate(h.Key());
    h.llm->SetDelayMs(0);
    h.PumpUntil([](auto const&) { return false; }, 400);
    CHECK(h.Spoken(h.Key()).empty());
    for (auto const& t : h.svc->RecentChat(h.Key(), 50))
        CHECK(t.text.find("IN FLIGHT REPLY") == std::string::npos);
    h.llm->QueueReply(ReplyJson("Fresh reply."));
    h.svc->OnPlayerMessage(h.Key(), "Hi", ChatChannel::Whisper, h.ctx, h.now + 60);
    CHECK(h.WaitSpeak(h.Key()));
}

TEST(Service, SameCommandPathForWhisperPartyAndAddon)
{
    // The game layer asks PrepareControl for every channel; the answer depends on the text and the
    // companion's settings only, never on where the line was typed.
    Harness h;
    h.Activate(h.Key());
    auto give = h.svc->PrepareControl(h.Key(), "Elara, give me 10 gold please", "Elara");
    CHECK(give.active);
    CHECK(give.command && give.command->kind == CompanionCommandKind::GiveGold && give.command->amount == 100000);
    auto ask = h.svc->PrepareControl(h.Key(), "what's in your bags?", "Elara");
    CHECK(ask.active && !ask.command && (ask.knowledgeTopics & knowledge::Inventory));
    // raw Playerbots commands are forwarded by the caller, never executed twice as companion commands
    CHECK(!h.svc->PrepareControl(h.Key(), "!follow", "Elara").command);
    CHECK(!h.svc->PrepareControl(h.Key(), "co +dps", "Elara").command);
    // the character name works as well as the chosen name
    PersonalityProfile p;
    p.name = "Seren";
    h.svc->SavePersonality(h.Key(), kAccount, "Elara", p, h.now);
    CHECK(h.svc->PrepareControl(h.Key(), "Seren, attack the wolf", "Elara").command);
    CHECK(h.svc->PrepareControl(h.Key(), "Elara, attack the wolf", "Elara").command);

    // a command carried out by the game layer suppresses the legacy movement word on every channel
    for (ChatChannel ch : { ChatChannel::Whisper, ChatChannel::Party, ChatChannel::Addon })
    {
        WorldContext ctx = h.ctx;
        ctx.commandOutcome = "You attack the wolf.";
        ctx.commandHandled = true;
        h.now += 30;
        h.llm->QueueReply(ReplyJson("On it!"));
        auto r = h.svc->OnPlayerMessage(h.Key(), "Seren, attack the wolf", ch, ctx, h.now, ch == ChatChannel::Party);
        CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
        CHECK(!r.command.has_value());
        h.PumpUntil([](auto const&) { return false; }, 100);
    }

    // natural commands off: knowledge only; AI conversation off: commands still work
    CompanionSettings st;
    st.naturalCommands = false;
    h.svc->SaveSettings(h.Key(), kAccount, "Elara", st, h.now);
    auto off = h.svc->PrepareControl(h.Key(), "give me 10 gold and show me your bags", "Elara");
    CHECK(off.active && !off.command && off.knowledgeTopics != 0);
    st.aiEnabled = false;
    st.naturalCommands = true;
    h.svc->SaveSettings(h.Key(), kAccount, "Elara", st, h.now);
    auto aiOff = h.svc->PrepareControl(h.Key(), "give me 10 gold", "Elara");
    CHECK(aiOff.active && aiOff.command.has_value());
}

TEST(Service, AiOffMeansCommandsOnlyAndDoesNotCountTowardTheLimit)
{
    Harness h;
    h.cfg.maxActiveCompanionsPerPlayer = 1;
    h.Restart();
    // Lyra has her AI conversation off: she comes along for commands without taking the one AI slot
    CompanionSettings st;
    st.aiEnabled = false;
    h.svc->SaveSettings(h.Key(kArthas, kLyra), kAccount, "Lyra", st, h.now);
    CHECK(h.svc->Activate(h.Key(kArthas, kLyra), kAccount, "Lyra", "Arthas", h.now).ok);
    h.Activate(h.Key());   // Elara, AI on
    CHECK_EQ(h.svc->AiActiveForOwner(kArthas), size_t(1));
    // turning Lyra's AI on now would exceed the limit
    st.aiEnabled = true;
    CHECK_EQ(h.svc->SaveSettings(h.Key(kArthas, kLyra), kAccount, "Lyra", st, h.now).error, std::string("limit_reached"));
    // a command to Lyra: carried out, answered in plain words, no request to the AI
    size_t calls = h.llm->CallCount();
    WorldContext ctx = h.ctx;
    ctx.commandOutcome = "You took the lead; they follow you. Tell them.";
    ctx.commandHandled = true;
    auto r = h.svc->OnPlayerMessage(h.Key(kArthas, kLyra), "lead the way", ChatChannel::Whisper, ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(h.WaitSpeak(h.Key(kArthas, kLyra)));
    CHECK_EQ(h.Spoken(h.Key(kArthas, kLyra)).back(), std::string("I took the lead; you follow me."));
    // plain chat to her still goes to Playerbots, and the AI is never asked
    CHECK(h.svc->OnPlayerMessage(h.Key(kArthas, kLyra), "how are you?", ChatChannel::Whisper, h.ctx, h.now).result ==
          TrueCompanionService::MessageResult::AiDisabled);
    CHECK_EQ(h.llm->CallCount(), calls);
}

TEST(Service, ExportImportRoundTripAtMemoryCapacity)
{
    Harness h;
    h.Activate(h.Key());
    static char const* const adj[] = { "silver", "broken", "ancient", "tiny", "glowing", "cursed", "golden", "wooden", "frozen", "burning" };
    static char const* const noun[] = { "lantern", "compass", "dagger", "amulet", "scroll", "kettle", "flute", "helmet", "feather", "map" };
    static char const* const place[] = { "Goldshire", "Westfall", "Duskwood", "Ironforge", "Darnassus", "Stormwind", "Menethil", "Redridge" };
    // distinct memories up to the configured capacity (similar ones would be merged by de-duplication)
    size_t want = h.cfg.maxMemoriesPerCompanion;
    size_t total = 0;
    auto word = [](size_t n) { std::string w; for (int k = 0; k < 4; ++k) { w += char('a' + n % 26); n /= 26; } return w; };
    for (size_t i = 0; total < want && i < want * 4; ++i)
    {
        std::string text = std::string("Madox kept a ") + adj[i % 10] + " " + noun[(i / 10) % 10] + " from " + place[(i / 100) % 8] +
                           " called \"" + word(i * 7919 + 13) + " " + word(i * 104729 + 7) + "\" \xc3\xa4\xc3\xb6\xc3\xbc";
        h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, text, 40, h.now);
        h.svc->ListMemories(h.Key(), std::nullopt, "", 0, 1, &total);
    }
    CHECK_EQ(total, want);

    // export: every page, wrapped in its reply, fits the addon's receive limits (64 KiB, 400 chunks)
    auto pages = h.svc->ExportPages(h.Key(), kAccount);
    CHECK(pages.size() > 1);
    for (size_t i = 0; i < pages.size(); ++i)
    {
        CHECK(pages[i].size() <= kExportPageBytes);
        Json reply = Json::MakeObject();
        reply.Set("op", "export_data");
        reply.Set("guid", int64_t(kElara));
        reply.Set("part", int64_t(i));
        reply.Set("parts", int64_t(pages.size()));
        reply.Set("data", pages[i]);
        auto chunks = EncodeAddonMessage(reply, uint32_t(i + 1));
        size_t bytes = 0;
        for (auto const& c : chunks)
            bytes += c.size();
        CHECK(chunks.size() <= 400);
        CHECK(bytes <= 64 * 1024);
    }
    auto full = JoinExportPages(pages);
    CHECK(full.has_value());
    CHECK_EQ((*full)["memories"].AsArray().size(), want);

    // import: split into parts whose requests pass the server's real reassembler (16 KiB, 96 chunks)
    auto parts = SplitExportPages(*full, 9000);
    ImportAssembler imp;
    CHECK(imp.Begin(parts.size(), h.now).empty());
    AddonReassembler server;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        Json req = Json::MakeObject();
        req.Set("op", "import_part");
        req.Set("guid", int64_t(kLyra));
        req.Set("index", int64_t(i));
        req.Set("data", parts[i]);
        req.Set("req", int64_t(100 + i));
        CHECK(req.Dump().size() <= kMaxInboundMessageBytes);
        AddonReassembler::Result got;
        for (auto const& chunk : EncodeAddonMessage(req, uint32_t(200 + i)))
            got = server.Feed(chunk, 1000);
        CHECK(got.status == AddonReassembler::Status::Complete);
        CHECK(imp.Add(size_t(got.message.GetInt("index")), got.message.GetString("data"), h.now).empty());
    }
    std::string err;
    auto joined = imp.Finish(err, h.now);
    CHECK(joined.has_value());
    h.svc->TakeOutgoing();
    auto res = h.svc->ImportCompanion(h.Key(kArthas, kLyra), kAccount, "Lyra", *joined, h.now);
    CHECK(res.ok);
    // one memory_changed for the whole import (one per memory flooded the addon with reloads)
    size_t changed = 0;
    for (auto const& a : h.svc->TakeOutgoing())
        if (a.type == OutgoingAction::Type::UiPush && a.payload["op"].AsString() == "memory_changed")
            ++changed;
    CHECK_EQ(changed, size_t(1));
    h.svc->ListMemories(h.Key(kArthas, kLyra), std::nullopt, "", 0, 100000, &total);
    CHECK_EQ(total, want);
    CHECK_CONTAINS(res.notes.front(), std::to_string(want) + " of " + std::to_string(want));
}

TEST(Service, IncompleteImportChangesNothing)
{
    Harness h;
    ImportAssembler imp;
    CHECK(imp.Begin(3, h.now).empty());
    CHECK(imp.Add(0, "{}", h.now).empty());
    CHECK(imp.Add(2, "{}", h.now).empty());
    CHECK_EQ(imp.Add(5, "{}", h.now), std::string("import_bad_part"));
    std::string err;
    CHECK(!imp.Finish(err, h.now).has_value());
    CHECK_EQ(err, std::string("import_incomplete"));
    // timed out between parts
    CHECK(imp.Begin(2, h.now).empty());
    CHECK_EQ(imp.Add(0, "{}", h.now + kImportTimeoutSec + 1), std::string("import_not_started"));
    CHECK_EQ(imp.Begin(kMaxImportParts + 1, h.now), std::string("import_too_large"));
    CHECK_EQ(imp.Begin(0, h.now), std::string("import_too_large"));
    // pages out of order or from another format are rejected when joining
    CHECK(!JoinExportPages({ "{\"format\":\"other\",\"page\":0,\"pages\":1,\"memories\":[]}" }).has_value());
}

TEST(Service, SettingsValidation)
{
    Harness h;
    h.cfg.allowedModels = { "qwen3.5:4b" };
    CompanionSettings s;
    s.model = "evil-model";
    CHECK(!h.svc->SaveSettings(h.Key(), kAccount, "Elara", s, h.now).ok);
    s.model = "qwen3.5:4b";
    s.temperature = 9.0;
    s.proactiveFrequency = 50;
    CHECK(h.svc->SaveSettings(h.Key(), kAccount, "Elara", s, h.now).ok);
    auto rec = h.repo.LoadCompanion(h.Key());
    CHECK(rec.has_value());
    CHECK(rec->settings.temperature <= h.cfg.maxTemperature);
}

TEST(Service, PlayerProfileCharacterOverridesAccountDefault)
{
    Harness h;
    PlayerProfile acc;
    acc.preferredName = "David";
    h.svc->SavePlayerProfile(kAccount, kArthas, acc, true);
    bool isDefault = false;
    CHECK_EQ(h.svc->GetPlayerProfile(kAccount, kArthas, &isDefault).preferredName, std::string("David"));
    CHECK(isDefault);
    PlayerProfile ch;
    ch.preferredName = "Dave the Paladin";
    h.svc->SavePlayerProfile(kAccount, kArthas, ch, false);
    CHECK_EQ(h.svc->GetPlayerProfile(kAccount, kArthas, &isDefault).preferredName, std::string("Dave the Paladin"));
    CHECK(!isDefault);
}

TEST(Service, ExportImportRoundTrip)
{
    Harness h;
    h.Activate(h.Key());
    h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, "David collects rare equipment.", 60, h.now);
    Json exp = h.svc->ExportCompanion(h.Key(), kAccount);
    CHECK_EQ(exp.GetString("format"), std::string("mod-true-companion-export"));
    CHECK_EQ(exp["memories"].Size(), size_t(1));
    auto res = h.svc->ImportCompanion(h.Key(kArthas, kLyra), kAccount, "Lyra", exp, h.now);
    CHECK(res.ok);
    CHECK_EQ(h.repo.LoadMemories(h.Key(kArthas, kLyra)).size(), size_t(1));
    Json bad = Json::MakeObject();
    bad.Set("format", "something-else");
    CHECK(!h.svc->ImportCompanion(h.Key(kArthas, kLyra), kAccount, "Lyra", bad, h.now).ok);
}

TEST(Service, SummaryCreatesSummaryMemory)
{
    Harness h;
    h.cfg.summarizeEveryTurns = 4;
    h.Restart();
    h.Activate(h.Key());
    h.llm->SetResponder([](LlmRequest const& r)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        if (r.messages[0].content.find("long-term memory") != std::string::npos)
            res.content = "{\"summary\":\"Elara and Arthas talked about Dalaran and his love of rare swords.\",\"facts\":[{\"text\":\"Arthas loves collecting rare swords.\",\"kind\":\"preference\",\"about\":\"player\",\"importance\":55}]}";
        else
            res.content = ReplyJson("Mm, tell me more.");
        return res;
    });
    h.svc->OnPlayerMessage(h.Key(), "I love collecting rare swords.", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    h.svc->OnPlayerMessage(h.Key(), "Dalaran has the best rare swords.", ChatChannel::Whisper, h.ctx, h.now + 5);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK(h.PumpUntil([&](auto const&)
    {
        for (auto const& m : h.repo.LoadMemories(h.Key()))
            if (m.kind == MemoryKind::Summary)
                return true;
        return false;
    }));
    bool fact = false;
    for (auto const& m : h.repo.LoadMemories(h.Key()))
        fact |= m.text == "Arthas loves collecting rare swords." && m.source == MemorySource::PlayerStated;
    CHECK(fact);
}

// The primary acceptance example (spec section 14), driven through the service.
TEST(Acceptance, ElaraCoupleRingRestartRemember)
{
    Harness h;
    PersonalityProfile p = FindPreset("anime_girlfriend")->profile;
    p.name = "Elara";
    p.age = 24;
    p.outgoing = 25;
    p.background = "Elara and Arthas have known each other for years.";
    h.svc->SavePersonality(h.Key(), kAccount, "Elara", p, h.now);
    h.svc->SetInitialRelationship(h.Key(), kAccount, "Elara", Stage::Dating, true, "Elara and I have known each other for years.", h.now);
    CompanionSettings st;
    st.proactiveEnabled = true;
    st.proactiveFrequency = 50;
    st.model = "qwen3.5:4b";
    CHECK(h.svc->SaveSettings(h.Key(), kAccount, "Elara", st, h.now).ok);

    h.Activate(h.Key());
    h.svc->OnGameEvent(h.Key(), { GameEventType::Summoned, "Stormwind City", 0, h.now }, h.now);
    h.llm->SetResponder([](LlmRequest const& r)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        std::string last = r.messages.back().content;
        std::string sys = r.messages[0].content;
        if (last.find("ring") != std::string::npos && last.find("<<GAME_EVENT>>") == std::string::npos)
            res.content = ReplyJson(sys.find("Silver Ring of Devotion") != std::string::npos
                                        ? "Of course I remember! The Silver Ring of Devotion you gave me. I never take it off."
                                        : "A ring? What ring?");
        else if (last.find("handed you") != std::string::npos)
            res.content = ReplyJson("Wait... this is for me? It's beautiful. Thank you! I'll treasure it.", "excited");
        else if (last.find("won a fight") != std::string::npos)
            res.content = ReplyJson("Ehehe, we make a good team, don't we?", "happy");
        else if (last.find("joined") != std::string::npos)
            res.content = ReplyJson("There you are! I missed you~", "happy");
        else
            res.content = ReplyJson("Always! Lead the way~", "excited", "positive", { "kindness" });
        return res;
    });
    CHECK(h.WaitSpeak(h.Key()));                  // greeting on summon
    h.svc->OnPlayerMessage(h.Key(), "Hey Elara, ready for an adventure?", ChatChannel::Whisper, h.ctx, h.now + 5);
    CHECK(h.WaitSpeak(h.Key(), 2));
    h.now += 120;   // past the event-reaction cooldown
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatStart, "", 0, h.now }, h.now);
    h.cfg.eventReactionChance = 1.0;
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatEnd, "Defias Thug", 3, h.now }, h.now);   // 3+: worth a remark
    CHECK(h.WaitSpeak(h.Key(), 3));
    h.now += 60;
    GiftItem ring;
    ring.itemGuid = 99123;
    ring.entry = 43250;
    ring.name = "Silver Ring of Devotion";
    ring.quality = 3;
    ring.itemClass = 4;
    ring.inventoryType = 11;
    ring.sellPrice = 15000;
    ring.isEquippable = true;
    ring.usableByCompanion = true;
    ring.itemLevel = 80;
    ring.equippedItemLevel = 72;
    CHECK(h.svc->OnGift(h.Key(), ring, h.ctx, h.now).rewarded);
    CHECK(h.WaitSpeak(h.Key(), 4));
    CHECK_CONTAINS(h.Spoken(h.Key())[3], "treasure it");
    Stage stageBefore = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now).relationship.stage;

    // log out, restart the server, next day
    h.svc->Deactivate(h.Key(), h.now);
    h.Restart();
    h.now += 86400;
    h.out.clear();
    h.Activate(h.Key());
    auto rec = h.svc->GetOrCreateRecord(h.Key(), kAccount, "Elara", h.now);
    CHECK(rec.relationship.stage == stageBefore);
    CHECK_EQ(rec.personality.age, 24);
    CHECK(std::find(rec.personality.styles.begin(), rec.personality.styles.end(), "anime") != rec.personality.styles.end());
    h.svc->OnPlayerMessage(h.Key(), "Do you still have the ring I gave you yesterday?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_CONTAINS(h.Spoken(h.Key())[0], "Silver Ring of Devotion");

    // the memory viewer shows it and it can be deleted
    size_t total = 0;
    auto gifts = h.svc->ListMemories(h.Key(), MemoryKind::Gift, "ring", 0, 10, &total);
    CHECK_EQ(total, size_t(1));
    CHECK(h.svc->DeleteMemory(h.Key(), gifts[0].id));
    total = 0;
    h.svc->ListMemories(h.Key(), MemoryKind::Gift, "ring", 0, 10, &total);
    CHECK_EQ(total, size_t(0));
}

TEST(Service, WithoutOllamaCommandsStillWorkAndSheSaysWhatSheDid)
{
    Harness h;
    h.cfg.ollamaEnabled = false;
    h.Restart();
    h.Activate(h.Key());
    int calls = 0;
    h.llm->SetResponder([&](LlmRequest const&) { ++calls; LlmResult r; r.status = LlmResult::Status::Ok; r.content = ReplyJson("hi"); return r; });
    WorldContext ctx = h.ctx;
    ctx.commandOutcome = "Nothing. You have no junk to sell; tell them.";
    ctx.commandHandled = true;
    auto r = h.svc->OnPlayerMessage(h.Key(), "sell your junk", ChatChannel::Whisper, ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);   // not passed on to Playerbots as text
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_EQ(h.Spoken(h.Key()).back(), std::string("I have no junk to sell."));
    // what happens later (a turn-in on an errand) is said too
    h.svc->OnGameEvent(h.Key(), { GameEventType::CompanionNotice, "You turned in \"A\" at Bob and are coming back to them. Tell them briefly.", 2, h.now }, h.now);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK_EQ(h.Spoken(h.Key()).back(), std::string("I turned in \"A\" at Bob and am coming back to you."));
    // plain chat needs the AI: left to Playerbots as before
    auto chat = h.svc->OnPlayerMessage(h.Key(), "how are you today?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(chat.result == TrueCompanionService::MessageResult::AiDisabled);
    CHECK_EQ(calls, 0);
}

TEST(Service, WhenOllamaFailsTheCommandOutcomeStillReachesThePlayer)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->SetOnline(false);   // Ollama is down (requests and health checks fail)
    WorldContext ctx = h.ctx;
    ctx.commandOutcome = "You took the lead; they follow you. Tell them where you are going.";
    ctx.commandHandled = true;
    auto r = h.svc->OnPlayerMessage(h.Key(), "lead the way", ChatChannel::Whisper, ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_EQ(h.Spoken(h.Key()).back(), std::string("I took the lead; you follow me."));
    // now known offline: the next command is answered right away, without a request
    size_t calls = h.llm->CallCount();
    h.now += 5;
    ctx.commandOutcome = "Nothing. There is no enemy nearby to attack; tell them.";
    h.svc->OnPlayerMessage(h.Key(), "attack", ChatChannel::Whisper, ctx, h.now);
    CHECK(h.WaitSpeak(h.Key(), 2));
    CHECK_EQ(h.Spoken(h.Key()).back(), std::string("There is no enemy nearby to attack."));
    CHECK_EQ(h.llm->CallCount(), calls);
}

TEST(Service, WithoutTheAiSheStillThanksForAGift)
{
    Harness h;
    h.cfg.ollamaEnabled = false;
    h.Restart();
    h.Activate(h.Key());
    GiftItem item;
    item.itemGuid = 777;
    item.entry = 2589;
    item.count = 5;
    item.name = "Linen Cloth";
    auto g = h.svc->OnGift(h.Key(), item, h.ctx, h.now);
    CHECK(g.rewarded);
    CHECK(h.WaitSpeak(h.Key()));
    std::string said = h.Spoken(h.Key()).back();
    CHECK_CONTAINS(said, "5 Linen Cloth");
    CHECK(said.find("Thank") != std::string::npos || said.find("Thanks") != std::string::npos);
    CHECK(said.find("Thank me") == std::string::npos);   // said as it is, no person swapping
}

TEST(Service, ResetDuringASummaryDoesNotStopLaterSummaries)
{
    Harness h;
    h.cfg.summarizeEveryTurns = 4;
    h.cfg.allowFullReset = true;
    h.Restart();
    h.Activate(h.Key());
    std::atomic<bool> release{ false };
    std::atomic<int> summaries{ 0 };
    h.llm->SetResponder([&](LlmRequest const& r)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        if (r.messages[0].content.find("long-term memory") != std::string::npos)
        {
            int n = ++summaries;
            if (n == 1)
                while (!release)   // the first summary hangs until after the reset
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            res.content = std::string("{\"summary\":\"") + (n == 1 ? "OLD summary from before the reset." : "NEW summary after the reset.") +
                          "\",\"facts\":[]}";
        }
        else
            res.content = ReplyJson("Mm, tell me more.");
        return res;
    });
    auto talk = [&](int i)
    {
        h.now += 5;
        h.svc->OnPlayerMessage(h.Key(), "Tell me about Dalaran, part " + std::to_string(i), ChatChannel::Whisper, h.ctx, h.now);
        size_t before = h.Spoken(h.Key()).size();
        CHECK(h.WaitSpeak(h.Key(), before + 1));
    };
    talk(1);
    talk(2);
    CHECK(h.PumpUntil([&](auto const&) { return summaries >= 1; }));   // the old summary is running (blocked)
    CHECK(h.svc->ResetCompanion(h.Key(), h.now));
    release = true;
    h.PumpUntil([](auto const&) { return false; }, 100);               // its late result arrives and is dropped
    talk(3);
    talk(4);
    CHECK(h.PumpUntil([&](auto const&)
    {
        for (auto const& m : h.repo.LoadMemories(h.Key()))
            if (m.kind == MemoryKind::Summary && m.text.find("NEW") != std::string::npos)
                return true;
        return false;
    }));
    for (auto const& m : h.repo.LoadMemories(h.Key()))
        CHECK(m.text.find("OLD") == std::string::npos);
}

// ============================================================ edge cases found in review

namespace
{
    std::vector<Json> TypingPushes(std::vector<OutgoingAction> const& out, size_t from = 0)
    {
        std::vector<Json> t;
        for (size_t i = from; i < out.size(); ++i)
            if (out[i].type == OutgoingAction::Type::UiPush && out[i].payload.GetString("op") == "typing")
                t.push_back(out[i].payload);
        return t;
    }

    class ThrowingLlm : public ILlmBackend
    {
    public:
        LlmResult Chat(LlmRequest const&, CancelToken const&) override { throw std::runtime_error("boom"); }
        std::optional<std::vector<std::string>> ListModels(uint32_t) override { throw std::runtime_error("boom"); }
        std::optional<std::string> Version(uint32_t) override { throw std::runtime_error("boom"); }
    };
}

TEST(Service, ThrowingBackendNeverLeavesTheCompanionBusy)
{
    TrueCompanionConfig cfg;
    cfg.minResponseCooldownMs = 0;
    cfg.proactiveEnabled = false;
    InMemoryRepository repo;
    TrueCompanionService svc(cfg, repo, std::make_shared<ThrowingLlm>(), 7);
    WorldContext ctx;
    ctx.companionInWorld = true;
    TimePoint now = 1'800'000'000;
    CompanionKey key{ kArthas, kElara };
    CHECK(svc.Activate(key, kAccount, "Elara", "Arthas", now).ok);
    auto waitError = [&]()
    {
        for (int i = 0; i < 400; ++i)
        {
            svc.Tick(now, [&](CompanionKey const&) { return std::optional<WorldContext>(ctx); });
            for (auto const& a : svc.TakeOutgoing())
                if (a.type == OutgoingAction::Type::UiPush && a.payload.GetString("op") == "error")
                    return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    // more messages than MaxPendingPerCompanion: each one must come back as an error, not hang
    for (int i = 0; i < 4; ++i)
    {
        auto r = svc.OnPlayerMessage(key, "Message number " + std::to_string(i) + ", are you there?", ChatChannel::Whisper, ctx, now + i);
        CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
        CHECK(waitError());
    }
    // the health check survives a throwing backend too and reports "offline"
    for (int i = 0; i < 400 && !svc.Status().checked; ++i)
    {
        svc.Tick(now, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(svc.Status().checked);
    CHECK(!svc.Status().online);
}

TEST(Service, PlayerMemoriesRespectTheLimit)
{
    Harness h;
    h.cfg.maxMemoriesPerCompanion = 4;
    h.Restart();
    h.Activate(h.Key());
    // two observed episodes the server may prune to make room
    h.svc->OnGameEvent(h.Key(), { GameEventType::QuestComplete, "Kobold Camp Cleanup", 0, h.now }, h.now);
    h.svc->OnGameEvent(h.Key(), { GameEventType::QuestComplete, "Wolves Across the Border", 0, h.now }, h.now);
    CHECK_EQ(h.repo.LoadMemories(h.Key()).size(), size_t(2));
    char const* texts[] = { "Arthas grew up on a farm near Goldshire.", "Arthas is afraid of deep water.",
                            "Arthas plays the lute badly but happily.", "Arthas once tamed a wild gryphon.",
                            "Arthas hates the taste of murloc stew." };
    for (int i = 0; i < 4; ++i)
        CHECK(h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, texts[i], 60, h.now).has_value());
    CHECK_EQ(h.repo.LoadMemories(h.Key()).size(), size_t(4));   // the episodes made room
    std::string error;
    CHECK(!h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, texts[4], 60, h.now, true, &error).has_value());
    CHECK_EQ(error, std::string("memory_limit_reached"));
    CHECK_EQ(h.repo.LoadMemories(h.Key()).size(), size_t(4));
    // the same limit applies while the companion is not summoned
    h.svc->Deactivate(h.Key(), h.now);
    CHECK(!h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, texts[4], 60, h.now, true, &error).has_value());
    CHECK_EQ(h.repo.LoadMemories(h.Key()).size(), size_t(4));
    // a near-duplicate still merges instead of being refused
    CHECK(h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, "Arthas is afraid of deep water!", 70, h.now).has_value());
    error.clear();
    CHECK(!h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, "x", 60, h.now, true, &error).has_value());
    CHECK_EQ(error, std::string("invalid_text"));
}

TEST(Service, TypingIndicatorFollowsPendingRequests)
{
    Harness h;
    h.Activate(h.Key());
    int n = 0;
    std::mutex m;
    h.llm->SetResponder([&](LlmRequest const&)
    {
        int mine;
        {
            std::lock_guard<std::mutex> g(m);
            mine = ++n;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(mine == 1 ? 50 : 400));
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        res.content = ReplyJson(mine == 1 ? "First thought." : "And a completely different second one.");
        return res;
    });
    h.svc->OnPlayerMessage(h.Key(), "Tell me about Dalaran.", ChatChannel::Whisper, h.ctx, h.now);
    h.svc->OnPlayerMessage(h.Key(), "And about Stormwind?", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    size_t first = h.out.size();
    CHECK(h.WaitSpeak(h.Key(), 2));
    // after the first answer the second request is still running: the indicator stays on
    bool onAfterFirst = false;
    size_t speakIdx = 0;
    for (size_t i = 0; i < first; ++i)
        if (h.out[i].type == OutgoingAction::Type::Speak)
            speakIdx = i;
    for (size_t i = speakIdx; i < first; ++i)
        if (h.out[i].type == OutgoingAction::Type::UiPush && h.out[i].payload.GetString("op") == "typing")
            onAfterFirst = h.out[i].payload.GetBool("on");
    CHECK(onAfterFirst);
    auto all = TypingPushes(h.out);
    CHECK(!all.empty());
    CHECK(!all.back().GetBool("on"));
}

TEST(Service, ChatTabCommandsClearTheTypingIndicator)
{
    Harness h;
    h.Activate(h.Key());
    h.svc->TakeOutgoing();
    auto r = h.svc->OnPlayerMessage(h.Key(), "follow", ChatChannel::Addon, h.ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::PassToPlayerbots);
    auto pushes = TypingPushes(h.svc->TakeOutgoing());
    CHECK_EQ(pushes.size(), size_t(1));
    CHECK(!pushes[0].GetBool("on"));
    // whispers never produce addon traffic for commands
    h.svc->OnPlayerMessage(h.Key(), "follow", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(TypingPushes(h.svc->TakeOutgoing()).empty());
    // AI switched off: also no reply coming
    CompanionSettings off;
    off.aiEnabled = false;
    h.svc->SaveSettings(h.Key(), kAccount, "Elara", off, h.now);
    h.svc->TakeOutgoing();
    CHECK(h.svc->OnPlayerMessage(h.Key(), "Hello there, Elara!", ChatChannel::Addon, h.ctx, h.now).result ==
          TrueCompanionService::MessageResult::AiDisabled);
    pushes = TypingPushes(h.svc->TakeOutgoing());
    CHECK_EQ(pushes.size(), size_t(1));
    CHECK(!pushes[0].GetBool("on"));
}

TEST(Service, QuestWithoutTitleIsNotRememberedAsEmptyQuotes)
{
    Harness h;
    h.Activate(h.Key());
    h.svc->OnGameEvent(h.Key(), { GameEventType::QuestComplete, "", 0, h.now }, h.now);
    for (auto const& m : h.repo.LoadMemories(h.Key()))
        CHECK_NOT_CONTAINS(m.text, "\"\"");
}

TEST(Service, SwitchingTheAiOnRespectsTheServerLimit)
{
    Harness h;
    h.cfg.maxAiCompanionsServer = 1;
    h.Restart();
    h.Activate(h.Key());   // Arthas's Elara takes the one AI slot of the server
    // another player's companion, with her AI off: she may come along for commands
    CompanionSettings st;
    st.aiEnabled = false;
    CHECK(h.svc->SaveSettings(h.Key(kOtherPlayer, kLyra), 200, "Lyra", st, h.now).ok);
    CHECK(h.svc->Activate(h.Key(kOtherPlayer, kLyra), 200, "Lyra", "Jaina", h.now).ok);
    // switching her AI on would make two: refused, and nothing changes
    st.aiEnabled = true;
    CHECK_EQ(h.svc->SaveSettings(h.Key(kOtherPlayer, kLyra), 200, "Lyra", st, h.now).error, std::string("server_limit_reached"));
    CHECK(!h.svc->ActiveSettings(h.Key(kOtherPlayer, kLyra))->aiEnabled);
    // commands still work for her
    WorldContext ctx = h.ctx;
    ctx.commandOutcome = "You took the lead; they follow you. Tell them.";
    ctx.commandHandled = true;
    CHECK(h.svc->OnPlayerMessage(h.Key(kOtherPlayer, kLyra), "lead the way", ChatChannel::Whisper, ctx, h.now).result ==
          TrueCompanionService::MessageResult::Accepted);
    // Elara's AI off frees the slot
    CompanionSettings off;
    off.aiEnabled = false;
    CHECK(h.svc->SaveSettings(h.Key(), kAccount, "Elara", off, h.now).ok);
    CHECK(h.svc->SaveSettings(h.Key(kOtherPlayer, kLyra), 200, "Lyra", st, h.now).ok);
}

TEST(Service, SwitchingTheAiOffDropsRepliesOnTheirWay)
{
    Harness h;
    h.Activate(h.Key());
    h.llm->SetDelayMs(300);
    h.llm->SetResponder([](LlmRequest const&)
    {
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        res.content = ReplyJson("A reply from the model.");
        return res;
    });
    h.svc->OnPlayerMessage(h.Key(), "Tell me a story", ChatChannel::Whisper, h.ctx, h.now);
    // and a command whose outcome she would put in her own words
    WorldContext ctx = h.ctx;
    ctx.commandOutcome = "You took the lead; they follow you. Tell them.";
    ctx.commandHandled = true;
    h.svc->OnPlayerMessage(h.Key(), "lead the way", ChatChannel::Whisper, ctx, h.now);
    CompanionSettings off;
    off.aiEnabled = false;
    CHECK(h.svc->SaveSettings(h.Key(), kAccount, "Elara", off, h.now).ok);
    h.PumpUntil([](auto const&) { return false; }, 800);
    for (auto const& line : h.Spoken(h.Key()))
        CHECK(line.find("from the model") == std::string::npos);
    // the command's outcome still reaches the player, in plain words
    auto spoken = h.Spoken(h.Key());
    CHECK(std::find(spoken.begin(), spoken.end(), std::string("I took the lead; you follow me.")) != spoken.end());
    // the typing indicator is off at the end
    bool typing = true;
    for (auto const& a : h.out)
        if (a.type == OutgoingAction::Type::UiPush && a.payload["op"].AsString() == "typing")
            typing = a.payload["on"].AsBool();
    CHECK(!typing);
    // switched on again: she answers normally
    h.llm->SetDelayMs(0);
    CompanionSettings on;
    CHECK(h.svc->SaveSettings(h.Key(), kAccount, "Elara", on, h.now).ok);
    h.svc->OnPlayerMessage(h.Key(), "Tell me another one", ChatChannel::Whisper, h.ctx, h.now + 5);
    CHECK(h.PumpUntil([&](auto const&)
    {
        for (auto const& line : h.Spoken(h.Key()))
            if (line.find("from the model") != std::string::npos)
                return true;
        return false;
    }));
}

TEST(Service, QuestieLinesAndBareAcknowledgmentsGetNoReply)
{
    Harness h;
    h.Activate(h.Key());
    size_t calls = h.llm->CallCount();
    // the player's Questie progress in party chat: not words to her, even as the only companion in the group
    auto r = h.svc->OnPlayerMessage(h.Key(), "{rt1} Questie: 8/8 Lynx Collar for [Unfortunate Measures]!", ChatChannel::Party, h.ctx,
                                    h.now, true);
    CHECK(r.result == TrueCompanionService::MessageResult::NotForUs);
    // "indeed" needs no answer; a whisper is still taken (kept from Playerbots)
    r = h.svc->OnPlayerMessage(h.Key(), "Indeed", ChatChannel::Whisper, h.ctx, h.now);
    CHECK(r.result == TrueCompanionService::MessageResult::Accepted);
    r = h.svc->OnPlayerMessage(h.Key(), "ok", ChatChannel::Party, h.ctx, h.now, true);
    CHECK(r.result == TrueCompanionService::MessageResult::NotForUs);
    h.PumpUntil([](auto const&) { return false; }, 100);
    CHECK_EQ(h.llm->CallCount(), calls);
    CHECK(h.Spoken(h.Key()).empty());
}

TEST(Service, EverydayFightsGetNoRemark)
{
    Harness h;
    h.cfg.eventReactionChance = 1.0;
    h.cfg.eventReactionCooldownSec = 0;
    h.Restart();
    h.Activate(h.Key());
    h.PumpUntil([](auto const&) { return false; }, 50);
    size_t calls = h.llm->CallCount();
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatStart, "", 0, h.now }, h.now);
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatEnd, "Springpaw Lynx", 1, h.now }, h.now);
    h.PumpUntil([](auto const&) { return false; }, 100);
    CHECK_EQ(h.llm->CallCount(), calls);
    // a fight against a group is still worth a word
    h.llm->QueueReply(ReplyJson("That was a whole pack!"));
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatStart, "", 0, h.now }, h.now);
    h.svc->OnGameEvent(h.Key(), { GameEventType::CombatEnd, "Springpaw Lynx", 4, h.now }, h.now);
    CHECK(h.WaitSpeak(h.Key()));
}

TEST(Service, JournalReachesEveryPrompt)
{
    Harness h;
    h.Activate(h.Key());
    std::string sys;
    h.llm->SetResponder([&](LlmRequest const& req)
    {
        sys = req.messages[0].content;
        LlmResult res;
        res.status = LlmResult::Status::Ok;
        res.content = ReplyJson("Two collars so far.");
        return res;
    });
    WorldContext ctx = h.ctx;
    ctx.journal = { "Your quest \"Unfortunate Measures\": you have Lynx Collar 2/8 (Arthas's own count, not yours: Lynx Collar 8/8)." };
    // no quest word in the message: she still has her own numbers
    h.svc->OnPlayerMessage(h.Key(), "how many collars do you need?", ChatChannel::Whisper, ctx, h.now);
    CHECK(h.WaitSpeak(h.Key()));
    CHECK_CONTAINS(sys, "Your journal");
    CHECK_CONTAINS(sys, "Lynx Collar 2/8");
    CHECK_CONTAINS(sys, "their own progress, not yours");
}

TEST(Service, ImportWithRejectedSettingsChangesNothing)
{
    Harness h;
    h.cfg.allowedModels = { "qwen3.5:4b" };
    h.Restart();
    h.Activate(h.Key());
    h.svc->AddUserMemory(h.Key(), MemoryKind::Fact, "Arthas collects rare equipment.", 60, h.now);
    Json exp = h.svc->ExportCompanion(h.Key(), kAccount);
    Json settings = exp["settings"];
    settings.Set("model", "evil-model");
    exp.Set("settings", settings);
    Json personality = exp["personality"];
    personality.Set("name", "Imported Name");
    exp.Set("personality", personality);
    auto res = h.svc->ImportCompanion(h.Key(kArthas, kLyra), kAccount, "Lyra", exp, h.now);
    CHECK(!res.ok);
    CHECK_CONTAINS(res.error, "import_settings_rejected:model_not_allowed");
    CHECK(h.repo.LoadMemories(h.Key(kArthas, kLyra)).empty());
    auto rec = h.repo.LoadCompanion(h.Key(kArthas, kLyra));
    CHECK(!rec || rec->personality.name != "Imported Name");
    // a malformed section is rejected as a whole too
    Json bad = h.svc->ExportCompanion(h.Key(), kAccount);
    bad.Set("memories", "not a list");
    CHECK_EQ(h.svc->ImportCompanion(h.Key(kArthas, kLyra), kAccount, "Lyra", bad, h.now).error, std::string("import_malformed"));
}

TEST(Service, ImportEndAfterTheTimeoutIsNotApplied)
{
    Harness h;
    ImportAssembler imp;
    CHECK(imp.Begin(1, h.now).empty());
    CHECK(imp.Add(0, "{}", h.now).empty());
    std::string err;
    CHECK(!imp.Finish(err, h.now + kImportTimeoutSec + 1).has_value());
    CHECK_EQ(err, std::string("import_expired"));
    CHECK(!imp.Finish(err, h.now + kImportTimeoutSec + 2).has_value());
    CHECK_EQ(err, std::string("import_not_started"));
}

TEST(Service, MovementOrdersWorkWithTheAiOff)
{
    Harness h;
    h.Activate(h.Key());
    CompanionSettings off;
    off.aiEnabled = false;
    h.svc->SaveSettings(h.Key(), kAccount, "Elara", off, h.now);
    h.svc->TakeOutgoing();
    // "wait" must still stop her: with the AI off the line is consumed here, so Playerbots never sees it
    auto wait = h.svc->OnPlayerMessage(h.Key(), "wait", ChatChannel::Addon, h.ctx, h.now);
    CHECK(wait.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(wait.command == ActionType::Stay);
    auto follow = h.svc->OnPlayerMessage(h.Key(), "follow me", ChatChannel::Addon, h.ctx, h.now);
    CHECK(follow.result == TrueCompanionService::MessageResult::Accepted);
    CHECK(follow.command == ActionType::Follow);
    // a chat line is still refused, as it must be with no AI
    CHECK(h.svc->OnPlayerMessage(h.Key(), "how was your day?", ChatChannel::Addon, h.ctx, h.now).result ==
          TrueCompanionService::MessageResult::AiDisabled);
}
