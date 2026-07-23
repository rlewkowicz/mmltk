/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cerrno>
#include <cstdio>

#  include <sys/mman.h>
#  include <unistd.h>

#include "mozjemalloc_types.h"

#include "Mutex.h"
#include "Chunk.h"
#include "Extent.h"
#include "Globals.h"
#include "RedBlackTree.h"

#include "mozilla/Assertions.h"
#include "mozilla/HelperMacros.h"
#include "mozilla/TaggedAnonymousMemory.h"
#include "mozilla/ThreadSafety.h"


using namespace mozilla;



#if (defined(XP_LINUX) && !defined(__alpha__)) || \
    (defined(__FreeBSD_kernel__) && defined(__GLIBC__))
#  include <sys/syscall.h>
#if defined(SYS_mmap) || defined(SYS_mmap2)
static inline void* _mmap(void* addr, size_t length, int prot, int flags,
                          int fd, off_t offset) {
#if defined(__s390__)
  struct {
    void* addr;
    size_t length;
    long prot;
    long flags;
    long fd;
    off_t offset;
  } args = {addr, length, prot, flags, fd, offset};
  return (void*)syscall(SYS_mmap, &args);
#else
#if defined(SYS_mmap2)
  return (void*)syscall(SYS_mmap2, addr, length, prot, flags, fd, offset >> 12);
#else
  return (void*)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
#endif
#endif
}
#    define mmap _mmap
#    define munmap(a, l) syscall(SYS_munmap, a, l)
#endif
#endif


void pages_unmap(void* aAddr, size_t aSize) {
  if (munmap(aAddr, aSize) == -1) {
    char buf[64];

    if (!strerror_r(errno, buf, sizeof(buf))) {
      _malloc_message(_getprogname(), ": (malloc) Error in munmap(): ", buf,
                      "\n");
    }
  }
}

