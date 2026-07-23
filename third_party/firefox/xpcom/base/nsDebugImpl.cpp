/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/process_util.h"

#include "mozilla/Atomics.h"
#include "mozilla/IntentionalCrash.h"
#include "mozilla/Printf.h"
#include "MainThreadUtils.h"
#include "nsDebugImpl.h"
#include "nsDebug.h"
#include "nsString.h"
#include "nsXULAppAPI.h"
#include "prerror.h"
#include "prerr.h"
#include "prenv.h"



#include "mozilla/StackWalk.h"

#if defined(XP_UNIX)
#  include <signal.h>
#endif





#  define KINFO_PROC struct kinfo_proc

#  define KP_FLAGS p_flag

static void Abort(const char* aMsg);

static void RealBreak();

static void Break(const char* aMsg);


using namespace mozilla;

static const char* sMultiprocessDescription = nullptr;

static Atomic<int32_t> gAssertionCount;

NS_IMPL_QUERY_INTERFACE(nsDebugImpl, nsIDebug2)

NS_IMETHODIMP_(MozExternalRefCountType)
nsDebugImpl::AddRef() { return 2; }

NS_IMETHODIMP_(MozExternalRefCountType)
nsDebugImpl::Release() { return 1; }

NS_IMETHODIMP
nsDebugImpl::Assertion(const char* aStr, const char* aExpr, const char* aFile,
                       int32_t aLine) {
  NS_DebugBreak(NS_DEBUG_ASSERTION, aStr, aExpr, aFile, aLine);
  return NS_OK;
}

NS_IMETHODIMP
nsDebugImpl::Warning(const char* aStr, const char* aFile, int32_t aLine) {
  NS_DebugBreak(NS_DEBUG_WARNING, aStr, nullptr, aFile, aLine);
  return NS_OK;
}

NS_IMETHODIMP
nsDebugImpl::Break(const char* aFile, int32_t aLine) {
  NS_DebugBreak(NS_DEBUG_BREAK, nullptr, nullptr, aFile, aLine);
  return NS_OK;
}

NS_IMETHODIMP
nsDebugImpl::Abort(const char* aFile, int32_t aLine) {
  NS_DebugBreak(NS_DEBUG_ABORT, nullptr, nullptr, aFile, aLine);
  return NS_OK;
}

NS_IMETHODIMP
nsDebugImpl::CrashWithOOM() {
  NS_ABORT_OOM(-1);
  return NS_OK;
}

extern "C" void intentional_panic(const char* message);

NS_IMETHODIMP
nsDebugImpl::RustPanic(const char* aMessage) {
  intentional_panic(aMessage);
  return NS_OK;
}

extern "C" void debug_log(const char* target, const char* message);

NS_IMETHODIMP
nsDebugImpl::RustLog(const char* aTarget, const char* aMessage) {
  debug_log(aTarget, aMessage);
  return NS_OK;
}

NS_IMETHODIMP
nsDebugImpl::GetIsDebugBuild(bool* aResult) {
#if defined(DEBUG)
  *aResult = true;
#else
  *aResult = false;
#endif
  return NS_OK;
}

NS_IMETHODIMP
nsDebugImpl::GetAssertionCount(int32_t* aResult) {
  *aResult = gAssertionCount;
  return NS_OK;
}

NS_IMETHODIMP
nsDebugImpl::GetIsDebuggerAttached(bool* aResult) {
  *aResult = false;


  return NS_OK;
}

void nsDebugImpl::SetMultiprocessMode(const char* aDesc) {
  sMultiprocessDescription = aDesc;
}

 const char* nsDebugImpl::GetMultiprocessMode() {
  return sMultiprocessDescription;
}

enum nsAssertBehavior {
  NS_ASSERT_UNINITIALIZED,
  NS_ASSERT_WARN,
  NS_ASSERT_SUSPEND,
  NS_ASSERT_STACK,
  NS_ASSERT_TRAP,
  NS_ASSERT_ABORT,
  NS_ASSERT_STACK_AND_ABORT
};

