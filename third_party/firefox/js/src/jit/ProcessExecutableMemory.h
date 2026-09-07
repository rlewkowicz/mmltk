/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(jit_ProcessExecutableMemory_h)
#define jit_ProcessExecutableMemory_h

#include "util/Poison.h"


namespace js {
namespace jit {

#if JS_BITS_PER_WORD == 32
static const size_t MaxCodeBytesPerProcess = 140 * 1024 * 1024;
#else
static const size_t MaxCodeBytesPerProcess = 2044 * 1024 * 1024;
#endif


#if defined(JS_CODEGEN_ARM)
static const size_t MaxCodeBytesPerBuffer = (1 << 25) - 4;
#elif defined(JS_CODEGEN_ARM64)
static const size_t MaxCodeBytesPerBuffer = (1 << 27) - 4;
#else
static const size_t MaxCodeBytesPerBuffer = MaxCodeBytesPerProcess;
#endif

static const size_t ExecutableCodePageSize = 64 * 1024;

enum class ProtectionSetting {
  Writable,
  Executable,
};


enum class MustFlushICache { No, Yes };

[[nodiscard]] extern bool ReprotectRegion(void* start, size_t size,
                                          ProtectionSetting protection,
                                          MustFlushICache flushICache);

[[nodiscard]] extern bool InitProcessExecutableMemory();
extern void ReleaseProcessExecutableMemory();

extern void* AllocateExecutableMemory(size_t bytes,
                                      ProtectionSetting protection,
                                      MemCheckKind checkKind);
extern void DeallocateExecutableMemory(void* addr, size_t bytes);

extern bool CanLikelyAllocateMoreExecutableMemory();

extern size_t LikelyAvailableExecutableMemory();

extern bool AddressIsInExecutableMemory(const void* p);

class MOZ_RAII AutoMarkJitCodeWritableForThread {
#if defined(DEBUG)
  void checkConstructor();
  void checkDestructor();
#else
  void checkConstructor() {}
  void checkDestructor() {}
#endif

#if defined(JS_USE_APPLE_FAST_WX) && !0
  void markExecutable(bool executable);
#endif

 public:
  MOZ_ALWAYS_INLINE_EVEN_DEBUG AutoMarkJitCodeWritableForThread() {
#if defined(JS_USE_APPLE_FAST_WX)
    markExecutable(false);
#endif
    checkConstructor();
  }
  MOZ_ALWAYS_INLINE_EVEN_DEBUG ~AutoMarkJitCodeWritableForThread() {
#if defined(JS_USE_APPLE_FAST_WX)
    markExecutable(true);
#endif
    checkDestructor();
  }
};

}  
}  

#endif
