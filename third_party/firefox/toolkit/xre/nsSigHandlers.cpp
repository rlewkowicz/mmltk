/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "nsSigHandlers.h"

#if defined(XP_UNIX)

#  include <signal.h>
#  include <stdio.h>
#  include <string.h>
#  include "prthread.h"
#  include "prenv.h"
#  include "nsDebug.h"
#  include "nsString.h"
#  include "nsXULAppAPI.h"

#if defined(LINUX)
#    include <sys/time.h>
#    include <sys/resource.h>
#    include <unistd.h>
#    include <stdlib.h>  // atoi
#    include <sys/prctl.h>
#      include <ucontext.h>
#endif


#if defined(MOZ_WIDGET_GTK)
#    include <dlfcn.h>
#    include "WidgetUtilsGtk.h"
#endif

#if defined(MOZ_WAYLAND)
#    include "wayland-proxy.h"
#endif

unsigned int _gdb_sleep_duration = 300;

#if defined(LINUX) && !0 && defined(DEBUG) && \
      (defined(__i386) || defined(__x86_64) || defined(PPC))
#    define CRAWL_STACK_ON_SIGSEGV
#endif

#if !defined(PR_SET_PTRACER)
#    define PR_SET_PTRACER 0x59616d61
#endif
#if !defined(PR_SET_PTRACER_ANY)
#    define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#if defined(CRAWL_STACK_ON_SIGSEGV)

#    include <unistd.h>
#    include "nsISupportsUtils.h"
#    include "mozilla/Attributes.h"
#    include "mozilla/StackWalk.h"

static const char* gProgname = "huh?";

static const int kClientChannelFd = 3;

extern "C" {

static void PrintStackFrame(uint32_t aFrameNumber, void* aPC, void* aSP,
                            void* aClosure) {
  char buf[1024];
  MozCodeAddressDetails details;

  MozDescribeCodeAddress(aPC, &details);
  MozFormatCodeAddressDetails(buf, sizeof(buf), aFrameNumber, aPC, &details);
  fprintf(stdout, "%s\n", buf);
  fflush(stdout);
}
}

void common_crap_handler(int signum, const void* aFirstFramePC) {
  printf("\nProgram %s (pid = %d) received signal %d.\n", gProgname, getpid(),
         signum);

  printf("Stack:\n");
  MozStackWalk(PrintStackFrame, aFirstFramePC,  0, nullptr);

  printf("Sleeping for %d seconds.\n", _gdb_sleep_duration);
  printf("Type 'gdb %s %d' to attach your debugger to this thread.\n",
         gProgname, getpid());

  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);

  sleep(_gdb_sleep_duration);

  printf("Done sleeping...\n");

  _exit(signum);
}

MOZ_NEVER_INLINE void ah_crap_handler(int signum) {
  common_crap_handler(signum, CallerPC());
}

MOZ_NEVER_INLINE void child_ah_crap_handler(int signum) {
  if (!getenv("MOZ_DONT_UNBLOCK_PARENT_ON_CHILD_CRASH"))
    close(kClientChannelFd);
  common_crap_handler(signum, CallerPC());
}

#endif

#if defined(MOZ_WIDGET_GTK)
#    include <glib.h>
#endif

#if defined(MOZ_WIDGET_GTK)

#if GLIB_MAJOR_VERSION == 2 && GLIB_MINOR_VERSION < 50
typedef enum {
  G_LOG_WRITER_HANDLED = 1,
  G_LOG_WRITER_UNHANDLED = 0,
} GLogWriterOutput;
typedef struct _GLogField GLogField;
struct _GLogField {
  const gchar* key;
  gconstpointer value;
  gssize length;
};
typedef GLogWriterOutput (*GLogWriterFunc)(GLogLevelFlags log_level,
                                           const GLogField* fields,
                                           gsize n_fields, gpointer user_data);
#endif

static GLogFunc orig_log_func = nullptr;

extern "C" {
static void glib_log_func(const gchar* log_domain, GLogLevelFlags log_level,
                          const gchar* message, gpointer user_data);
static GLogWriterOutput glib_log_writer_func(GLogLevelFlags, const GLogField*,
                                             gsize, gpointer);
}

static bool IsCrashyGtkMessage(const nsACString& aMessage) {
  if (aMessage.EqualsLiteral("Lost connection to Wayland compositor.")) {
    return true;
  }
  if (StringBeginsWith(aMessage, "Error flushing display: "_ns)) {
    return true;
  }
  if (StringBeginsWith(aMessage, "Error reading events from display: "_ns)) {
    return true;
  }
  if (StringBeginsWith(aMessage, "Error "_ns) &&
      StringEndsWith(aMessage, " dispatching to Wayland display."_ns)) {
    return true;
  }
  return false;
}

