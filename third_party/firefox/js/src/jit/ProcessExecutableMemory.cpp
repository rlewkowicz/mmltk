/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ProcessExecutableMemory.h"

#include "mozilla/Array.h"
#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TaggedAnonymousMemory.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <errno.h>

#include "jsfriendapi.h"

#include "gc/Memory.h"
#include "jit/FlushICache.h"  // js::jit::FlushICache
#include "jit/JitOptions.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "util/Memory.h"
#include "util/Poison.h"
#include "util/RandomSeed.h"
#include "vm/MutexIDs.h"

#if defined(__wasi__)
#if defined(JS_CODEGEN_WASM32)
#    include <cstdlib>
#else
#endif
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#if defined(MOZ_VALGRIND)
#  include <valgrind/valgrind.h>
#endif

using namespace js;
using namespace js::jit;

#if defined(__wasi__)
#if defined(JS_CODEGEN_WASM32)
static void* ReserveProcessExecutableMemory(size_t bytes) {
  return malloc(bytes);
}

static void DeallocateProcessExecutableMemory(void* addr, size_t bytes) {
  free(addr);
}

[[nodiscard]] static bool CommitPages(void* addr, size_t bytes,
                                      ProtectionSetting protection) {
  return true;
}

static void DecommitPages(void* addr, size_t bytes) {}

#else
static void* ReserveProcessExecutableMemory(size_t bytes) {
  MOZ_CRASH("NYI for WASI.");
  return nullptr;
}
static void DeallocateProcessExecutableMemory(void* addr, size_t bytes) {
  MOZ_CRASH("NYI for WASI.");
}
[[nodiscard]] static bool CommitPages(void* addr, size_t bytes,
                                      ProtectionSetting protection) {
  MOZ_CRASH("NYI for WASI.");
  return false;
}
static void DecommitPages(void* addr, size_t bytes) {
  MOZ_CRASH("NYI for WASI.");
}
#endif
#else
#if !defined(MAP_NORESERVE)
#    define MAP_NORESERVE 0
#endif

static void* ComputeRandomAllocationAddress() {
  uint64_t rand = js::GenerateRandomSeed();

#if defined(HAVE_64BIT_BUILD)
  rand >>= 18;
#else
  rand >>= 34;
  rand += 512 * 1024 * 1024;
#endif

  uintptr_t mask = ~uintptr_t(gc::SystemPageSize() - 1);
  return (void*)uintptr_t(rand & mask);
}

static void DecommitPages(void* addr, size_t bytes);

static void* ReserveProcessExecutableMemory(size_t bytes) {

  void* randomAddr = ComputeRandomAllocationAddress();
  unsigned protection = PROT_NONE;
  unsigned flags = MAP_NORESERVE | MAP_PRIVATE | MAP_ANON;
  void* p = MozTaggedAnonymousMmap(randomAddr, bytes, protection, flags, -1, 0,
                                   "js-executable-memory");
  if (p == MAP_FAILED) {
    return nullptr;
  }
  return p;
}

static void DeallocateProcessExecutableMemory(void* addr, size_t bytes) {
  mozilla::DebugOnly<int> result = munmap(addr, bytes);
  MOZ_ASSERT(!result || errno == ENOMEM);
}

static unsigned ProtectionSettingToFlags(ProtectionSetting protection) {
  if (!JitOptions.writeProtectCode) {
    return PROT_READ | PROT_WRITE | PROT_EXEC;
  }
#if defined(MOZ_VALGRIND)
  if (RUNNING_ON_VALGRIND) {
    switch (protection) {
      case ProtectionSetting::Writable:
        return PROT_READ | PROT_WRITE | PROT_EXEC;
      case ProtectionSetting::Executable:
        return PROT_READ | PROT_EXEC;
    }
    MOZ_CRASH();
  }
#endif
  switch (protection) {
    case ProtectionSetting::Writable:
      return PROT_READ | PROT_WRITE;
    case ProtectionSetting::Executable:
      return PROT_READ | PROT_EXEC;
  }
  MOZ_CRASH();
}

