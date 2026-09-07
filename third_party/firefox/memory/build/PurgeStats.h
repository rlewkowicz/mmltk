/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZJEMALLOC_PURGE_STATS_H
#define MOZJEMALLOC_PURGE_STATS_H

#include "mozjemalloc_types.h"

namespace mozilla {

struct PurgeStats {
  arena_id_t arena_id;
  const char* arena_label;
  const char* caller;

  size_t pages_dirty = 0;

  size_t pages_total = 0;

  size_t pages_unpurgable = 0;

  size_t system_calls = 0;
  size_t chunks = 0;

  PurgeStats(arena_id_t aId, const char* aLabel, const char* aCaller)
      : arena_id(aId), arena_label(aLabel), caller(aCaller) {}
};

}  

#endif  // MOZJEMALLOC_PURGE_STATS_H
