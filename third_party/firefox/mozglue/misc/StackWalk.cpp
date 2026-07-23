/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "mozilla/Array.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/StackWalk.h"
#  include <unistd.h>
#include "mozilla/Sprintf.h"

#include <string.h>


using namespace mozilla;

#if defined(HAVE__UNWIND_BACKTRACE) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#if defined(HAVE_DLFCN_H) || 0
#  include <dlfcn.h>
#endif

#  define MOZ_STACKWALK_SUPPORTS_MACOSX 0

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 1)
#  define HAVE___LIBC_STACK_END 1
#else
#  define HAVE___LIBC_STACK_END 0
#endif

#if (defined(linux) &&                                            \
     ((defined(__GNUC__) && (defined(__i386) || defined(PPC))) || \
      defined(HAVE__UNWIND_BACKTRACE)) &&                         \
     (HAVE___LIBC_STACK_END || 0))
#  define MOZ_STACKWALK_SUPPORTS_LINUX 1
#else
#  define MOZ_STACKWALK_SUPPORTS_LINUX 0
#endif

#if HAVE___LIBC_STACK_END
extern MOZ_EXPORT void* __libc_stack_end;  
#if defined(__aarch64__)
static Atomic<uintptr_t> ldso_base;
#endif
#endif


class FrameSkipper {
 public:
  constexpr FrameSkipper() : mSkipUntilAddr(0) {}
  static uintptr_t AddressFromPC(const void* aPC) {
#if defined(__arm__)
    return uintptr_t(aPC) & ~1;
#else
    return uintptr_t(aPC);
#endif
  }
  bool ShouldSkipPC(void* aPC) {
    uintptr_t instructionAddress = AddressFromPC(aPC);
    if (mSkipUntilAddr != 0) {
      if (mSkipUntilAddr != instructionAddress) {
        return true;
      }
      mSkipUntilAddr = 0;
    }
    return false;
  }
  explicit FrameSkipper(const void* aPC) : mSkipUntilAddr(AddressFromPC(aPC)) {}

 private:
  uintptr_t mSkipUntilAddr;
};

#if HAVE_DLADDR &&                                           \
    (HAVE__UNWIND_BACKTRACE || MOZ_STACKWALK_SUPPORTS_LINUX || \
     MOZ_STACKWALK_SUPPORTS_MACOSX)

#  include <stdlib.h>
#  include <stdio.h>

#if (__GLIBC_MINOR__ >= 1) && !defined(__USE_GNU)
#    define __USE_GNU
#endif

#if defined(MOZ_DEMANGLE_SYMBOLS)
#    include <cxxabi.h>
#endif

namespace mozilla {

void DemangleSymbol(const char* aSymbol, char* aBuffer, int aBufLen) {
  aBuffer[0] = '\0';

#if defined(MOZ_DEMANGLE_SYMBOLS)
  char* demangled = abi::__cxa_demangle(aSymbol, nullptr, nullptr, nullptr);

  if (demangled) {
    strncpy(aBuffer, demangled, aBufLen);
    aBuffer[aBufLen - 1] = '\0';
    free(demangled);
  }
#endif
}

}  

#if ((defined(__i386) || defined(PPC) || defined(__ppc__)) && \
       (MOZ_STACKWALK_SUPPORTS_MACOSX || MOZ_STACKWALK_SUPPORTS_LINUX))

static void DoFramePointerStackWalk(MozWalkStackCallback aCallback,
                                    const void* aFirstFramePC,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd);

MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure) {
  void** bp = (void**)__builtin_frame_address(0);

  void* stackEnd;
#if HAVE___LIBC_STACK_END
  stackEnd = __libc_stack_end;
#else
#      error Unsupported configuration
#endif
  DoFramePointerStackWalk(aCallback, aFirstFramePC, aMaxFrames, aClosure, bp,
                          stackEnd);
}

#elif defined(HAVE__UNWIND_BACKTRACE)

#    include <unwind.h>

struct unwind_info {
  MozWalkStackCallback callback;
  FrameSkipper skipper;
  int maxFrames;
  int numFrames;
  void* closure;
};

