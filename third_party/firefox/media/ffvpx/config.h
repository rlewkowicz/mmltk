/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MOZ_FFVPX_CONFIG_H)
#define MOZ_FFVPX_CONFIG_H

#if defined(MOZ_FFVPX_AUDIOONLY)
#if defined(__aarch64__)
#    include "config_unix_aarch64.h"
#else
#    include "config_generic.h"
#endif
#else
#if defined(XP_UNIX)
#if defined(__aarch64__)
#      include "config_unix_aarch64.h"
#elif defined(HAVE_64BIT_BUILD)
#      include "config_unix64.h"
#else
#      include "config_unix32.h"
#endif
#endif
#endif
#include "config_override.h"

#endif