static void* pages_map(void* aAddr, size_t aSize, ShouldCommit should_commit) {
  void* ret;
#if defined(__ia64__) || \
      (defined(__sparc__) && defined(__arch64__) && defined(__linux__))
  bool check_placement = true;
  if (!aAddr) {
    aAddr = (void*)0x0000070000000000;
    check_placement = false;
  }
#endif

#if defined(__sparc__) && defined(__arch64__) && defined(__linux__)
  const uintptr_t start = 0x0000070000000000ULL;
  const uintptr_t end = 0x0000800000000000ULL;

  uintptr_t hint;
  void* region = MAP_FAILED;
  for (hint = start; region == MAP_FAILED && hint + aSize <= end;
       hint += kChunkSize) {
    region = mmap((void*)hint, aSize,
                  should_commit ? PROT_READ | PROT_WRITE : PROT_NONE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region != MAP_FAILED) {
      if (((size_t)region + (aSize - 1)) & 0xffff800000000000) {
        if (munmap(region, aSize)) {
          MOZ_ASSERT(errno == ENOMEM);
        }
        region = MAP_FAILED;
      }
    }
  }
  ret = region;
#else
  ret = mmap(
      aAddr, aSize,
      should_commit == ReserveAndCommit ? PROT_READ | PROT_WRITE : PROT_NONE,
      MAP_PRIVATE | MAP_ANON, -1, 0);
  MOZ_ASSERT(ret);
#endif
  if (ret == MAP_FAILED) {
    ret = nullptr;
  }
#if defined(__ia64__) || \
      (defined(__sparc__) && defined(__arch64__) && defined(__linux__))
  else if ((long long)ret & 0xffff800000000000) {
    munmap(ret, aSize);
    ret = nullptr;
  }
  else if (check_placement && ret != aAddr) {
#else
  else if (aAddr && ret != aAddr) {
#endif
    pages_unmap(ret, aSize);
    ret = nullptr;
  }
  if (ret) {
    MozTagAnonymousMemory(ret, aSize, "jemalloc");
  }

#if defined(__ia64__) || \
      (defined(__sparc__) && defined(__arch64__) && defined(__linux__))
  MOZ_ASSERT(!ret || (!check_placement && ret) ||
             (check_placement && ret == aAddr));
#else
  MOZ_ASSERT(!ret || (!aAddr && ret != aAddr) || (aAddr && ret == aAddr));
#endif
  return ret;
}


void pages_decommit(void* aAddr, size_t aSize) {
  if (mmap(aAddr, aSize, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1,
           0) == MAP_FAILED) {
    const char out_of_mappings[] =
        "[unhandlable oom] Failed to mmap, likely no more mappings "
        "available " __FILE__ " : " MOZ_STRINGIFY(__LINE__);
    if (errno == ENOMEM) {
      fputs(out_of_mappings, stderr);
      fflush(stderr);
      MOZ_CRASH_ANNOTATE(out_of_mappings);
    }
    MOZ_REALLY_CRASH(__LINE__);
  }
  MozTagAnonymousMemory(aAddr, aSize, "jemalloc-decommitted");
}

[[nodiscard]] bool pages_commit(void* aAddr, size_t aSize) {
  if (mmap(aAddr, aSize, PROT_READ | PROT_WRITE,
           MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
    return false;
  }
  MozTagAnonymousMemory(aAddr, aSize, "jemalloc");
  return true;
}

static void pages_purge(void* addr, size_t length) {
  pages_decommit(addr, length);
}


static void* pages_trim(void* addr, size_t alloc_size, size_t leadsize,
                        size_t size, ShouldCommit should_commit) {
  void* ret = (void*)((uintptr_t)addr + leadsize);

  MOZ_ASSERT(alloc_size >= leadsize + size);
  {
    size_t trailsize = alloc_size - leadsize - size;

    if (leadsize != 0) {
      pages_unmap(addr, leadsize);
    }
    if (trailsize != 0) {
      pages_unmap((void*)((uintptr_t)ret + size), trailsize);
    }
    return ret;
  }
}

static void* pages_mmap_aligned_slow(size_t size, size_t alignment,
                                     ShouldCommit should_commit) {
  void *ret, *pages;
  size_t alloc_size, leadsize;

  alloc_size = size + alignment - gRealPageSize;
  if (alloc_size < size) {
    return nullptr;
  }
  do {
    pages = pages_map(nullptr, alloc_size, should_commit);
    if (!pages) {
      return nullptr;
    }
    leadsize =
        ALIGNMENT_CEILING((uintptr_t)pages, alignment) - (uintptr_t)pages;
    ret = pages_trim(pages, alloc_size, leadsize, size, should_commit);
  } while (!ret);

  MOZ_ASSERT(ret);
  return ret;
}

void* pages_mmap_aligned(size_t size, size_t alignment,
                         ShouldCommit should_commit) {
  void* ret;
  size_t offset;

  ret = pages_map(nullptr, size, should_commit);
  if (!ret) {
    return nullptr;
  }
  offset = ALIGNMENT_ADDR2OFFSET(ret, alignment);
  if (offset != 0) {
    pages_unmap(ret, size);
    return pages_mmap_aligned_slow(size, alignment, should_commit);
  }

  MOZ_ASSERT(ret);
  return ret;
}

constinit AddressRadixTree<(sizeof(void*) << 3) - LOG2(kChunkSize)> gChunkRTree;

static Mutex chunks_mtx;

static RedBlackTree<extent_node_t, ExtentTreeSzTrait> gChunksBySize
    MOZ_GUARDED_BY(chunks_mtx);
static RedBlackTree<extent_node_t, ExtentTreeTrait> gChunksByAddress
    MOZ_GUARDED_BY(chunks_mtx);

Atomic<size_t> gRecycledSize;

void chunks_init() {
  chunks_mtx.Init();
}

#  define CAN_RECYCLE(size) true

#if defined(MOZ_DEBUG)
void chunk_assert_zero(void* aPtr, size_t aSize) {
#if defined(MALLOC_DEBUG_VIGILANT)
  size_t i;
  size_t* p = (size_t*)(uintptr_t)aPtr;

  for (i = 0; i < aSize / sizeof(size_t); i++) {
    MOZ_ASSERT(p[i] == 0);
  }
#endif
}
#endif

static void chunk_record(void* aChunk, size_t aSize, ChunkType aType) {
  if (aType != ZEROED_CHUNK) {
    pages_purge(aChunk, aSize);
    aType = ZEROED_CHUNK;
  }

  UniqueBaseNode xnode(new (fallible) extent_node_t());
  UniqueBaseNode xprev;

  MutexAutoLock lock(chunks_mtx);
  void* addr = (void*)((uintptr_t)aChunk + aSize);
  extent_node_t* node = gChunksByAddress.SearchOrNext(addr);
  if (node && node->mAddr == addr) {
    gChunksBySize.Remove(node);
    node->mAddr = aChunk;
    node->mSize += aSize;
    if (node->mChunkType != aType) {
      node->mChunkType = RECYCLED_CHUNK;
    }
    gChunksBySize.Insert(node);
  } else {
    if (!xnode) {
      return;
    }
    node = xnode.release();
    node->mAddr = aChunk;
    node->mSize = aSize;
    node->mChunkType = aType;
    gChunksByAddress.Insert(node);
    gChunksBySize.Insert(node);
  }

  extent_node_t* prev = gChunksByAddress.Prev(node);
  if (prev && (void*)((uintptr_t)prev->mAddr + prev->mSize) == aChunk) {
    gChunksBySize.Remove(prev);
    gChunksByAddress.Remove(prev);

    gChunksBySize.Remove(node);
    node->mAddr = prev->mAddr;
    node->mSize += prev->mSize;
    if (node->mChunkType != prev->mChunkType) {
      node->mChunkType = RECYCLED_CHUNK;
    }
    gChunksBySize.Insert(node);

    xprev.reset(prev);
  }

  gRecycledSize += aSize;
}

void base_chunk_dealloc(void* aChunk, size_t aSize, ChunkType aType) {
  MOZ_ASSERT(aChunk);
  MOZ_ASSERT(GetChunkOffsetForPtr(aChunk) == 0);
  MOZ_ASSERT(aSize != 0);
  MOZ_ASSERT((aSize & kChunkSizeMask) == 0);
  MOZ_ASSERT(!gChunkRTree.Get(aChunk));

  if (CAN_RECYCLE(aSize)) {
    size_t recycled_so_far = gRecycledSize;
    if (recycled_so_far < gRecycleLimit) {
      size_t recycle_remaining = gRecycleLimit - recycled_so_far;
      size_t to_recycle;
      if (aSize > recycle_remaining) {
        to_recycle = recycle_remaining;
        pages_trim(aChunk, aSize, 0, to_recycle, ReserveAndCommit);
      } else {
        to_recycle = aSize;
      }
      chunk_record(aChunk, to_recycle, aType);
      return;
    }
  }

  pages_unmap(aChunk, aSize);
}

void arena_chunk_dealloc(chunk_allocator_t* aChunkAllocator, void* aChunk,
                         size_t aSize) {
  MOZ_ASSERT(aChunk);
  MOZ_ASSERT(GetChunkOffsetForPtr(aChunk) == 0);
  MOZ_ASSERT(aSize != 0);
  MOZ_ASSERT((aSize & kChunkSizeMask) == 0);

  gChunkRTree.Unset(aChunk);

  aChunkAllocator->unmap(aChunk, aSize);
}

static void* chunk_recycle(size_t aSize, size_t aAlignment) {
  size_t alloc_size = aSize + aAlignment - kChunkSize;
  if (alloc_size < aSize) {
    return nullptr;
  }
  chunks_mtx.Lock();
  extent_node_t* node = gChunksBySize.SearchOrNext(alloc_size);
  if (!node) {
    chunks_mtx.Unlock();
    return nullptr;
  }
  size_t leadsize = ALIGNMENT_CEILING((uintptr_t)node->mAddr, aAlignment) -
                    (uintptr_t)node->mAddr;
  MOZ_ASSERT(node->mSize >= leadsize + aSize);
  size_t trailsize = node->mSize - leadsize - aSize;
  void* ret = (void*)((uintptr_t)node->mAddr + leadsize);

  MOZ_ASSERT(node->mChunkType == ZEROED_CHUNK);

  gChunksBySize.Remove(node);
  gChunksByAddress.Remove(node);
  if (leadsize != 0) {
    node->mSize = leadsize;
    gChunksBySize.Insert(node);
    gChunksByAddress.Insert(node);
    node = nullptr;
  }
  if (trailsize != 0) {
    if (!node) {
      chunks_mtx.Unlock();
      node = new (fallible) extent_node_t();
      if (!node) {
        base_chunk_dealloc(ret, aSize, ZEROED_CHUNK);
        return nullptr;
      }
      chunks_mtx.Lock();
    }
    node->mAddr = (void*)((uintptr_t)(ret) + aSize);
    node->mSize = trailsize;
    node->mChunkType = ZEROED_CHUNK;
    gChunksBySize.Insert(node);
    gChunksByAddress.Insert(node);
    node = nullptr;
  }

  gRecycledSize -= aSize;

  chunks_mtx.Unlock();

  if (node) {
    delete node;
  }
  if (!pages_commit(ret, aSize)) {
    return nullptr;
  }

  return ret;
}

void* base_chunk_alloc(size_t aSize, size_t aAlignment) {
  MOZ_ASSERT(aSize != 0);
  MOZ_ASSERT((aSize & kChunkSizeMask) == 0);
  MOZ_ASSERT(aAlignment != 0);
  MOZ_ASSERT((aAlignment & kChunkSizeMask) == 0);

  void* ret = pages_mmap_aligned(aSize, aAlignment, ReserveAndCommit);
  MOZ_ASSERT(GetChunkOffsetForPtr(ret) == 0);

  return ret;
}

void* arena_chunk_alloc(chunk_allocator_t* aChunkAllocator, size_t aSize,
                        size_t aAlignment) {
  MOZ_ASSERT(aSize != 0);
  MOZ_ASSERT((aSize & kChunkSizeMask) == 0);
  MOZ_ASSERT(aAlignment != 0);
  MOZ_ASSERT((aAlignment & kChunkSizeMask) == 0);

  void* ret = aChunkAllocator->map(aSize, aAlignment);
  if (ret) {
    if (!gChunkRTree.Set(ret, ret)) {
      aChunkAllocator->unmap(ret, aSize);
      return nullptr;
    }
  }

  MOZ_ASSERT(GetChunkOffsetForPtr(ret) == 0);
  return ret;
}

static void* system_pages_map(size_t aSize, size_t aAlignment) {
  void* ret = nullptr;

  if (CAN_RECYCLE(aSize)) {
    ret = chunk_recycle(aSize, aAlignment);
  }
  if (!ret) {
    ret = pages_mmap_aligned(aSize, aAlignment, ReserveAndCommit);
  }

  return ret;
}

static void system_pages_unmap(void* aAddr, size_t aSize) {
  base_chunk_dealloc(aAddr, aSize, ARENA_CHUNK);
}

chunk_allocator_t gSystemChunkAllocator{
    .map = system_pages_map,
    .unmap = system_pages_unmap,
    .commit = pages_commit,
    .decommit = pages_decommit,
};

arena_chunk_t::arena_chunk_t(arena_t* aArena)
    : mArena(aArena), mDirtyRunHint(gChunkHeaderNumPages) {}

bool arena_chunk_t::IsEmpty() {
  return (mPageMap[gChunkHeaderNumPages].bits &
          (~gPageSizeMask | CHUNK_MAP_ALLOCATED)) == gMaxLargeClass;
}
