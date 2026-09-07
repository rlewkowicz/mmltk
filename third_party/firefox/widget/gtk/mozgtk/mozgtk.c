/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Types.h"

#include <gdk/gdk.h>

MOZ_EXPORT void mozgtk_linker_holder() { gdk_display_get_default(); }