[[nodiscard]] static bool CommitPages(void* addr, size_t bytes,
                                      ProtectionSetting protection) {
  unsigned flags = ProtectionSettingToFlags(protection);
  void* p = MozTaggedAnonymousMmap(addr, bytes, flags,
                                   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0,
                                   "js-executable-memory");
  if (p == MAP_FAILED) {
    return false;
  }
  MOZ_RELEASE_ASSERT(p == addr);
  return true;
}

static void DecommitPages(void* addr, size_t bytes) {
  void* p = MozTaggedAnonymousMmap(addr, bytes, PROT_NONE,
                                   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0,
                                   "js-executable-memory");
  MOZ_RELEASE_ASSERT(addr == p);
}
#endif

template <size_t NumBits>
class PageBitSet {
  using WordType = uint32_t;
  static const size_t BitsPerWord = sizeof(WordType) * 8;

  static_assert((NumBits % BitsPerWord) == 0,
                "NumBits must be a multiple of BitsPerWord");
  static const size_t NumWords = NumBits / BitsPerWord;

  mozilla::Array<WordType, NumWords> words_;

  uint32_t indexToWord(uint32_t index) const {
    MOZ_ASSERT(index < NumBits);
    return index / BitsPerWord;
  }
  WordType indexToBit(uint32_t index) const {
    MOZ_ASSERT(index < NumBits);
    return WordType(1) << (index % BitsPerWord);
  }

 public:
  void init() { mozilla::PodArrayZero(words_); }
  bool contains(size_t index) const {
    uint32_t word = indexToWord(index);
    return words_[word] & indexToBit(index);
  }
  void insert(size_t index) {
    MOZ_ASSERT(!contains(index));
    uint32_t word = indexToWord(index);
    words_[word] |= indexToBit(index);
  }
  void remove(size_t index) {
    MOZ_ASSERT(contains(index));
    uint32_t word = indexToWord(index);
    words_[word] &= ~indexToBit(index);
  }

#if defined(DEBUG)
  bool empty() const {
    for (size_t i = 0; i < NumWords; i++) {
      if (words_[i] != 0) {
        return false;
      }
    }
    return true;
  }
#endif
};

class ProcessExecutableMemory {
  static_assert(
      (MaxCodeBytesPerProcess % ExecutableCodePageSize) == 0,
      "MaxCodeBytesPerProcess must be a multiple of ExecutableCodePageSize");
  static const size_t MaxCodePages =
      MaxCodeBytesPerProcess / ExecutableCodePageSize;

  uint8_t* base_;

  Mutex lock_ MOZ_UNANNOTATED;

  mozilla::Atomic<size_t, mozilla::ReleaseAcquire> pagesAllocated_;

  size_t cursor_;

  mozilla::Maybe<mozilla::non_crypto::XorShift128PlusRNG> rng_;
  PageBitSet<MaxCodePages> pages_;

 public:
  ProcessExecutableMemory()
      : base_(nullptr),
        lock_(mutexid::ProcessExecutableRegion),
        pagesAllocated_(0),
        cursor_(0),
        pages_() {}

  [[nodiscard]] bool init() {
    pages_.init();

    MOZ_RELEASE_ASSERT(!initialized());
    MOZ_RELEASE_ASSERT(HasJitBackend());
    MOZ_RELEASE_ASSERT(gc::SystemPageSize() <= ExecutableCodePageSize);

    void* p = ReserveProcessExecutableMemory(MaxCodeBytesPerProcess);
    if (!p) {
      return false;
    }

    base_ = static_cast<uint8_t*>(p);

    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    rng_.emplace(seed[0], seed[1]);
    return true;
  }

  uint8_t* base() const { return base_; }

  bool initialized() const { return base_ != nullptr; }

