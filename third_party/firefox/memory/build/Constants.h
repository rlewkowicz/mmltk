/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(CONSTANTS_H)
#define CONSTANTS_H

#include "mozilla/Literals.h"

#include <bit>

#include "Utils.h"





#if defined(XP_LINUX) && defined(MADV_FREE)
#    undef MADV_FREE
#endif
#if !defined(MADV_FREE)
#    define MADV_FREE MADV_DONTNEED
#endif



static constexpr size_t kMinQuantumClass = 16;
static constexpr size_t kMinQuantumWideClass = 512;
static constexpr size_t kMinSubPageClass = 4_KiB;

static constexpr size_t kQuantum = 16;
static constexpr size_t kQuantumMask = kQuantum - 1;
static constexpr size_t kQuantumWide = 256;
static constexpr size_t kQuantumWideMask = kQuantumWide - 1;

static constexpr size_t kMaxQuantumClass = kMinQuantumWideClass - kQuantum;
static constexpr size_t kMaxQuantumWideClass = kMinSubPageClass - kQuantumWide;

static_assert(std::has_single_bit(kQuantum), "kQuantum is not a power of two");
static_assert(std::has_single_bit(kQuantumWide),
              "kQuantumWide is not a power of two");

static_assert(kMaxQuantumClass % kQuantum == 0,
              "kMaxQuantumClass is not a multiple of kQuantum");
static_assert(kMaxQuantumWideClass % kQuantumWide == 0,
              "kMaxQuantumWideClass is not a multiple of kQuantumWide");
static_assert(kQuantum < kQuantumWide,
              "kQuantum must be smaller than kQuantumWide");
static_assert(std::has_single_bit(kMinSubPageClass),
              "kMinSubPageClass is not a power of two");

static constexpr size_t kNumQuantumClasses =
    (kMaxQuantumClass + kQuantum - kMinQuantumClass) / kQuantum;
static constexpr size_t kNumQuantumWideClasses =
    (kMaxQuantumWideClass + kQuantumWide - kMinQuantumWideClass) / kQuantumWide;

static constexpr size_t kChunkSize = 1_MiB;
static constexpr size_t kChunkSizeMask = kChunkSize - 1;

constexpr size_t kCacheLineSize =
    64
    ;
constexpr size_t kCacheLineMask = kCacheLineSize - 1;

static constexpr size_t gRecycleLimit = 128_MiB;

#endif
