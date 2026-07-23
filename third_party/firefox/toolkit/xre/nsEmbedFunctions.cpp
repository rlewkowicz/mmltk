/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "nsXULAppAPI.h"

#include <stdlib.h>
#if defined(MOZ_WIDGET_GTK)
#  include <glib.h>
#endif

#include "prenv.h"

#include "nsIAppShell.h"
#include "nsIToolkitProfile.h"


#include "nsAppRunner.h"
#include "nsThreadUtils.h"
#include "nsJSUtils.h"
#include "nsWidgetsCID.h"
#include "nsXREDirProvider.h"

#include "mozilla/Omnijar.h"
#include "nsGDKErrorHandler.h"
#include "base/at_exit.h"
#include "base/message_loop.h"
#include "base/process_util.h"

#include "mozilla/AbstractThread.h"
#include "mozilla/FilePreferences.h"
#include "mozilla/IOInterposer.h"
#include "mozilla/ProcessType.h"
#include "mozilla/RDDProcessImpl.h"
#include "mozilla/ipc/UtilityProcessImpl.h"
#include "mozilla/UniquePtr.h"

#include "mozilla/ipc/IOThread.h"
#include "mozilla/ipc/ProcessChild.h"

#include "mozilla/dom/ContentProcess.h"

#include "mozilla/gfx/GPUProcessImpl.h"
#include "mozilla/net/SocketProcessImpl.h"
#  include <sys/prctl.h>
#if !defined(PR_SET_PTRACER)
#    define PR_SET_PTRACER 0x59616d61
#endif
#if !defined(PR_SET_PTRACER_ANY)
#    define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif


#if defined(MOZ_ENABLE_FORKSERVER)
#  include "mozilla/ipc/ForkServer.h"
#endif


#include "nsTraceRefcnt.h"

using namespace mozilla;

using mozilla::ipc::GeckoChildProcessHost;
using mozilla::ipc::IOThread;
using mozilla::ipc::ProcessChild;

using mozilla::dom::ContentProcess;

namespace mozilla::_ipdltest {
UniquePtr<mozilla::ipc::ProcessChild> (*gMakeIPDLUnitTestProcessChild)(
    IPC::Channel::ChannelHandle, base::ProcessId, const nsID&) = nullptr;
}  

static NS_DEFINE_CID(kAppShellCID, NS_APPSHELL_CID);

const char* XRE_GeckoProcessTypeToString(GeckoProcessType aProcessType) {
  switch (aProcessType) {
#define GECKO_PROCESS_TYPE(enum_value, enum_name, string_name, proc_typename, \
                           process_bin_type, procinfo_typename,               \
                           webidl_typename, allcaps_name)                     \
  case GeckoProcessType::GeckoProcessType_##enum_name:                        \
    return string_name;
#include "mozilla/GeckoProcessTypes.h"
#undef GECKO_PROCESS_TYPE
    default:
      return "invalid";
  }
}

const char* XRE_ChildProcessTypeToAnnotation(GeckoProcessType aProcessType) {
  switch (aProcessType) {
    case GeckoProcessType_Default:
      return "";
    case GeckoProcessType_Content:
      return "content";
    default:
      return XRE_GeckoProcessTypeToString(aProcessType);
  }
}



namespace {

int GetDebugChildPauseTime() {
  auto pauseStr = PR_GetEnv("MOZ_DEBUG_CHILD_PAUSE");
  if (pauseStr && *pauseStr) {
    int pause = atoi(pauseStr);
    if (pause != 1) {  
      return pause;
    }
  }
#if defined(XP_UNIX)
  return 30;  
#else
  return 0;
#endif
}

}  

nsresult XRE_InitChildProcess(int aArgc, char* aArgv[],
                              const XREChildData* aChildData) {
  NS_ENSURE_ARG_MIN(aArgc, 2);
  NS_ENSURE_ARG_POINTER(aArgv);
  NS_ENSURE_ARG_POINTER(aArgv[0]);
  MOZ_ASSERT(aChildData);

  NS_SetCurrentThreadName("MainThread");




  ScopedLogging logger;

  mozilla::LogModule::Init(aArgc, aArgv);
  nsTraceRefcnt::EarlyInit();




  AbstractThread::InitTLS();


  SetupErrorHandling(aArgv[0]);

#if defined(MOZ_WIDGET_GTK)
  g_set_prgname(aArgv[0]);
#endif

#if defined(XP_UNIX)
  if (PR_GetEnv("MOZ_DEBUG_CHILD_PROCESS") ||
      PR_GetEnv("MOZ_DEBUG_CHILD_PAUSE")) {
#if defined(XP_LINUX) && defined(DEBUG)
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) {
      printf_stderr("Could not allow ptrace from any process.\n");
    }
#endif
    printf_stderr(
        "\n\nCHILDCHILDCHILDCHILD (process type %s)\n  debug me @ %d\n\n",
        XRE_GetProcessTypeString(), base::GetCurrentProcId());
    sleep(GetDebugChildPauseTime());
  }
