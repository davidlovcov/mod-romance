/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Built-in personality presets. Players start from one and customize freely.
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
    };

    std::vector<PersonalityPreset> const& BuiltInPresets();
    PersonalityPreset const* FindPreset(std::string const& id);

    // Applies preset traits/styles to `p`, keeping identity fields the player already filled in.
    void ApplyPreset(PersonalityProfile& p, PersonalityPreset const& preset);
}

#endif
