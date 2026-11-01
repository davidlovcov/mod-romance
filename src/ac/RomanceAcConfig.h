/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#ifndef MOD_ROMANCE_AC_CONFIG_H
#define MOD_ROMANCE_AC_CONFIG_H

#include "RomanceConfig.h"

namespace romance::ac
{
    // Reads every Romance.* option from mod_romance.conf (falls back to defaults).
    RomanceConfig LoadConfigFromWorldserver();
}

#endif
