/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Gift evaluation: how meaningful a *completed* item transfer is to the companion.
 * Expensive is not automatically best: usefulness, class fit, upgrades, personal
 * likes, romantic meaning and repetition all matter.
 */

#ifndef MOD_ROMANCE_GIFT_H
#define MOD_ROMANCE_GIFT_H

#include "RomanceConfig.h"
#include "RomanceTypes.h"

#include <string>
#include <vector>

namespace romance
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
        explicit GiftEvaluator(RomanceConfig const& cfg) : _cfg(cfg) { }

        static GiftCategory Categorize(GiftItem const& item, RomanceConfig const& cfg);

        GiftEvaluation Evaluate(GiftItem const& item, PersonalityProfile const& p, RelationshipState const& st,
                                std::vector<GiftHistoryEntry> const& recent, TimePoint now) const;

        // Short English description for prompts and memories: "a Silver Ring (rare ring)".
        static std::string Describe(GiftItem const& item, GiftCategory cat);

    private:
        RomanceConfig const& _cfg;
    };
}

#endif