  size_t bytesAllocated() const {
    MOZ_ASSERT(pagesAllocated_ <= MaxCodePages);
    return pagesAllocated_ * ExecutableCodePageSize;
  }

  void release() {
    MOZ_ASSERT(initialized());
    MOZ_ASSERT(pages_.empty());
    MOZ_ASSERT(pagesAllocated_ == 0);
    DeallocateProcessExecutableMemory(base_, MaxCodeBytesPerProcess);
    base_ = nullptr;
    rng_.reset();
    MOZ_ASSERT(!initialized());
  }

  void assertValidAddress(void* p, size_t bytes) const {
    MOZ_RELEASE_ASSERT(p >= base_ &&
                       uintptr_t(p) + bytes <=
                           uintptr_t(base_) + MaxCodeBytesPerProcess);
  }

  bool containsAddress(const void* p) const {
    return p >= base_ &&
           uintptr_t(p) < uintptr_t(base_) + MaxCodeBytesPerProcess;
  }

  void* allocate(size_t bytes, ProtectionSetting protection,
                 MemCheckKind checkKind);
  void deallocate(void* addr, size_t bytes, bool decommit);
};

void* ProcessExecutableMemory::allocate(size_t bytes,
                                        ProtectionSetting protection,
                                        MemCheckKind checkKind) {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(HasJitBackend());
  MOZ_ASSERT(bytes > 0);
  MOZ_ASSERT((bytes % ExecutableCodePageSize) == 0);

  size_t numPages = bytes / ExecutableCodePageSize;

  void* p = nullptr;
  {
    LockGuard<Mutex> guard(lock_);
    MOZ_ASSERT(pagesAllocated_ <= MaxCodePages);

    if (pagesAllocated_ + numPages >= MaxCodePages) {
      return nullptr;
    }

    MOZ_ASSERT(bytes <= MaxCodeBytesPerProcess);

    size_t page = cursor_ + (rng_.ref().next() % 2);

    for (size_t i = 0; i < MaxCodePages; i++) {
      if (page + numPages > MaxCodePages) {
        page = 0;
      }

      bool available = true;
      for (size_t j = 0; j < numPages; j++) {
        if (pages_.contains(page + j)) {
          available = false;
          break;
        }
      }
      if (!available) {
        page++;
        continue;
      }

      for (size_t j = 0; j < numPages; j++) {
        pages_.insert(page + j);
      }

      pagesAllocated_ += numPages;
      MOZ_ASSERT(pagesAllocated_ <= MaxCodePages);

      if (numPages <= 2) {
        cursor_ = page + numPages;
      }

      p = base_ + page * ExecutableCodePageSize;
      break;
    }
    if (!p) {
      return nullptr;
    }
  }

  if (!CommitPages(p, bytes, protection)) {
    deallocate(p, bytes,  false);
    return nullptr;
  }

#if !defined(__wasi__)
  gc::RecordMemoryAlloc(bytes);
#endif

  SetMemCheckKind(p, bytes, checkKind);

  return p;
}

void ProcessExecutableMemory::deallocate(void* addr, size_t bytes,
                                         bool decommit) {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(addr);
  MOZ_ASSERT((uintptr_t(addr) % gc::SystemPageSize()) == 0);
  MOZ_ASSERT(bytes > 0);
  MOZ_ASSERT((bytes % ExecutableCodePageSize) == 0);

  assertValidAddress(addr, bytes);

  size_t firstPage =
      (static_cast<uint8_t*>(addr) - base_) / ExecutableCodePageSize;
  size_t numPages = bytes / ExecutableCodePageSize;

  MOZ_MAKE_MEM_NOACCESS(addr, bytes);
  if (decommit) {
    DecommitPages(addr, bytes);
#if !defined(__wasi__)
    gc::RecordMemoryFree(bytes);
#endif
  }

  LockGuard<Mutex> guard(lock_);
  MOZ_ASSERT(numPages <= pagesAllocated_);
  pagesAllocated_ -= numPages;

  for (size_t i = 0; i < numPages; i++) {
    pages_.remove(firstPage + i);
  }

  if (firstPage < cursor_) {
    cursor_ = firstPage;
  }
}

