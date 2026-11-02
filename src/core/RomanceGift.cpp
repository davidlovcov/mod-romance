/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceGift.h"
#include "RomanceMemory.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace romance
{
    namespace
    {
        // WoW 3.3.5a item constants (ItemTemplate.h): kept here so the core stays engine independent.
        constexpr int ITEM_CLASS_CONSUMABLE = 0;
        constexpr int ITEM_CLASS_WEAPON = 2;
        constexpr int ITEM_CLASS_ARMOR = 4;
        constexpr int ITEM_SUBCLASS_FOOD = 5;
        constexpr int INVTYPE_NECK = 2;
        constexpr int INVTYPE_FINGER = 11;
        constexpr int INVTYPE_TRINKET = 12;

        char const* const kCategoryIds[] = {
            "flowers", "food", "jewelry", "ring", "necklace", "armor", "weapon", "consumable", "trinket", "gold", "junk", "other" };

        bool NameHasAny(std::string const& lowerName, std::vector<std::string> const& words)
        {
            for (auto const& w : words)
                if (!w.empty() && lowerName.find(w) != std::string::npos)
                    return true;
            return false;
        }

        bool IsFloral(std::string const& lowerName)
        {
            static std::vector<std::string> const floral = { "rose", "flower", "bouquet", "lily", "blossom", "bloom",
                                                             "blume", "rosen", "orchid", "tulip", "daisy", "lotus" };
            return NameHasAny(lowerName, floral);
        }

        double StageRomanceFactor(Stage s)
        {
            switch (s)
            {
                case Stage::Strangers: return 0.1;
                case Stage::Acquaintances: return 0.25;
                case Stage::Friends: return 0.5;
                case Stage::CloseFriends: return 0.7;
                default: return 1.0;
            }
        }
    }

    char const* GiftCategoryId(GiftCategory c) { return kCategoryIds[int(c)]; }

    Json GiftEvaluation::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("category", GiftCategoryId(category));
        j.Set("score", score);
        j.Set("affection", affection);
        j.Set("attraction", attraction);
        j.Set("friendship", friendship);
        j.Set("romantic", romantic);
        j.Set("ring", isRing);
        j.Set("awkward", awkward);
        Json n = Json::MakeArray();
        for (auto const& s : notes)
            n.Push(s);
        j.Set("notes", n);
        return j;
    }

    GiftCategory GiftEvaluator::Categorize(GiftItem const& item, RomanceConfig const& cfg)
    {
        if (item.entry == 0)
            return item.money > 0 ? GiftCategory::Gold : GiftCategory::Other;
        std::string lower = ToLower(item.name);
        if (item.inventoryType == INVTYPE_FINGER)
            return GiftCategory::Ring;
        if (item.inventoryType == INVTYPE_NECK)
            return GiftCategory::Necklace;
        if (item.inventoryType == INVTYPE_TRINKET)
            return GiftCategory::Trinket;
        bool listed = std::find(cfg.romanticItemEntries.begin(), cfg.romanticItemEntries.end(), item.entry) != cfg.romanticItemEntries.end();
        if (IsFloral(lower) || (listed && item.itemClass != ITEM_CLASS_CONSUMABLE))
            return GiftCategory::Flowers;
        if (item.quality == 0)
            return GiftCategory::Junk;
        if (item.itemClass == ITEM_CLASS_CONSUMABLE)
            return item.itemSubClass == ITEM_SUBCLASS_FOOD ? GiftCategory::Food : GiftCategory::Consumable;
        if (item.itemClass == ITEM_CLASS_WEAPON)
            return GiftCategory::Weapon;
        if (item.itemClass == ITEM_CLASS_ARMOR)
            return GiftCategory::Armor;
        return GiftCategory::Other;
    }

    std::string GiftEvaluator::Describe(GiftItem const& item, GiftCategory cat)
    {
        static char const* const qualities[] = { "poor", "common", "uncommon", "rare", "epic", "legendary", "artifact", "heirloom" };
        std::ostringstream o;
        if (cat == GiftCategory::Gold)
        {
            uint32_t gold = item.money / 10000;
            o << gold << " gold";
            return o.str();
        }
        if (item.count > 1)
            o << item.count << "x ";
        o << item.name << " (" << qualities[std::max(0, std::min(7, item.quality))] << " " << GiftCategoryId(cat) << ")";
        return o.str();
    }

    GiftEvaluation GiftEvaluator::Evaluate(GiftItem const& item, PersonalityProfile const& p, RelationshipState const& st,
                                           std::vector<GiftHistoryEntry> const& recent, TimePoint now) const
    {
        GiftEvaluation g;
        g.category = Categorize(item, _cfg);
        std::string lower = ToLower(item.name);
        bool romanticCtx = st.romantic && p.romanceEnabled;

        double usefulness = 0, rarity = 0, value = 0, romance = 0, personal = 0;

        static double const qualityScore[] = { 0, 4, 10, 18, 26, 32, 32, 20 };
        rarity = qualityScore[std::max(0, std::min(7, item.quality))];
        value = std::min(15.0, std::log10(double(item.sellPrice) * std::max<uint32_t>(1, item.count) + 1.0) * 3.0);

        // usefulness: can she actually use it, and is it better than what she wears?
        if (item.isEquippable)
        {
            if (!item.usableByCompanion)
            {
                usefulness = 0;
                g.notes.push_back("you cannot use this item (wrong class or armor type)");
            }
            else if (item.equippedItemLevel < 0)
            {
                usefulness = 30;
                g.notes.push_back("fills an empty equipment slot");
            }
            else
            {
                int delta = item.itemLevel - item.equippedItemLevel;
                if (delta > 0)
                {
                    usefulness = 20 + std::min(30, delta * 2);
                    g.notes.push_back("a real upgrade over your current gear");
                }
                else
                {
                    usefulness = 4;
                    g.notes.push_back("not better than what you already wear");
                }
            }
        }
        else if (g.category == GiftCategory::Consumable)
            usefulness = 12;
        else if (g.category == GiftCategory::Food)
            usefulness = 8;

        // romantic meaning
        switch (g.category)
        {
            case GiftCategory::Flowers: romance = 25; g.romantic = true; break;
            case GiftCategory::Ring: romance = 28 * _cfg.jewelryMultiplier; g.romantic = true; g.isRing = true; break;
            case GiftCategory::Necklace: romance = 20 * _cfg.jewelryMultiplier; g.romantic = true; break;
            default:
                if (NameHasAny(lower, _cfg.romanticItemKeywords) ||
                    std::find(_cfg.romanticItemEntries.begin(), _cfg.romanticItemEntries.end(), item.entry) != _cfg.romanticItemEntries.end())
                {
                    romance = 15;
                    g.romantic = true;
                }
                break;
        }
        if (g.romantic)
        {
            double f = StageRomanceFactor(st.stage);
            if (g.isRing && st.stage <= Stage::Acquaintances)
            {
                g.awkward = true;
                g.notes.push_back("a ring from someone you barely know feels too forward");
                f *= 0.5;
            }
            if (!romanticCtx)
                f = std::min(f, 0.4);
            romance *= f;
            if (g.category == GiftCategory::Ring && st.stage >= Stage::Dating)
                g.notes.push_back("a ring carries deep romantic meaning between you");
            else if (g.category == GiftCategory::Necklace || g.category == GiftCategory::Ring)
                g.notes.push_back("jewelry feels personal and special");
            else if (g.category == GiftCategory::Flowers)
                g.notes.push_back("a romantic gesture");
        }

        // personal taste: does the gift match her likes / interests, or something she dislikes?
        std::vector<std::string> itemWords = Tokenize(item.name);
        itemWords.push_back(GiftCategoryId(g.category));
        if (g.category == GiftCategory::Flowers)
            itemWords.push_back("flowers"), itemWords.push_back("flower");
        if (g.category == GiftCategory::Ring || g.category == GiftCategory::Necklace)
            itemWords.push_back("jewelry"), itemWords.push_back("jewellery");
        auto matches = [&](std::string const& text)
        {
            std::vector<std::string> t = Tokenize(text);
            for (auto const& w : itemWords)
                for (auto const& x : t)
                    if (w.size() >= 3 && (x == w || (x.size() >= 4 && w.size() >= 4 && (x.find(w) == 0 || w.find(x) == 0))))
                        return true;
            return false;
        };
        if (matches(p.likes) || matches(p.interests))
        {
            personal = 25;
            g.notes.push_back("matches something you love");
        }
        if (matches(p.dislikes))
        {
            personal = -30;
            g.notes.push_back("it is something you dislike");
        }

        if (g.category == GiftCategory::Junk)
        {
            usefulness = 0;
            g.notes.push_back("this is basically junk");
        }

        double raw = usefulness + rarity + value + romance + personal;
        if (g.category == GiftCategory::Gold)
        {
            double gold = item.money / 10000.0;
            raw = std::min(25.0, std::log10(gold + 1.0) * 10.0);
            g.notes.push_back("a gift of coin is practical rather than personal");
        }

        // repetition: the same item again and again means less each time
        int sameEntry = 0, sameCategoryHour = 0;
        double dayAffection = 0;
        for (auto const& h : recent)
        {
            if (now - h.at > 24 * 3600)
                continue;
            dayAffection += h.score / 100.0 * 8.0 * _cfg.giftMultiplier;
            if (h.entry == item.entry && item.entry != 0)
                ++sameEntry;
            if (now - h.at < 3600)
                ++sameCategoryHour;
        }
        double repeat = 1.0 / (1.0 + sameEntry) * (sameCategoryHour >= 3 ? 0.6 : 1.0);
        if (sameEntry > 0)
            g.notes.push_back("you received this same item recently");

        g.score = std::max(0.0, std::min(100.0, raw * repeat));

        double mult = _cfg.giftMultiplier;
        g.affection = g.score / 100.0 * 8.0 * mult;
        if (g.romantic && romanticCtx)
            g.attraction = romance / 50.0 * 4.0 * mult * repeat;
        g.friendship = std::max(0.0, (usefulness + std::max(0.0, personal)) / 100.0 * 4.0 * mult * repeat);
        g.trust = g.score >= 60 ? 1.0 : 0.0;
        g.satisfaction = g.score / 100.0 * 4.0 * mult;
        if (personal < 0)
        {
            g.affection = 0;
            g.satisfaction = -1.5;
        }
        if (g.awkward)
            g.attraction = 0;

        // Daily cap: gift farming cannot max out a relationship.
        double room = std::max(0.0, _cfg.giftDailyCap - dayAffection);
        if (g.affection > room)
        {
            double f = g.affection > 0 ? room / g.affection : 0;
            g.affection *= f;
            g.attraction *= f;
            g.friendship *= f;
            g.satisfaction *= f;
            g.trust = 0;
            g.notes.push_back("daily gift limit reached");
        }

        g.memorable = g.score >= 35 || g.romantic || g.isRing || item.quality >= 4;
        return g;
    }

    int PreferredArmorTier(int classId, int level)
    {
        switch (classId)
        {
            case 1:    // warrior
            case 2:    // paladin
                return level >= 40 ? 4 : 3;
            case 6:    // death knight
                return 4;
            case 3:    // hunter
            case 7:    // shaman
                return level >= 40 ? 3 : 2;
            case 4:    // rogue
            case 11:   // druid
                return 2;
            default:   // priest, mage, warlock
                return 1;
        }
    }

    namespace
    {
        // Cloth..plate for body armor; 0 for cloaks, jewelry, weapons and everything else.
        int BodyArmorTier(GiftItem const& g)
        {
            if (g.itemClass != ITEM_CLASS_ARMOR || g.itemSubClass < 1 || g.itemSubClass > 4)
                return 0;
            switch (g.inventoryType)
            {
                case 1: case 3: case 5: case 6: case 7: case 8: case 9: case 10: case 20:   // head..hands, robe
                    return g.itemSubClass;
                default:
                    return 0;
            }
        }
    }

    size_t PickQuestReward(std::vector<GiftItem> const& options, int preferredArmorTier)
    {
        size_t best = 0;
        long long bestScore = -1;
        for (size_t i = 0; i < options.size(); ++i)
        {
            GiftItem const& g = options[i];
            if (!g.entry)
                continue;
            long long worth = std::min<long long>(g.sellPrice / 100, 9999);
            long long score;
            if (!g.usableByCompanion && g.isEquippable)
                score = worth;                                         // cannot use it: sell value only
            else if (g.isEquippable)
            {
                long long gain = g.equippedItemLevel < 0 ? g.itemLevel : g.itemLevel - g.equippedItemLevel;
                score = gain > 0 ? 100000 + gain * 100 : 50000 + worth;   // upgrade / usable but no better
                int tier = BodyArmorTier(g);
                if (gain > 0 && tier > 0 && preferredArmorTier > 0 && tier < preferredArmorTier)
                    score -= 40000;                                    // an upgrade, but not her armor type
            }
            else
                score = 20000 + worth;                                 // potions, bags, trinkets without a slot
            if (score > bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }
}