static void HandleGLibMessage(GLogLevelFlags aLogLevel,
                              const nsDependentCString& aMessage) {
  if (MOZ_UNLIKELY(IsCrashyGtkMessage(aMessage))) {
#if defined(MOZ_WAYLAND)
    MOZ_CRASH_UNSAFE_PRINTF(
        "(%s) %s Proxy: %s",
        mozilla::widget::GetDesktopEnvironmentIdentifier().get(),
        strdup(aMessage.get()), WaylandProxy::GetState());
#else
    MOZ_CRASH_UNSAFE_PRINTF(
        "(%s) %s", mozilla::widget::GetDesktopEnvironmentIdentifier().get(),
        strdup(aMessage.get()));
#endif
  }

  if (aLogLevel &
      (G_LOG_LEVEL_ERROR | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION)) {
    NS_DebugBreak(NS_DEBUG_ASSERTION, aMessage.get(), "glib assertion",
                  __FILE__, __LINE__);
  } else if (aLogLevel & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) {
    NS_DebugBreak(NS_DEBUG_WARNING, aMessage.get(), "glib warning", __FILE__,
                  __LINE__);
  }
}

 void glib_log_func(const gchar* log_domain,
                                GLogLevelFlags log_level, const gchar* message,
                                gpointer user_data) {
  HandleGLibMessage(log_level, nsDependentCString(message));
  orig_log_func(log_domain, log_level, message, nullptr);
}

GLogWriterOutput glib_log_writer_func(GLogLevelFlags flags,
                                      const GLogField* fields, gsize n_fields,
                                      gpointer user_data) {
  static const GLogWriterFunc sLogWriterDefault =
      (GLogWriterFunc)dlsym(RTLD_DEFAULT, "g_log_writer_default");
  for (gsize i = 0; i < n_fields; ++i) {
    if (!strcmp(fields[i].key, "MESSAGE") && fields[i].length < 0) {
      HandleGLibMessage(flags,
                        nsDependentCString((const char*)fields[i].value));
      break;
    }
  }
  return sLogWriterDefault(flags, fields, n_fields, user_data);
}

#endif

#if defined(SA_SIGINFO)
static void fpehandler(int signum, siginfo_t* si, void* context) {
  if (si->si_code == FPE_INTDIV || si->si_code == FPE_INTOVF) {
    NS_DebugBreak(NS_DEBUG_ABORT, "Divide by zero", nullptr, __FILE__,
                  __LINE__);
  }

#if defined(LINUX) && !0

#if defined(__i386__)
  ucontext_t* uc = (ucontext_t*)context;
  unsigned long int* cw = &uc->uc_mcontext.fpregs->cw;
  *cw |= FPU_EXCEPTION_MASK;

  unsigned long int* sw = &uc->uc_mcontext.fpregs->sw;
  *sw &= ~FPU_STATUS_FLAGS;
#endif
#if defined(__amd64__)
  ucontext_t* uc = (ucontext_t*)context;

  uint16_t* cw = &uc->uc_mcontext.fpregs->cwd;
  *cw |= FPU_EXCEPTION_MASK;

  uint16_t* sw = &uc->uc_mcontext.fpregs->swd;
  *sw &= ~FPU_STATUS_FLAGS;

  uint32_t* mxcsr = &uc->uc_mcontext.fpregs->mxcsr;
  *mxcsr |= SSE_EXCEPTION_MASK; 
  *mxcsr &= ~SSE_STATUS_FLAGS;  
#endif
#endif
}
#endif

void InstallSignalHandlers(const char* aProgname) {
#if defined(CRAWL_STACK_ON_SIGSEGV)
  if (aProgname) {
    const char* tmp = strdup(aProgname);
    if (tmp) {
      gProgname = tmp;
    }
  }
#endif

  const char* gdbSleep = PR_GetEnv("MOZ_GDB_SLEEP");
  if (gdbSleep && *gdbSleep) {
    unsigned int s;
    if (1 == sscanf(gdbSleep, "%u", &s)) {
      _gdb_sleep_duration = s;
    }
  }

#if defined(CRAWL_STACK_ON_SIGSEGV)
  if (!getenv("XRE_NO_WINDOWS_CRASH_DIALOG")) {
    void (*crap_handler)(int) = GeckoProcessType_Default != XRE_GetProcessType()
                                    ? child_ah_crap_handler
                                    : ah_crap_handler;
    signal(SIGSEGV, crap_handler);
    signal(SIGILL, crap_handler);
    signal(SIGABRT, crap_handler);
  }
#endif

#if defined(SA_SIGINFO)
  struct sigaction sa, osa;
  sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
  sa.sa_sigaction = fpehandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGFPE, &sa, &osa);
#endif

  if (!XRE_IsParentProcess()) {
    signal(SIGINT, SIG_IGN);
  }

#if defined(DEBUG) && defined(LINUX)
  const char* memLimit = PR_GetEnv("MOZ_MEM_LIMIT");
  if (memLimit && *memLimit) {
    long m = atoi(memLimit);
    m *= (1024 * 1024);
    struct rlimit r;
    r.rlim_cur = m;
    r.rlim_max = m;
    setrlimit(RLIMIT_AS, &r);
  }
#endif

#if defined(MOZ_WIDGET_GTK)
  static const auto sSetLogWriter =
      (void (*)(GLogWriterFunc, gpointer, GDestroyNotify))dlsym(
          RTLD_DEFAULT, "g_log_set_writer_func");
  if (sSetLogWriter) {
    sSetLogWriter(glib_log_writer_func, nullptr, nullptr);
  } else {
    orig_log_func = g_log_set_default_handler(glib_log_func, nullptr);
  }
#endif
}

#else
#  error No signal handling implementation for this platform.
#endif