static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context* context,
                                           void* closure) {
  _Unwind_Reason_Code ret = _URC_NO_REASON;
  unwind_info* info = static_cast<unwind_info*>(closure);
  void* pc = reinterpret_cast<void*>(_Unwind_GetIP(context));
#if HAVE___LIBC_STACK_END && defined(__aarch64__)
  if (!ldso_base) {
    Dl_info info;
    dladdr(&__libc_stack_end, &info);
    ldso_base = (uintptr_t)info.dli_fbase;
  }
  if (ldso_base && ((uintptr_t)pc > ldso_base) &&
      (uintptr_t)pc < (uintptr_t)&__libc_stack_end) {
    ret = _URC_FOREIGN_EXCEPTION_CAUGHT;
  }
#endif
  if (!info->skipper.ShouldSkipPC(pc)) {
    info->numFrames++;
    (*info->callback)(info->numFrames, pc, nullptr, info->closure);
    if (info->maxFrames != 0 && info->numFrames == info->maxFrames) {
      return _URC_FOREIGN_EXCEPTION_CAUGHT;
    }
  }
  return ret;
}

MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure) {
  unwind_info info;
  info.callback = aCallback;
  info.skipper = FrameSkipper(aFirstFramePC ? aFirstFramePC : CallerPC());
  info.maxFrames = aMaxFrames;
  info.numFrames = 0;
  info.closure = aClosure;

  (void)_Unwind_Backtrace(unwind_callback, &info);
}

#endif

bool MFBT_API MozDescribeCodeAddress(void* aPC,
                                     MozCodeAddressDetails* aDetails) {
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;

  Dl_info info;

  int ok = dladdr(aPC, &info);

  if (!ok) {
    return true;
  }

  strncpy(aDetails->library, info.dli_fname, sizeof(aDetails->library));
  aDetails->library[std::size(aDetails->library) - 1] = '\0';
  aDetails->loffset = (char*)aPC - (char*)info.dli_fbase;


  const char* symbol = info.dli_sname;
  if (!symbol || symbol[0] == '\0') {
    return true;
  }

  DemangleSymbol(symbol, aDetails->function, sizeof(aDetails->function));

  if (aDetails->function[0] == '\0') {
    strncpy(aDetails->function, symbol, sizeof(aDetails->function));
    aDetails->function[std::size(aDetails->function) - 1] = '\0';
  }

  aDetails->foffset = (char*)aPC - (char*)info.dli_saddr;

  return true;
}

#else

MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure) {}

MFBT_API bool MozDescribeCodeAddress(void* aPC,
                                     MozCodeAddressDetails* aDetails) {
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;
  return false;
}

#endif

#if 0 || 0 || defined(XP_LINUX)

const uintptr_t kPointerMask = ~uintptr_t(0);

MOZ_ASAN_IGNORE
static void DoFramePointerStackWalk(MozWalkStackCallback aCallback,
                                    const void* aFirstFramePC,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd) {

  FrameSkipper skipper(aFirstFramePC);
  uint32_t numFrames = 0;

  static const uintptr_t kMaxStackSize = 8 * 1024 * 1024;
  if (uintptr_t(aBp) < uintptr_t(aStackEnd) -
                           std::min(kMaxStackSize, uintptr_t(aStackEnd)) ||
      aBp >= aStackEnd || (uintptr_t(aBp) & (sizeof(void*) - 1))) {
    return;
  }

  while (aBp) {
    void** next = (void**)*aBp;
    if (next <= aBp || next >= aStackEnd ||
        (uintptr_t(next) & (sizeof(void*) - 1))) {
      break;
    }
#if (defined(__ppc__) && 0) || defined(__powerpc64__)
    void* pc = *(aBp + 2);
    aBp += 3;
#else
    void* pc = *(aBp + 1);
    aBp += 2;
#endif

    pc = (void*)((uintptr_t)pc & kPointerMask);

    if (!skipper.ShouldSkipPC(pc)) {
      numFrames++;
      (*aCallback)(numFrames, pc, aBp, aClosure);
      if (aMaxFrames != 0 && numFrames == aMaxFrames) {
        break;
      }
    }
    aBp = next;
  }
}

namespace mozilla {

MFBT_API void FramePointerStackWalk(MozWalkStackCallback aCallback,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd) {
  DoFramePointerStackWalk(aCallback, nullptr, aMaxFrames, aClosure, aBp,
                          aStackEnd);
}

}  

