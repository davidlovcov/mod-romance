/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#ifndef MOD_TRUE_COMPANION_AC_CONFIG_H
#define MOD_TRUE_COMPANION_AC_CONFIG_H

#include "TrueCompanionConfig.h"

namespace truecompanion::ac
{
    // Reads every TrueCompanion.* option from mod_true_companion.conf (falls back to defaults).
    TrueCompanionConfig LoadConfigFromWorldserver();
}

#endif