#endif



  Maybe<base::ProcessId> parentPID = geckoargs::sParentPid.Get(aArgc, aArgv);
  Maybe<const char*> initialChannelIdString =
      geckoargs::sInitialChannelID.Get(aArgc, aArgv);
  if (NS_WARN_IF(!parentPID || !initialChannelIdString)) {
    return NS_ERROR_FAILURE;
  }

  Maybe<IPC::Channel::ChannelHandle> clientChannel =
      geckoargs::sIPCHandle.Get(aArgc, aArgv);
  if (NS_WARN_IF(!clientChannel)) {
    return NS_ERROR_FAILURE;
  }

  nsID messageChannelId{};
  if (NS_WARN_IF(!messageChannelId.Parse(*initialChannelIdString))) {
    return NS_ERROR_FAILURE;
  }

  base::AtExitManager exitManager;

  nsresult rv = XRE_InitCommandLine(aArgc, aArgv);
  if (NS_FAILED(rv)) {
    return NS_ERROR_FAILURE;
  }

  MessageLoop::Type uiLoopType;
  switch (XRE_GetProcessType()) {
    case GeckoProcessType_Content:
    case GeckoProcessType_GPU:
    case GeckoProcessType_IPDLUnitTest:
    case GeckoProcessType_RDD:
    case GeckoProcessType_Socket:
    case GeckoProcessType_Utility:
      uiLoopType = MessageLoop::TYPE_MOZILLA_CHILD;
      break;
    default:
      uiLoopType = MessageLoop::TYPE_UI;
      break;
  }


  {

    AutoIOInterposer ioInterposerGuard;

    MessageLoop uiMessageLoop(uiLoopType);
    {
      UniquePtr<ProcessChild> process;
      switch (XRE_GetProcessType()) {
        case GeckoProcessType_Default:
          MOZ_CRASH("This makes no sense");
          break;

        case GeckoProcessType_Content:
          ioInterposerGuard.Init();
          process = MakeUnique<ContentProcess>(std::move(*clientChannel),
                                               *parentPID, messageChannelId);
          break;

        case GeckoProcessType_IPDLUnitTest:
          MOZ_RELEASE_ASSERT(mozilla::_ipdltest::gMakeIPDLUnitTestProcessChild,
                             "xul-gtest not loaded!");
          process = mozilla::_ipdltest::gMakeIPDLUnitTestProcessChild(
              std::move(*clientChannel), *parentPID, messageChannelId);
          break;

        case GeckoProcessType_GPU:
          process = MakeUnique<gfx::GPUProcessImpl>(
              std::move(*clientChannel), *parentPID, messageChannelId);
          break;

        case GeckoProcessType_RDD:
          process = MakeUnique<RDDProcessImpl>(std::move(*clientChannel),
                                               *parentPID, messageChannelId);
          break;

        case GeckoProcessType_Socket:
          ioInterposerGuard.Init();
          process = MakeUnique<net::SocketProcessImpl>(
              std::move(*clientChannel), *parentPID, messageChannelId);
          break;

        case GeckoProcessType_Utility:
          process = MakeUnique<ipc::UtilityProcessImpl>(
              std::move(*clientChannel), *parentPID, messageChannelId);
          break;

#if defined(MOZ_ENABLE_FORKSERVER)
        case GeckoProcessType_ForkServer:
          MOZ_CRASH("Fork server should not go here");
          break;
#endif
        default:
          MOZ_CRASH("Unknown main thread class");
      }

      if (!process->Init(aArgc, aArgv)) {
        return NS_ERROR_FAILURE;
      }


      mozilla::FilePreferences::InitDirectoriesAllowlist();
      mozilla::FilePreferences::InitPrefs();

      uiMessageLoop.MessageLoop::Run();

      process->CleanUp();
      mozilla::Omnijar::CleanUp();
    }
  }

  return XRE_DeinitCommandLine();
}

nsISerialEventTarget* XRE_GetAsyncIOEventTarget() {
  return IOThread::Get()->GetEventTarget();
}

nsresult XRE_RunAppShell() {
  nsCOMPtr<nsIAppShell> appShell(do_GetService(kAppShellCID));
  NS_ENSURE_TRUE(appShell, NS_ERROR_FAILURE);
  return appShell->Run();
}

void XRE_ShutdownChildProcess() {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");

  mozilla::DebugOnly<nsISerialEventTarget*> ioTarget =
      XRE_GetAsyncIOEventTarget();
  MOZ_ASSERT(!!ioTarget, "Bad shutdown order");

  MessageLoop::current()->Quit();

}


#if defined(MOZ_ENABLE_FORKSERVER)
int XRE_ForkServer(int* aArgc, char*** aArgv) {
  return mozilla::ipc::ForkServer::RunForkServer(aArgc, aArgv) ? 1 : 0;
}
#endif
