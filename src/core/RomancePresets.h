/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Personality presets.
 *
 * A preset is a complete personality: traits, response style, age, gender,
 * pronouns, every story field and custom instructions. Applying a preset
 * replaces all of these. Only the identity the player chose (name, nickname),
 * the reply language, custom stage labels and the proposal setting are kept.
 *
 * Built-in presets ship with the module (female and male variants). Players can
 * also save their own personality as a custom preset (stored per account,
 * id "custom:<n>").
 */

#ifndef MOD_ROMANCE_PRESETS_H
#define MOD_ROMANCE_PRESETS_H

#include "RomanceTypes.h"

#include <string>
#include <vector>

namespace romance
{
    struct PersonalityPreset
    {
        std::string id;
        std::string label;
        std::string description;
        Stage suggestedStage = Stage::Strangers;
        PersonalityProfile profile;
        bool custom = false;

        Json ToJson() const;
    };

    std::vector<PersonalityPreset> const& BuiltInPresets();
    PersonalityPreset const* FindPreset(std::string const& id);

    // Replaces everything in `p` with the preset, except name, nickname,
    // language, stage labels and the proposal setting.
    void ApplyPreset(PersonalityProfile& p, PersonalityPreset const& preset);

    // "custom:<n>" -> n, otherwise 0.
    uint32_t CustomPresetNumber(std::string const& id);
    std::string CustomPresetId(uint32_t n);
}

#endif
