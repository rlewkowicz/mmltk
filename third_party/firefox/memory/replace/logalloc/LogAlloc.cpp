/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <cstdlib>
#include <fcntl.h>

#  include <unistd.h>
#  include <pthread.h>

#include "replace_malloc.h"
#include "FdPrintf.h"
#include "Mutex.h"

static malloc_table_t sFuncs;
static platform_handle_t sFd;
static bool sStdoutOrStderr = false;

static Mutex sMutex MOZ_UNANNOTATED;

static void prefork() MOZ_NO_THREAD_SAFETY_ANALYSIS { sMutex.Lock(); }
static void postfork_parent() MOZ_NO_THREAD_SAFETY_ANALYSIS { sMutex.Unlock(); }
static void postfork_child() { sMutex.Init(); }

static size_t GetPid() { return size_t(getpid()); }

static size_t GetTid() {
  return size_t(pthread_self());
}


class LogAllocBridge : public ReplaceMallocBridge {
  virtual void InitDebugFd(mozilla::DebugFdRegistry& aRegistry) override {
    if (!sStdoutOrStderr) {
      aRegistry.RegisterHandle(sFd);
    }
  }
};


static void* replace_malloc(size_t aSize) {
  void* ptr = sFuncs.malloc(aSize);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu malloc(%zu)=%p\n", GetPid(), GetTid(), aSize, ptr);
  return ptr;
}

static int replace_posix_memalign(void** aPtr, size_t aAlignment,
                                  size_t aSize) {
  int ret = sFuncs.posix_memalign(aPtr, aAlignment, aSize);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu posix_memalign(%zu,%zu)=%p\n", GetPid(), GetTid(),
           aAlignment, aSize, (ret == 0) ? *aPtr : nullptr);
  return ret;
}

static void* replace_aligned_alloc(size_t aAlignment, size_t aSize) {
  void* ptr = sFuncs.aligned_alloc(aAlignment, aSize);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu aligned_alloc(%zu,%zu)=%p\n", GetPid(), GetTid(),
           aAlignment, aSize, ptr);
  return ptr;
}

static void* replace_calloc(size_t aNum, size_t aSize) {
  void* ptr = sFuncs.calloc(aNum, aSize);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu calloc(%zu,%zu)=%p\n", GetPid(), GetTid(), aNum, aSize,
           ptr);
  return ptr;
}

static void* replace_realloc(void* aPtr, size_t aSize) {
  void* new_ptr = sFuncs.realloc(aPtr, aSize);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu realloc(%p,%zu)=%p\n", GetPid(), GetTid(), aPtr, aSize,
           new_ptr);
  return new_ptr;
}

static void replace_free(void* aPtr) {
  {
    MutexAutoLock lock(sMutex);
    FdPrintf(sFd, "%zu %zu free(%p)\n", GetPid(), GetTid(), aPtr);
  }
  sFuncs.free(aPtr);
}

static void* replace_memalign(size_t aAlignment, size_t aSize) {
  void* ptr = sFuncs.memalign(aAlignment, aSize);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu memalign(%zu,%zu)=%p\n", GetPid(), GetTid(),
           aAlignment, aSize, ptr);
  return ptr;
}

static void* replace_valloc(size_t aSize) {
  void* ptr = sFuncs.valloc(aSize);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu valloc(%zu)=%p\n", GetPid(), GetTid(), aSize, ptr);
  return ptr;
}

static void replace_jemalloc_stats(jemalloc_stats_t* aStats,
                                   jemalloc_bin_stats_t* aBinStats) {
  sFuncs.jemalloc_stats_internal(aStats, aBinStats);
  MutexAutoLock lock(sMutex);
  FdPrintf(sFd, "%zu %zu jemalloc_stats()\n", GetPid(), GetTid());
}

void replace_init(malloc_table_t* aTable, ReplaceMallocBridge** aBridge) {
  char* log = getenv("MALLOC_LOG");
  if (log && *log) {
    int fd = 0;
    const auto* fd_num = log;
    while (*fd_num) {
      if (*fd_num < '0' || *fd_num > '9') {
        fd = -1;
        break;
      }
      fd = fd * 10 + (*fd_num - '0');
      if (fd >= 10000) {
        fd = -1;
        break;
      }
      fd_num++;
    }
    if (fd == 1 || fd == 2) {
      sStdoutOrStderr = true;
    }
    if (fd == -1) {
      fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (fd > 0) {
      sFd = fd;
    }
  }

  if (!sFd) {
    return;
  }

  sMutex.Init();
  static LogAllocBridge bridge;
  sFuncs = *aTable;
#define MALLOC_FUNCS MALLOC_FUNCS_MALLOC_BASE
#define MALLOC_DECL(name, ...) aTable->name = replace_##name;
#include "malloc_decls.h"
  aTable->jemalloc_stats_internal = replace_jemalloc_stats;
  if (!getenv("MALLOC_LOG_MINIMAL")) {
    aTable->posix_memalign = replace_posix_memalign;
    aTable->aligned_alloc = replace_aligned_alloc;
    aTable->valloc = replace_valloc;
  }
  *aBridge = &bridge;

  sFuncs.malloc(-1);
  pthread_atfork(prefork, postfork_parent, postfork_child);
}