MOZ_RUNINIT static ProcessExecutableMemory execMemory;

void* js::jit::AllocateExecutableMemory(size_t bytes,
                                        ProtectionSetting protection,
                                        MemCheckKind checkKind) {
  return execMemory.allocate(bytes, protection, checkKind);
}

void js::jit::DeallocateExecutableMemory(void* addr, size_t bytes) {
  execMemory.deallocate(addr, bytes,  true);
}

bool js::jit::InitProcessExecutableMemory() { return execMemory.init(); }

void js::jit::ReleaseProcessExecutableMemory() { execMemory.release(); }

size_t js::jit::LikelyAvailableExecutableMemory() {
  return MaxCodeBytesPerProcess -
         AlignBytes(execMemory.bytesAllocated(), 0x100000U);
}

bool js::jit::CanLikelyAllocateMoreExecutableMemory() {
  static const size_t BufferSize = 8 * 1024 * 1024;

  MOZ_ASSERT(execMemory.bytesAllocated() <= MaxCodeBytesPerProcess);

  return execMemory.bytesAllocated() + BufferSize <= MaxCodeBytesPerProcess;
}

bool js::jit::AddressIsInExecutableMemory(const void* p) {
  return execMemory.containsAddress(p);
}

bool js::jit::ReprotectRegion(void* start, size_t size,
                              ProtectionSetting protection,
                              MustFlushICache flushICache) {
#if defined(JS_CODEGEN_WASM32)
  return true;
#endif

  if (flushICache == MustFlushICache::Yes) {
    MOZ_ASSERT(protection == ProtectionSetting::Executable);
    jit::FlushICache(start, size);
  }

  size_t pageSize = gc::SystemPageSize();
  intptr_t startPtr = reinterpret_cast<intptr_t>(start);
  intptr_t pageStartPtr = startPtr & ~(pageSize - 1);
  void* pageStart = reinterpret_cast<void*>(pageStartPtr);
  size += (startPtr - pageStartPtr);

  size += (pageSize - 1);
  size &= ~(pageSize - 1);

  MOZ_ASSERT((uintptr_t(pageStart) % pageSize) == 0);

  execMemory.assertValidAddress(pageStart, size);

#if defined(__wasi__)
  MOZ_CRASH("NYI FOR WASI.");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);

  if (!JitOptions.writeProtectCode) {
    return true;
  }

#if defined(JS_USE_APPLE_FAST_WX)
  MOZ_CRASH("writeProtectCode should always be false on Apple Silicon");
#endif

  unsigned flags = ProtectionSettingToFlags(protection);
  if (mprotect(pageStart, size, flags)) {
    return false;
  }
#endif

  execMemory.assertValidAddress(pageStart, size);
  return true;
}

#if defined(JS_USE_APPLE_FAST_WX) && !0
void js::jit::AutoMarkJitCodeWritableForThread::markExecutable(
    bool executable) {
  if (__builtin_available(macOS 11.0, *)) {
    pthread_jit_write_protect_np(executable);
  } else {
    MOZ_CRASH("pthread_jit_write_protect_np must be available");
  }
}
#endif

#if defined(DEBUG)
static MOZ_THREAD_LOCAL(bool) sMarkingWritable;

void js::jit::AutoMarkJitCodeWritableForThread::checkConstructor() {
  if (!sMarkingWritable.initialized()) {
    sMarkingWritable.infallibleInit();
  }
  MOZ_ASSERT(!sMarkingWritable.get(),
             "AutoMarkJitCodeWritableForThread shouldn't be nested");
  sMarkingWritable.set(true);
}

void js::jit::AutoMarkJitCodeWritableForThread::checkDestructor() {
  MOZ_ASSERT(sMarkingWritable.get());
  sMarkingWritable.set(false);
}
#endif