#else

namespace mozilla {
MFBT_API void FramePointerStackWalk(MozWalkStackCallback aCallback,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd) {}
}  

#endif

MFBT_API int MozFormatCodeAddressDetails(
    char* aBuffer, uint32_t aBufferSize, uint32_t aFrameNumber, void* aPC,
    const MozCodeAddressDetails* aDetails) {
  return MozFormatCodeAddress(aBuffer, aBufferSize, aFrameNumber, aPC,
                              aDetails->function, aDetails->library,
                              aDetails->loffset, aDetails->filename,
                              aDetails->lineno);
}

MFBT_API int MozFormatCodeAddress(char* aBuffer, uint32_t aBufferSize,
                                  uint32_t aFrameNumber, const void* aPC,
                                  const char* aFunction, const char* aLibrary,
                                  ptrdiff_t aLOffset, const char* aFileName,
                                  uint32_t aLineNo) {
  const char* function = aFunction && aFunction[0] ? aFunction : "???";
  if (aFileName && aFileName[0]) {
    return SprintfBuf(aBuffer, aBufferSize, "#%02u: %s (%s:%u)", aFrameNumber,
                      function, aFileName, aLineNo);
  } else if (aLibrary && aLibrary[0]) {
    return SprintfBuf(aBuffer, aBufferSize, "#%02u: %s[%s +0x%" PRIxPTR "]",
                      aFrameNumber, function, aLibrary,
                      static_cast<uintptr_t>(aLOffset));
  } else {
    return SprintfBuf(aBuffer, aBufferSize,
                      "#%02u: ??? (???:???"
                      ")",
                      aFrameNumber);
  }
}

static void EnsureWrite(FILE* aStream, const char* aBuf, size_t aLen) {
  int fd = fileno(aStream);
  while (aLen > 0) {
    auto written = write(fd, aBuf, aLen);
    if (written <= 0 || size_t(written) > aLen) {
      break;
    }
    aBuf += written;
    aLen -= written;
  }
}

template <int N>
static int PrintStackFrameBuf(char (&aBuf)[N], uint32_t aFrameNumber, void* aPC,
                              void* aSP) {
  MozCodeAddressDetails details;
  MozDescribeCodeAddress(aPC, &details);
  int len =
      MozFormatCodeAddressDetails(aBuf, N - 1, aFrameNumber, aPC, &details);
  len = std::min(len, N - 2);
  aBuf[len++] = '\n';
  aBuf[len] = '\0';
  return len;
}

static void PrintStackFrame(uint32_t aFrameNumber, void* aPC, void* aSP,
                            void* aClosure) {
  FILE* stream = (FILE*)aClosure;
  char buf[1025];  
  int len = PrintStackFrameBuf(buf, aFrameNumber, aPC, aSP);
  fflush(stream);
  EnsureWrite(stream, buf, len);
}

static bool WalkTheStackEnabled() {
  static bool result = [] {
    char* value = getenv("MOZ_DISABLE_WALKTHESTACK");
    return !(value && value[0]);
  }();
  return result;
}

MFBT_API void MozWalkTheStack(FILE* aStream, const void* aFirstFramePC,
                              uint32_t aMaxFrames) {
  if (WalkTheStackEnabled()) {
    MozStackWalk(PrintStackFrame, aFirstFramePC ? aFirstFramePC : CallerPC(),
                 aMaxFrames, aStream);
  }
}

static void WriteStackFrame(uint32_t aFrameNumber, void* aPC, void* aSP,
                            void* aClosure) {
  auto writer = (void (*)(const char*))aClosure;
  char buf[1024];
  PrintStackFrameBuf(buf, aFrameNumber, aPC, aSP);
  writer(buf);
}

MFBT_API void MozWalkTheStackWithWriter(void (*aWriter)(const char*),
                                        const void* aFirstFramePC,
                                        uint32_t aMaxFrames) {
  if (WalkTheStackEnabled()) {
    MozStackWalk(WriteStackFrame, aFirstFramePC ? aFirstFramePC : CallerPC(),
                 aMaxFrames, (void*)aWriter);
  }
}
