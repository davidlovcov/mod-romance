/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Gift evaluation: how meaningful a *completed* item transfer is to the companion.
 * Expensive is not automatically best: usefulness, class fit, upgrades, personal
 * likes, romantic meaning and repetition all matter.
 */

#ifndef MOD_TRUE_COMPANION_GIFT_H
#define MOD_TRUE_COMPANION_GIFT_H

#include "TrueCompanionConfig.h"
#include "TrueCompanionTypes.h"

#include <string>
#include <vector>

namespace truecompanion
{
    enum class GiftCategory : uint8_t
    {
        Flowers = 0, Food, Jewelry, Ring, Necklace, Armor, Weapon, Consumable, Trinket, Gold, Junk, Other
    };
    char const* GiftCategoryId(GiftCategory c);

    struct GiftEvaluation
    {
        GiftCategory category = GiftCategory::Other;
        double score = 0;              // 0..100 overall meaning
        double affection = 0;          // relationship points to apply (before relationship caps)
        double attraction = 0;
        double friendship = 0;
        double trust = 0;
        double satisfaction = 0;
        bool romantic = false;         // romantic significance (flowers, jewelry, ring)
        bool isRing = false;
        bool memorable = false;        // store as a long-term gift memory
        bool awkward = false;          // e.g. a ring from a near stranger
        std::vector<std::string> notes; // reaction hints for the model ("upgrade", "matches likes: flowers")

        Json ToJson() const;
    };

    struct GiftHistoryEntry
    {
        uint32_t entry = 0;
        TimePoint at = 0;
        double score = 0;
    };

    class GiftEvaluator
    {
    public:
        explicit GiftEvaluator(TrueCompanionConfig const& cfg) : _cfg(cfg) { }

        static GiftCategory Categorize(GiftItem const& item, TrueCompanionConfig const& cfg);

        GiftEvaluation Evaluate(GiftItem const& item, PersonalityProfile const& p, RelationshipState const& st,
                                std::vector<GiftHistoryEntry> const& recent, TimePoint now) const;

        // Short English description for prompts and memories: "a Silver Ring (rare ring)".
        static std::string Describe(GiftItem const& item, GiftCategory cat);

    private:
        TrueCompanionConfig const& _cfg;
    };

    // Armor type a class wears best: 1 cloth, 2 leather, 3 mail, 4 plate (WoW class ids; mail/plate from level 40).
    int PreferredArmorTier(int classId, int level);

    // Which of a quest's choose-one rewards the companion should take: an index into `options`
    // (entry == 0 marks an empty slot, which is never picked; 0 if there is nothing to pick).
    // Order: gear that upgrades what she wears (her best armor type first), then other usable gear,
    // then usable non-gear (potions, bags), then things she cannot use (the more valuable the better).
    size_t PickQuestReward(std::vector<GiftItem> const& options, int preferredArmorTier);
}

#endif
