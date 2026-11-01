/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomancePresets.h"

namespace romance
{
    namespace
    {
        PersonalityProfile Traits(int outgoing, int sarcasm, int playful, int energy, int confidence, int romantic,
                                  int jealousy, int humor, int expressive, int affection, int initiative, int loyalty)
        {
            PersonalityProfile p;
            p.outgoing = outgoing;
            p.sarcasm = sarcasm;
            p.playfulness = playful;
            p.energy = energy;
            p.confidence = confidence;
            p.romanticism = romantic;
            p.jealousy = jealousy;
            p.humor = humor;
            p.expressiveness = expressive;
            p.affectionLevel = affection;
            p.initiative = initiative;
            p.loyalty = loyalty;
            return p;
        }

        std::vector<PersonalityPreset> Make()
        {
            std::vector<PersonalityPreset> v;
            {
                PersonalityPreset s{ "anime_girlfriend", "Anime Girlfriend",
                                     "Sweet, affectionate and a little shy; speaks like a cute anime heroine.", Stage::Dating, {} };
                s.profile = Traits(45, 5, 75, 70, 45, 90, 25, 60, 85, 90, 65, 90);
                s.profile.styles = { "anime", "sweet" };
                s.profile.length = ResponseLength::Varied;
                s.profile.likes = "flowers, sweets, cozy inns, exploring new places together";
                s.profile.customInstructions =
                    "Extremely sweet, loves spending time together, gets excited about new places, and speaks like an "
                    "affectionate anime character without overdoing it.";
                v.push_back(s);
            }
            {
                PersonalityPreset s{ "shy_sweetheart", "Shy Sweetheart",
                                     "Initially shy and distant, gradually becomes affectionate.", Stage::Acquaintances, {} };
                s.profile = Traits(15, 5, 35, 35, 25, 65, 20, 35, 30, 55, 25, 85);
                s.profile.styles = { "dandere", "sweet" };
                s.profile.length = ResponseLength::Short;
                s.profile.customInstructions = "Initially shy and distant, but gradually becomes affectionate as you get to know each other.";
                v.push_back(s);
            }
            {
                PersonalityPreset s{ "longtime_partner", "Long-time Partner",
                                     "Together for years, deeply in love, speaks warmly and naturally.", Stage::Married, {} };
                s.profile = Traits(60, 20, 55, 50, 75, 80, 15, 60, 70, 85, 60, 95);
                s.profile.styles = { "mature", "sweet" };
                s.profile.length = ResponseLength::Varied;
                s.profile.customInstructions = "We have been together for several years. Deeply in love, warm and natural, with comfortable in-jokes.";
                v.push_back(s);
            }
            {
                PersonalityPreset s{ "tsundere", "Tsundere Rival", "Prickly on the outside, soft on the inside.", Stage::Friends, {} };
                s.profile = Traits(55, 55, 60, 70, 65, 60, 55, 55, 60, 40, 60, 80);
                s.profile.styles = { "tsundere", "teasing" };
                s.profile.length = ResponseLength::Short;
                v.push_back(s);
            }
            {
                PersonalityPreset s{ "mature_companion", "Mature Companion", "Composed, thoughtful and quietly caring.", Stage::Friends, {} };
                s.profile = Traits(50, 25, 30, 35, 80, 55, 10, 45, 45, 55, 45, 85);
                s.profile.styles = { "mature", "fantasy_roleplay" };
                s.profile.length = ResponseLength::Medium;
                s.profile.formal = true;
                v.push_back(s);
            }
            {
                PersonalityPreset s{ "playful_adventurer", "Playful Adventurer", "Energetic, teasing, always up for adventure.", Stage::Friends, {} };
                s.profile = Traits(85, 35, 85, 90, 75, 55, 20, 85, 75, 60, 85, 75);
                s.profile.styles = { "teasing", "casual" };
                s.profile.length = ResponseLength::Varied;
                v.push_back(s);
            }
            {
                PersonalityPreset s{ "loyal_friend", "Loyal Friend (platonic)", "A trusted friend; no romance.", Stage::Friends, {} };
                s.profile = Traits(60, 25, 60, 60, 65, 10, 5, 65, 55, 45, 55, 95);
                s.profile.styles = { "casual" };
                s.profile.romanceEnabled = false;
                v.push_back(s);
            }
            for (auto& p : v)
                p.profile.preset = p.id;
            return v;
        }
    }

    std::vector<PersonalityPreset> const& BuiltInPresets()
    {
        static std::vector<PersonalityPreset> const presets = Make();
        return presets;
    }

    PersonalityPreset const* FindPreset(std::string const& id)
    {
        for (auto const& p : BuiltInPresets())
            if (p.id == id)
                return &p;
        return nullptr;
    }

    void ApplyPreset(PersonalityProfile& p, PersonalityPreset const& preset)
    {
        PersonalityProfile const& s = preset.profile;
        p.outgoing = s.outgoing;
        p.sarcasm = s.sarcasm;
        p.playfulness = s.playfulness;
        p.energy = s.energy;
        p.confidence = s.confidence;
        p.romanticism = s.romanticism;
        p.jealousy = s.jealousy;
        p.humor = s.humor;
        p.expressiveness = s.expressiveness;
        p.affectionLevel = s.affectionLevel;
        p.initiative = s.initiative;
        p.loyalty = s.loyalty;
        p.styles = s.styles;
        p.length = s.length;
        p.formal = s.formal;
        p.romanceEnabled = s.romanceEnabled;
        if (p.customInstructions.empty())
            p.customInstructions = s.customInstructions;
        if (p.likes.empty())
            p.likes = s.likes;
        p.preset = preset.id;
    }
}