static nsAssertBehavior GetAssertBehavior() {
  static nsAssertBehavior gAssertBehavior = NS_ASSERT_UNINITIALIZED;
  if (gAssertBehavior != NS_ASSERT_UNINITIALIZED) {
    return gAssertBehavior;
  }

  gAssertBehavior = NS_ASSERT_WARN;

  const char* assertString = PR_GetEnv("XPCOM_DEBUG_BREAK");
  if (!assertString || !*assertString) {
    return gAssertBehavior;
  }
  if (!strcmp(assertString, "warn")) {
    return gAssertBehavior = NS_ASSERT_WARN;
  }
  if (!strcmp(assertString, "suspend")) {
    return gAssertBehavior = NS_ASSERT_SUSPEND;
  }
  if (!strcmp(assertString, "stack")) {
    return gAssertBehavior = NS_ASSERT_STACK;
  }
  if (!strcmp(assertString, "abort")) {
    return gAssertBehavior = NS_ASSERT_ABORT;
  }
  if (!strcmp(assertString, "trap") || !strcmp(assertString, "break")) {
    return gAssertBehavior = NS_ASSERT_TRAP;
  }
  if (!strcmp(assertString, "stack-and-abort")) {
    return gAssertBehavior = NS_ASSERT_STACK_AND_ABORT;
  }

  fprintf(stderr, "Unrecognized value of XPCOM_DEBUG_BREAK\n");
  return gAssertBehavior;
}

struct FixedBuffer final : public mozilla::PrintfTarget {
  FixedBuffer() : curlen(0) { buffer[0] = '\0'; }

  char buffer[764];
  uint32_t curlen;

  bool append(const char* sp, size_t len) override;
};

bool FixedBuffer::append(const char* aBuf, size_t aLen) {
  if (!aLen) {
    return true;
  }

  if (curlen + aLen >= sizeof(buffer)) {
    aLen = sizeof(buffer) - curlen - 1;
  }

  if (aLen) {
    memcpy(buffer + curlen, aBuf, aLen);
    curlen += aLen;
    buffer[curlen] = '\0';
  }

  return true;
}

static void NS_PrintStackTrace() {
  MozWalkTheStack(stderr);
}

EXPORT_XPCOM_API(void)
NS_DebugBreak(uint32_t aSeverity, const char* aStr, const char* aExpr,
              const char* aFile, int32_t aLine) {
  aFile = MOZ_StripRelativeComponents(aFile);
  FixedBuffer nonPIDBuf;
  FixedBuffer buf;
  const char* sevString = "WARNING";

  switch (aSeverity) {
    case NS_DEBUG_ASSERTION:
      sevString = "###!!! ASSERTION";
      break;

    case NS_DEBUG_BREAK:
      sevString = "###!!! BREAK";
      break;

    case NS_DEBUG_ABORT:
      sevString = "###!!! ABORT";
      break;

    default:
      aSeverity = NS_DEBUG_WARNING;
  }

  nonPIDBuf.print("%s: ", sevString);
  if (aStr) {
    nonPIDBuf.print("%s: ", aStr);
  }
  if (aExpr) {
    nonPIDBuf.print("'%s', ", aExpr);
  }
  if (aFile || aLine != -1) {
    nonPIDBuf.print("file %s:%d", aFile ? aFile : "<unknown>",
                    aLine != -1 ? aLine : 0);
  }

  buf.print("[");
  if (sMultiprocessDescription) {
    buf.print("%s ", sMultiprocessDescription);
  }

  bool isMainthread = (NS_IsMainThreadTLSInitialized() && NS_IsMainThread());
  PRThread* currentThread = PR_GetCurrentThread();
  const char* currentThreadName =
      isMainthread ? "Main Thread" : PR_GetThreadName(currentThread);
  if (currentThreadName) {
    buf.print("%" PRIPID ", %s] %s", base::GetCurrentProcId(),
              currentThreadName, nonPIDBuf.buffer);
  } else {
    buf.print("%" PRIPID ", Unnamed thread %p] %s", base::GetCurrentProcId(),
              currentThread, nonPIDBuf.buffer);
  }

  if (aSeverity != NS_DEBUG_WARNING) {
    fprintf(stderr, "\07");
  }



  if (!(PR_GetEnv("MOZ_IGNORE_WARNINGS") && aSeverity == NS_DEBUG_WARNING)) {
    fprintf(stderr, "%s\n", buf.buffer);
    fflush(stderr);
  }

  switch (aSeverity) {
    case NS_DEBUG_WARNING:
      return;

    case NS_DEBUG_BREAK:
      Break(buf.buffer);
      return;

    case NS_DEBUG_ABORT: {
      if (XRE_IsParentProcess()) {
        nsAutoCString note("xpcom_runtime_abort(");
        note += nonPIDBuf.buffer;
        note += ")";
      }

#if defined(DEBUG)
      NS_PrintStackTrace();
#endif
      Abort(buf.buffer);
      return;
    }
  }

  gAssertionCount++;

  switch (GetAssertBehavior()) {
    case NS_ASSERT_WARN:
      return;

    case NS_ASSERT_SUSPEND:
#if defined(XP_UNIX)
      fprintf(stderr, "Suspending process; attach with the debugger.\n");
      kill(0, SIGSTOP);
#else
      Break(buf.buffer);
#endif
      return;

    case NS_ASSERT_STACK:
      NS_PrintStackTrace();
      return;

    case NS_ASSERT_STACK_AND_ABORT:
      NS_PrintStackTrace();
      [[fallthrough]];

    case NS_ASSERT_ABORT:
      Abort(buf.buffer);
      return;

    case NS_ASSERT_TRAP:
    case NS_ASSERT_UNINITIALIZED:  
      Break(buf.buffer);
      return;
  }
}

