/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Memory.h"

#include "mozilla/MathAlgorithms.h"
#include "mozilla/RandomNum.h"
#include "mozilla/TaggedAnonymousMemory.h"

#include "jit/JitOptions.h"
#include "js/HeapAPI.h"
#include "js/Utility.h"
#include "vm/JSContext.h"

#if defined(MOZ_MEMORY)
#  include "mozmemory_stall.h"
#endif

#include "util/Memory.h"


#  include <algorithm>
#  include <errno.h>
#  include <unistd.h>

#if defined(XP_LINUX)
#    include <sys/syscall.h>
#endif

#if !defined(__wasi__)
#    include <sys/mman.h>
#    include <sys/resource.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#endif



namespace js::gc {

static size_t pageSize = 0;

static size_t allocGranularity = 0;

static size_t numAddressBits = 0;

static size_t virtualMemoryLimit = size_t(-1);

static bool decommitEnabled = false;

static bool disableDecommitRequested = false;

#if defined(XP_UNIX)
static mozilla::Atomic<int, mozilla::Relaxed> growthDirection(0);
#endif

static const int MaxLastDitchAttempts = 32;

#if defined(JS_64BIT)
static const size_t MinAddressBitsForRandomAlloc = 43;

static const size_t HugeAllocationSize = 1024 * 1024 * 1024;

static size_t minValidAddress = 0;
static size_t maxValidAddress = 0;

static size_t hugeSplit = 0;
#endif

mozilla::Atomic<size_t, mozilla::Relaxed> gMappedMemorySizeBytes;
mozilla::Atomic<uint64_t, mozilla::Relaxed> gMappedMemoryOperations;

size_t SystemPageSize() { return pageSize; }

size_t SystemAddressBits() { return numAddressBits; }

size_t VirtualMemoryLimit() { return virtualMemoryLimit; }

bool UsingScattershotAllocator() {
#if defined(JS_64BIT)
  return numAddressBits >= MinAddressBitsForRandomAlloc;
#else
  return false;
#endif
}

enum class Commit : bool {
  No = false,
  Yes = true,
};

#if defined(__wasi__)
enum class PageAccess : int {
  None = 0,
  Read = 0,
  ReadWrite = 0,
  Execute = 0,
  ReadExecute = 0,
  ReadWriteExecute = 0,
};
#else
enum class PageAccess : int {
  None = PROT_NONE,
  Read = PROT_READ,
  ReadWrite = PROT_READ | PROT_WRITE,
  Execute = PROT_EXEC,
  ReadExecute = PROT_READ | PROT_EXEC,
  ReadWriteExecute = PROT_READ | PROT_WRITE | PROT_EXEC,
};
#endif

template <bool AlwaysGetNew = true>
static bool TryToAlignChunk(void** aRegion, void** aRetainedRegion,
                            size_t length, size_t alignment);

#if !defined(__wasi__)
static void* MapAlignedPagesSlow(size_t length, size_t alignment);
#endif
static void* MapAlignedPagesLastDitch(size_t length, size_t alignment,
                                      StallAndRetry stallAndRetry);

#if defined(JS_64BIT)
static void* MapAlignedPagesRandom(size_t length, size_t alignment);
#endif

void* TestMapAlignedPagesLastDitch(size_t length, size_t alignment) {
  void* region = MapAlignedPagesLastDitch(length, alignment, StallAndRetry::No);
  if (region) {
    RecordMemoryAlloc(length);
  }
  return region;
}

bool DecommitEnabled() { return decommitEnabled; }

void DisableDecommit() {
  MOZ_RELEASE_ASSERT(
      pageSize == 0,
      "DisableDecommit should be called before InitMemorySubsystem");
  disableDecommitRequested = true;
}

static inline size_t OffsetFromAligned(void* region, size_t alignment) {
  return uintptr_t(region) % alignment;
}

template <Commit commit, StallAndRetry retry = StallAndRetry::No>
static inline void* MapInternal(void* desired, size_t length) {
  void* region = nullptr;
#if defined(__wasi__)
  if (int err = posix_memalign(&region, gc::SystemPageSize(), length)) {
    MOZ_RELEASE_ASSERT(err == ENOMEM);
    return nullptr;
  }
  if (region) {
    memset(region, 0, length);
  }
#else
  int flags = MAP_PRIVATE | MAP_ANON;
  region = MozTaggedAnonymousMmap(desired, length, int(PageAccess::ReadWrite),
                                  flags, -1, 0, "js-gc-heap");
  if (region == MAP_FAILED) {
    return nullptr;
  }
#endif
  return region;
}

static inline void UnmapInternal(void* region, size_t length) {
  MOZ_ASSERT(region && OffsetFromAligned(region, allocGranularity) == 0);
  MOZ_ASSERT(length > 0 && length % pageSize == 0);

#if defined(__wasi__)
  free(region);
#else
  if (munmap(region, length)) {
    MOZ_RELEASE_ASSERT(errno == ENOMEM);
  }
#endif
}

template <Commit commit = Commit::Yes, StallAndRetry retry = StallAndRetry::No>
static inline void* MapMemory(size_t length) {
  MOZ_ASSERT(length > 0);

  return MapInternal<commit, retry>(nullptr, length);
}

template <Commit commit = Commit::Yes>
static inline void* MapMemoryAtFuzzy(void* desired, size_t length) {
  MOZ_ASSERT(desired && OffsetFromAligned(desired, allocGranularity) == 0);
  MOZ_ASSERT(length > 0);

  return MapInternal<commit>(desired, length);
}

template <Commit commit = Commit::Yes>
static inline void* MapMemoryAt(void* desired, size_t length) {
  MOZ_ASSERT(desired && OffsetFromAligned(desired, allocGranularity) == 0);
  MOZ_ASSERT(length > 0);

  void* region = MapInternal<commit>(desired, length);
  if (!region) {
    return nullptr;
  }

  if (region != desired) {
    UnmapInternal(region, length);
    return nullptr;
  }
  return region;
}

#if defined(JS_64BIT)

static inline uint64_t GetNumberInRange(uint64_t minNum, uint64_t maxNum) {
  const uint64_t MaxRand = UINT64_C(0xffffffffffffffff);

  MOZ_ASSERT(minNum <= maxNum);
  if (minNum == maxNum) {
    return minNum;
  }

  maxNum -= minNum;
  uint64_t binSize = 1 + (MaxRand - maxNum) / (maxNum + 1);

  uint64_t rndNum;
  do {
    mozilla::Maybe<uint64_t> result;
    do {
      result = mozilla::RandomUint64();
    } while (!result);
    rndNum = result.value() / binSize;
  } while (rndNum > maxNum);

  return minNum + rndNum;
}

static inline uint64_t FindAddressLimitInner(size_t highBit, size_t tries);

static size_t FindAddressLimit() {
  uint64_t low = 31;
  uint64_t highestSeen = (UINT64_C(1) << 32) - allocGranularity - 1;

  uint64_t high = 47;
  for (; high >= std::max(low, UINT64_C(46)); --high) {
    highestSeen = std::max(FindAddressLimitInner(high, 4), highestSeen);
    low = mozilla::FloorLog2(highestSeen);
  }
  while (high - 1 > low) {
    uint64_t middle = low + (high - low) / 2;
    highestSeen = std::max(FindAddressLimitInner(middle, 4), highestSeen);
    low = mozilla::FloorLog2(highestSeen);
    if (highestSeen < (UINT64_C(1) << middle)) {
      high = middle;
    }
  }
  do {
    high = low + 1;
    highestSeen = std::max(FindAddressLimitInner(high, 8), highestSeen);
    low = mozilla::FloorLog2(highestSeen);
  } while (low >= high);

  return low + 1;
}

static inline uint64_t FindAddressLimitInner(size_t highBit, size_t tries) {
  const size_t length = allocGranularity;  

  uint64_t highestSeen = 0;
  uint64_t startRaw = UINT64_C(1) << highBit;
  uint64_t endRaw = 2 * startRaw - length - 1;
  uint64_t start = (startRaw + length - 1) / length;
  uint64_t end = (endRaw - (length - 1)) / length;
  for (size_t i = 0; i < tries; ++i) {
    uint64_t desired = length * GetNumberInRange(start, end);
    void* address = MapMemoryAtFuzzy(reinterpret_cast<void*>(desired), length);
    uint64_t actual = uint64_t(address);
    if (address) {
      UnmapInternal(address, length);
    }
    if (actual > highestSeen) {
      highestSeen = actual;
      if (actual >= startRaw) {
        break;
      }
    }
  }
  return highestSeen;
}

#endif

void InitMemorySubsystem() {
  if (pageSize == 0) {
    pageSize = size_t(sysconf(_SC_PAGESIZE));
    allocGranularity = pageSize;

    decommitEnabled = pageSize == PageSize && !disableDecommitRequested;

#if defined(JS_64BIT)
    numAddressBits = FindAddressLimit();
    minValidAddress = allocGranularity;
    maxValidAddress = (UINT64_C(1) << numAddressBits) - 1 - allocGranularity;
    uint64_t maxJSAddress = UINT64_C(0x00007fffffffffff) - allocGranularity;
    if (maxValidAddress > maxJSAddress) {
      maxValidAddress = maxJSAddress;
      hugeSplit = UINT64_C(0x00003fffffffffff) - allocGranularity;
    } else {
      hugeSplit = (UINT64_C(1) << (numAddressBits - 1)) - 1 - allocGranularity;
    }
#else
    numAddressBits = 32;
#endif
#if defined(RLIMIT_AS)
    if (jit::HasJitBackend()) {
      rlimit as_limit;
      if (getrlimit(RLIMIT_AS, &as_limit) == 0 &&
          as_limit.rlim_max != RLIM_INFINITY) {
        virtualMemoryLimit = as_limit.rlim_max;
      }
    }
#endif
  }

  MOZ_ASSERT(gMappedMemorySizeBytes == 0);
}

void MapStack(size_t stackSize) {
  MOZ_ASSERT(js::CurrentThreadIsMainThread());
  MOZ_ASSERT(MaybeGetJSContext()->runtime()->isMainRuntime());

}

void CheckMemorySubsystemOnShutDown() {
  MOZ_ASSERT(gMappedMemorySizeBytes == 0);
}

#if defined(JS_64BIT)
static inline bool IsInvalidRegion(void* region, size_t length) {
  const uint64_t invalidPointerMask = UINT64_C(0xffff800000000000);
  return (uintptr_t(region) + length - 1) & invalidPointerMask;
}
#endif

void* MapAlignedPages(size_t length, size_t alignment,
                      StallAndRetry stallAndRetry) {
  MOZ_RELEASE_ASSERT(length > 0 && alignment > 0);
  MOZ_RELEASE_ASSERT(length % pageSize == 0);
  MOZ_RELEASE_ASSERT(std::max(alignment, allocGranularity) %
                         std::min(alignment, allocGranularity) ==
                     0);

  if (alignment < allocGranularity) {
    alignment = allocGranularity;
  }

#if defined(__wasi__)
  void* region = nullptr;
  if (int err = posix_memalign(&region, alignment, length)) {
    MOZ_ASSERT(err == ENOMEM);
    (void)err;
    return nullptr;
  }
  MOZ_ASSERT(region != nullptr);
  memset(region, 0, length);
  return region;
#else

#if defined(JS_64BIT)
  if (UsingScattershotAllocator()) {
    void* region = MapAlignedPagesRandom(length, alignment);
    if (region) {
      MOZ_RELEASE_ASSERT(!IsInvalidRegion(region, length));
      MOZ_ASSERT(OffsetFromAligned(region, alignment) == 0);
      RecordMemoryAlloc(length);
      return region;
    }
  }
#endif

  void* region = MapMemory(length);
  if (!region) {
    return nullptr;
  }

  if (OffsetFromAligned(region, alignment) == 0) {
    RecordMemoryAlloc(length);
    return region;
  }

  void* retainedRegion;
  if (TryToAlignChunk(&region, &retainedRegion, length, alignment)) {
    MOZ_ASSERT(region && OffsetFromAligned(region, alignment) == 0);
    MOZ_ASSERT(!retainedRegion);
    RecordMemoryAlloc(length);
    return region;
  }

  if (retainedRegion) {
    UnmapInternal(retainedRegion, length);
  }

  if (region) {
    MOZ_ASSERT(OffsetFromAligned(region, alignment) != 0);
    UnmapInternal(region, length);
  }

  region = MapAlignedPagesSlow(length, alignment);
  if (!region) {
    region = MapAlignedPagesLastDitch(length, alignment, stallAndRetry);
    if (!region) {
      return nullptr;
    }
  }

  MOZ_ASSERT(OffsetFromAligned(region, alignment) == 0);

  RecordMemoryAlloc(length);
  return region;
#endif
}

#if defined(JS_64BIT)

static void* MapAlignedPagesRandom(size_t length, size_t alignment) {
  MOZ_ASSERT(length != 0);
  if (length - 1 > maxValidAddress) {
    return nullptr;
  }

  uint64_t minNum, maxNum;
  if (length < HugeAllocationSize) {
    minNum = (minValidAddress + alignment - 1) / alignment;
    maxNum = (hugeSplit - (length - 1)) / alignment;
  } else {
    minNum = (hugeSplit + 1 + alignment - 1) / alignment;
    maxNum = (maxValidAddress - (length - 1)) / alignment;
  }

  if (minNum > maxNum) {
    return nullptr;
  }

  void* region = nullptr;
  for (size_t i = 1; i <= 1024; ++i) {
    if (i & 0xf) {
      uint64_t desired = alignment * GetNumberInRange(minNum, maxNum);
      region = MapMemoryAtFuzzy(reinterpret_cast<void*>(desired), length);

      if (!region) {
        continue;
      }
    } else {
      region = MapMemory(length);
      if (!region) {
        return nullptr;
      }
    }
    if (IsInvalidRegion(region, length)) {
      UnmapInternal(region, length);
      continue;
    }
    if (OffsetFromAligned(region, alignment) == 0) {
      return region;
    }
    void* retainedRegion = nullptr;
    if (TryToAlignChunk<false>(&region, &retainedRegion, length, alignment)) {
      MOZ_ASSERT(region && OffsetFromAligned(region, alignment) == 0);
      MOZ_ASSERT(!retainedRegion);
      return region;
    }
    MOZ_ASSERT(region && !retainedRegion);
    UnmapInternal(region, length);
  }

  if (numAddressBits < 48) {
    region = MapAlignedPagesSlow(length, alignment);
    if (region) {
      return region;
    }
  }
  if (length < HugeAllocationSize) {
    MOZ_CRASH("Couldn't allocate even after 1000 tries!");
  }

  return nullptr;
}

#endif

#if !defined(__wasi__)
static void* MapAlignedPagesSlow(size_t length, size_t alignment) {
  void* alignedRegion = nullptr;
  do {
    size_t reserveLength = length + alignment - pageSize;
    void* region = MapMemory(reserveLength);
    if (!region) {
      return nullptr;
    }
    alignedRegion =
        reinterpret_cast<void*>(AlignBytes(uintptr_t(region), alignment));
    if (alignedRegion != region) {
      UnmapInternal(region, uintptr_t(alignedRegion) - uintptr_t(region));
    }
    void* regionEnd =
        reinterpret_cast<void*>(uintptr_t(region) + reserveLength);
    void* alignedEnd =
        reinterpret_cast<void*>(uintptr_t(alignedRegion) + length);
    if (alignedEnd != regionEnd) {
      UnmapInternal(alignedEnd, uintptr_t(regionEnd) - uintptr_t(alignedEnd));
    }
  } while (!alignedRegion);

  return alignedRegion;
}
#endif

static void* MapAlignedPagesLastDitch(size_t length, size_t alignment,
                                      StallAndRetry stallAndRetry) {
  void* tempMaps[MaxLastDitchAttempts];
  int attempt = 0;
  void* region;

  if (stallAndRetry == StallAndRetry::Yes) {
    region = MapMemory<Commit::Yes, StallAndRetry::Yes>(length);
  } else {
    region = MapMemory<Commit::Yes, StallAndRetry::No>(length);
  }

  if (OffsetFromAligned(region, alignment) == 0) {
    return region;
  }
  for (; attempt < MaxLastDitchAttempts; ++attempt) {
    if (TryToAlignChunk(&region, tempMaps + attempt, length, alignment)) {
      MOZ_ASSERT(region && OffsetFromAligned(region, alignment) == 0);
      MOZ_ASSERT(!tempMaps[attempt]);
      break;  
    }
    if (!region || !tempMaps[attempt]) {
      break;  
    }
  }
  if (region && OffsetFromAligned(region, alignment) != 0) {
    UnmapInternal(region, length);
    region = nullptr;
  }
  while (--attempt >= 0) {
    UnmapInternal(tempMaps[attempt], length);
  }
  return region;
}


template <bool AlwaysGetNew>
static bool TryToAlignChunk(void** aRegion, void** aRetainedRegion,
                            size_t length, size_t alignment) {
  void* regionStart = *aRegion;
  MOZ_ASSERT(regionStart && OffsetFromAligned(regionStart, alignment) != 0);

  bool addressesGrowUpward = growthDirection > 0;
  bool directionUncertain = -8 < growthDirection && growthDirection <= 8;
  size_t offsetLower = OffsetFromAligned(regionStart, alignment);
  size_t offsetUpper = alignment - offsetLower;
  for (size_t i = 0; i < 2; ++i) {
    if (addressesGrowUpward) {
      void* upperStart =
          reinterpret_cast<void*>(uintptr_t(regionStart) + offsetUpper);
      void* regionEnd =
          reinterpret_cast<void*>(uintptr_t(regionStart) + length);
      if (MapMemoryAt(regionEnd, offsetUpper)) {
        UnmapInternal(regionStart, offsetUpper);
        if (directionUncertain) {
          ++growthDirection;
        }
        regionStart = upperStart;
        break;
      }
    } else {
      auto* lowerStart =
          reinterpret_cast<void*>(uintptr_t(regionStart) - offsetLower);
      auto* lowerEnd = reinterpret_cast<void*>(uintptr_t(lowerStart) + length);
      if (lowerStart && MapMemoryAt(lowerStart, offsetLower)) {
        UnmapInternal(lowerEnd, offsetLower);
        if (directionUncertain) {
          --growthDirection;
        }
        regionStart = lowerStart;
        break;
      }
    }
    if (!directionUncertain) {
      break;
    }
    addressesGrowUpward = !addressesGrowUpward;
  }

  void* retainedRegion = nullptr;
  bool result = OffsetFromAligned(regionStart, alignment) == 0;
  if (AlwaysGetNew && !result) {
    retainedRegion = regionStart;
    regionStart = MapMemory(length);
    result = OffsetFromAligned(regionStart, alignment) == 0;
    if (result) {
      UnmapInternal(retainedRegion, length);
      retainedRegion = nullptr;
    }
  }

  *aRegion = regionStart;
  *aRetainedRegion = retainedRegion;
  return regionStart && result;
}


void UnmapPages(void* region, size_t length) {
  MOZ_RELEASE_ASSERT(region &&
                     OffsetFromAligned(region, allocGranularity) == 0);
  MOZ_RELEASE_ASSERT(length > 0 && length % pageSize == 0);

  MOZ_MAKE_MEM_UNDEFINED(region, length);

  UnmapInternal(region, length);

#if !defined(__wasi__)
  RecordMemoryFree(length);
#endif
}

static void CheckDecommit(void* region, size_t length) {
  MOZ_RELEASE_ASSERT(region);
  MOZ_RELEASE_ASSERT(length > 0);

  MOZ_ASSERT(OffsetFromAligned(region, ArenaSize) == 0);
  MOZ_ASSERT(length % ArenaSize == 0);

  MOZ_RELEASE_ASSERT(OffsetFromAligned(region, pageSize) == 0);
  MOZ_RELEASE_ASSERT(length % pageSize == 0);
}

bool MarkPagesUnusedSoft(void* region, size_t length) {
  MOZ_ASSERT(DecommitEnabled());
  CheckDecommit(region, length);

  MOZ_MAKE_MEM_NOACCESS(region, length);

#if defined(__wasi__)
  return 0;
#else
  int status;
  do {
    status = madvise(region, length, MADV_DONTNEED);
  } while (status == -1 && errno == EAGAIN);
  return status == 0;
#endif
}

bool MarkPagesUnusedHard(void* region, size_t length) {
  CheckDecommit(region, length);

  MOZ_MAKE_MEM_NOACCESS(region, length);

  if (!DecommitEnabled()) {
    return true;
  }

  return MarkPagesUnusedSoft(region, length);
}

void MarkPagesInUseSoft(void* region, size_t length) {
  MOZ_ASSERT(DecommitEnabled());
  CheckDecommit(region, length);

  MOZ_MAKE_MEM_UNDEFINED(region, length);
}

bool MarkPagesInUseHard(void* region, size_t length) {
  if (js::oom::ShouldFailWithOOM()) {
    return false;
  }

  CheckDecommit(region, length);

  MOZ_MAKE_MEM_UNDEFINED(region, length);

  if (!DecommitEnabled()) {
    return true;
  }

  return true;
}

size_t GetPageFaultCount() {
#if defined(__wasi__)
  return 0;
#else
  struct rusage usage;
  int err = getrusage(RUSAGE_SELF, &usage);
  if (err) {
    return 0;
  }
  return usage.ru_majflt;
#endif
}

void* AllocateMappedContent(int fd, size_t offset, size_t length,
                            size_t alignment) {
#if defined(__wasi__)
  MOZ_CRASH("Not yet supported for WASI");
#else
  if (length == 0 || alignment == 0 || offset % alignment != 0 ||
      std::max(alignment, allocGranularity) %
              std::min(alignment, allocGranularity) !=
          0) {
    return nullptr;
  }

  size_t alignedOffset = offset - (offset % allocGranularity);
  size_t alignedLength = length + (offset % allocGranularity);

  size_t mappedLength = alignedLength;
  if (alignedLength % pageSize != 0) {
    mappedLength += pageSize - alignedLength % pageSize;
  }

  struct stat st;
  if (fstat(fd, &st) || offset >= uint64_t(st.st_size) ||
      length > uint64_t(st.st_size) - offset) {
    return nullptr;
  }

  void* region = MapAlignedPages(mappedLength, alignment);
  if (!region) {
    return nullptr;
  }

  uint8_t* map =
      static_cast<uint8_t*>(mmap(region, alignedLength, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_FIXED, fd, alignedOffset));
  if (map == MAP_FAILED) {
    UnmapInternal(region, mappedLength);
    RecordMemoryFree(mappedLength);
    return nullptr;
  }

#if defined(DEBUG)
  if (offset != alignedOffset) {
    memset(map, 0, offset - alignedOffset);
  }
  if (alignedLength % pageSize) {
    memset(map + alignedLength, 0, pageSize - (alignedLength % pageSize));
  }
#endif

  return map + (offset - alignedOffset);
#endif
}

void DeallocateMappedContent(void* region, size_t length) {
#if defined(__wasi__)
  MOZ_CRASH("Not yet supported for WASI");
#else
  if (!region) {
    return;
  }


  uintptr_t map = uintptr_t(region) - (uintptr_t(region) % allocGranularity);

  size_t alignedLength = length + (uintptr_t(region) % allocGranularity);

  size_t mappedLength = alignedLength;
  if (alignedLength % pageSize != 0) {
    mappedLength += pageSize - alignedLength % pageSize;
  }

  if (munmap(reinterpret_cast<void*>(map), alignedLength)) {
    MOZ_RELEASE_ASSERT(errno == ENOMEM);
  }

  RecordMemoryFree(mappedLength);

#endif
}

static inline void ProtectMemory(void* region, size_t length, PageAccess prot) {
  MOZ_RELEASE_ASSERT(region && OffsetFromAligned(region, pageSize) == 0);
  MOZ_RELEASE_ASSERT(length > 0 && length % pageSize == 0);
#if defined(__wasi__)
#else
  MOZ_RELEASE_ASSERT(mprotect(region, length, int(prot)) == 0);
#endif
}

void ProtectPages(void* region, size_t length) {
  ProtectMemory(region, length, PageAccess::None);
}

void MakePagesReadOnly(void* region, size_t length) {
  ProtectMemory(region, length, PageAccess::Read);
}

void UnprotectPages(void* region, size_t length) {
  ProtectMemory(region, length, PageAccess::ReadWrite);
}

void RecordMemoryAlloc(size_t bytes) {
  MOZ_ASSERT(bytes);
  MOZ_ASSERT((bytes % pageSize) == 0);

  gMappedMemorySizeBytes += bytes;
  gMappedMemoryOperations++;
}

void RecordMemoryFree(size_t bytes) {
  MOZ_ASSERT(bytes);
  MOZ_ASSERT((bytes % pageSize) == 0);
  MOZ_ASSERT(bytes <= gMappedMemorySizeBytes);

  gMappedMemorySizeBytes -= bytes;
  gMappedMemoryOperations++;
}

JS_PUBLIC_API ProfilerMemoryCounts GetProfilerMemoryCounts() {
  return {gc::gMappedMemorySizeBytes, gc::gMappedMemoryOperations};
}

}  
