/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 * Natural-language companion commands and knowledge topics.
 */

#include "TestFramework.h"
#include "RomanceCommands.h"

using namespace romance;

namespace
{
    CompanionCommandKind Kind(std::string const& text, std::string const& name = "Elara")
    {
        auto c = ParseCompanionCommand(text, name);
        return c ? c->kind : CompanionCommandKind::None;
    }
    std::string Arg(std::string const& text)
    {
        auto c = ParseCompanionCommand(text, "Elara");
        return c ? c->arg : "<none>";
    }
}

TEST(Commands, Money)
{
    CHECK_EQ(ParseMoney("10g"), uint64_t(100000));
    CHECK_EQ(ParseMoney("give me 5 gold and 20 silver"), uint64_t(52000));
    CHECK_EQ(ParseMoney("1g50s3c"), uint64_t(15003));
    CHECK_EQ(ParseMoney("level 10 quest"), uint64_t(0));
    CHECK_EQ(ParseMoney("i have 99999999g"), uint64_t(0));
}

TEST(Commands, GiveGoldAndItems)
{
    auto c = ParseCompanionCommand("Elara, can you give me 10 gold please?", "Elara");
    CHECK(c && c->kind == CompanionCommandKind::GiveGold && c->amount == 100000);
    CHECK(Kind("I need 2g") == CompanionCommandKind::GiveGold);
    CHECK(Kind("i need healing") == CompanionCommandKind::None);
    CHECK(Kind("give me some gold") == CompanionCommandKind::None);
    c = ParseCompanionCommand("give me 5 linen cloth", "Elara");
    CHECK(c && c->kind == CompanionCommandKind::GiveItem && c->amount == 5 && c->arg == "linen cloth");
    CHECK_EQ(Arg("hey Elara could you hand me your hearthstone"), std::string("hearthstone"));
    CHECK_EQ(Arg("can I have the Runecloth Bag?"), std::string("runecloth bag"));
    CHECK_EQ(Arg("trade your copper ore to me"), std::string("copper ore"));
    CHECK(Kind("give me a hug") == CompanionCommandKind::None);
    CHECK(Kind("give me a moment") == CompanionCommandKind::None);
    CHECK(Kind("gib mir 3 gold") == CompanionCommandKind::GiveGold);
}

TEST(Commands, CastAttackSell)
{
    auto c = ParseCompanionCommand("cast Power Word: Fortitude on me", "Elara");
    CHECK(c && c->kind == CompanionCommandKind::Cast && c->onMe && c->arg == "power word fortitude");
    c = ParseCompanionCommand("please use your frost nova", "Elara");
    CHECK(c && c->kind == CompanionCommandKind::Cast && !c->onMe && c->arg == "frost nova");

    CHECK(Kind("attack the wolf over there") == CompanionCommandKind::AttackNamed);
    CHECK_EQ(Arg("attack the wolf over there"), std::string("wolf"));
    CHECK_EQ(Arg("Elara kill those murlocs"), std::string("murlocs"));
    CHECK(Kind("attack my target") == CompanionCommandKind::AttackTarget);
    CHECK(Kind("kill it!") == CompanionCommandKind::AttackTarget);
    CHECK(Kind("don't attack the wolf") == CompanionCommandKind::None);
    CHECK(Kind("never kill critters") == CompanionCommandKind::None);

    CHECK(Kind("can you sell your junk?") == CompanionCommandKind::SellJunk);
    CHECK(Kind("sell all the gray items") == CompanionCommandKind::SellJunk);
    CHECK(Kind("sell") == CompanionCommandKind::SellJunk);
    c = ParseCompanionCommand("sell your wolf meat at the vendor", "Elara");
    CHECK(c && c->kind == CompanionCommandKind::SellItem && c->arg == "wolf meat");
}

TEST(Commands, QuestsAndLead)
{
    CHECK(Kind("share your quests with me") == CompanionCommandKind::ShareQuests);
    CHECK(Kind("can you share quests") == CompanionCommandKind::ShareQuests);
    auto c = ParseCompanionCommand("share the quest Wolves Across the Border", "Elara");
    CHECK(c && c->kind == CompanionCommandKind::ShareQuest && c->arg == "wolves across the border");
    CHECK(Kind("lead the way") == CompanionCommandKind::Lead);
    c = ParseCompanionCommand("lead me to the quest Kobold Camp Cleanup", "Elara");
    CHECK(c && c->kind == CompanionCommandKind::Lead && c->arg == "kobold camp cleanup");
    CHECK_EQ(Arg("let's do the Fargodeep Mine quest"), std::string("fargodeep mine"));
    CHECK(Kind("i'll lead") == CompanionCommandKind::StopLead);
    CHECK(Kind("stop leading") == CompanionCommandKind::StopLead);
}

TEST(Commands, QuestionsAndChatAreNotCommands)
{
    CHECK(Kind("what spells can you cast?") == CompanionCommandKind::None);
    CHECK(Kind("do you have any gold?") == CompanionCommandKind::None);
    CHECK(Kind("how are you today") == CompanionCommandKind::None);
    CHECK(Kind("I love you") == CompanionCommandKind::None);
    CHECK(Kind("Elara") == CompanionCommandKind::None);
}

TEST(Commands, KnowledgeTopics)
{
    using namespace knowledge;
    CHECK(DetectKnowledgeTopics("Hey what do you have in your inventory") & Inventory);
    CHECK(DetectKnowledgeTopics("what quests are you on?") & Quests);
    CHECK(DetectKnowledgeTopics("which spells do you know") & Spells);
    CHECK(DetectKnowledgeTopics("how much gold do you have") & Gold);
    CHECK(DetectKnowledgeTopics("how much xp until you level?") & Xp);
    CHECK(DetectKnowledgeTopics("Who am I looking at?") & Target);
    CHECK(DetectKnowledgeTopics("nice armor") & Gear);
    CHECK_EQ(DetectKnowledgeTopics("I love the sunset"), 0u);
    CHECK_EQ(CreatureTypeFromWord("Beasts"), 1u);
    CHECK_EQ(CreatureTypeFromWord("undead"), 6u);
    CHECK_EQ(CreatureTypeFromWord("wolf"), 0u);
}