static void Abort(const char* aMsg) {
  NoteIntentionalCrash(XRE_GetProcessTypeString());
  MOZ_CRASH_UNSAFE(aMsg);
}

static void RealBreak() {
#if defined(__GNUC__) && \
    (defined(__i386__) || defined(__i386) || defined(__x86_64__))
  asm("int $3");
#elif defined(__arm__)
  asm(
#if defined(__ARM_ARCH_4T__)
      ".arch armv5t\n"
      ".object_arch armv4t\n"
#endif
      "BKPT #0");
#elif defined(__aarch64__)
  asm("brk #0");
#else
#  warning do not know how to break on this platform
#endif
}

static void Break(const char* aMsg) {
#if defined(__GNUC__) && \
    (defined(__i386__) || defined(__i386) || defined(__x86_64__))
  RealBreak();
#elif defined(__arm__) || defined(__aarch64__)
  RealBreak();
#else
#  warning do not know how to break on this platform
#endif
}

nsresult nsDebugImpl::Create(const nsIID& aIID, void** aInstancePtr) {
  static const nsDebugImpl* sImpl;

  if (!sImpl) {
    sImpl = new nsDebugImpl();
  }

  return const_cast<nsDebugImpl*>(sImpl)->QueryInterface(aIID, aInstancePtr);
}


nsresult NS_ErrorAccordingToNSPR() {
  PRErrorCode err = PR_GetError();
  switch (err) {
    case PR_OUT_OF_MEMORY_ERROR:
      return NS_ERROR_OUT_OF_MEMORY;
    case PR_WOULD_BLOCK_ERROR:
      return NS_BASE_STREAM_WOULD_BLOCK;
    case PR_FILE_NOT_FOUND_ERROR:
      return NS_ERROR_FILE_NOT_FOUND;
    case PR_READ_ONLY_FILESYSTEM_ERROR:
      return NS_ERROR_FILE_READ_ONLY;
    case PR_NOT_DIRECTORY_ERROR:
      return NS_ERROR_FILE_NOT_DIRECTORY;
    case PR_IS_DIRECTORY_ERROR:
      return NS_ERROR_FILE_IS_DIRECTORY;
    case PR_LOOP_ERROR:
      return NS_ERROR_FILE_UNRESOLVABLE_SYMLINK;
    case PR_FILE_EXISTS_ERROR:
      return NS_ERROR_FILE_ALREADY_EXISTS;
    case PR_FILE_IS_LOCKED_ERROR:
      return NS_ERROR_FILE_IS_LOCKED;
    case PR_FILE_TOO_BIG_ERROR:
      return NS_ERROR_FILE_TOO_BIG;
    case PR_NO_DEVICE_SPACE_ERROR:
      return NS_ERROR_FILE_NO_DEVICE_SPACE;
    case PR_NAME_TOO_LONG_ERROR:
      return NS_ERROR_FILE_NAME_TOO_LONG;
    case PR_DIRECTORY_NOT_EMPTY_ERROR:
      return NS_ERROR_FILE_DIR_NOT_EMPTY;
    case PR_NO_ACCESS_RIGHTS_ERROR:
      return NS_ERROR_FILE_ACCESS_DENIED;
    default:
      return NS_ERROR_FAILURE;
  }
}

void NS_ABORT_OOM(size_t aSize) {
  MOZ_CRASH("OOM");
}
