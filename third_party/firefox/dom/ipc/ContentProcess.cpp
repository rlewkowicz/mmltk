/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ContentProcess.h"

#include "js/Initialization.h"
#include "mozilla/Preferences.h"


#include "mozilla/GeckoArgs.h"
#include "mozilla/Omnijar.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/ProcessUtils.h"
#include "nsAppRunner.h"
#include "nsCategoryManagerUtils.h"

namespace mozilla::dom {

static nsresult GetGREDir(nsIFile** aResult) {
  nsCOMPtr<nsIFile> current;
  nsresult rv = XRE_GetBinaryPath(getter_AddRefs(current));
  NS_ENSURE_SUCCESS(rv, rv);

  const int depth = 1;

  for (int i = 0; i < depth; ++i) {
    nsCOMPtr<nsIFile> parent;
    rv = current->GetParent(getter_AddRefs(parent));
    NS_ENSURE_SUCCESS(rv, rv);

    current = parent;
    NS_ENSURE_TRUE(current, NS_ERROR_UNEXPECTED);
  }


  current.forget(aResult);

  return NS_OK;
}

ContentProcess::ContentProcess(IPC::Channel::ChannelHandle aClientChannel,
                               ProcessId aParentPid,
                               const nsID& aMessageChannelId)
    : ProcessChild(std::move(aClientChannel), aParentPid, aMessageChannelId) {
  NS_LogInit();
}

ContentProcess::~ContentProcess() { NS_LogTerm(); }

bool ContentProcess::Init(int aArgc, char* aArgv[]) {
  InfallibleInit(aArgc, aArgv);
  return true;
}

void ContentProcess::InfallibleInit(int aArgc, char* aArgv[]) {
  Maybe<bool> isForBrowser = Nothing();
  Maybe<const char*> parentBuildID =
      geckoargs::sParentBuildID.Get(aArgc, aArgv);

  Maybe<mozilla::ipc::ReadOnlySharedMemoryHandle> jsInitHandle =
      geckoargs::sJsInitHandle.Get(aArgc, aArgv);

  nsCOMPtr<nsIFile> appDirArg;
  Maybe<const char*> appDir = geckoargs::sAppDir.Get(aArgc, aArgv);
  if (appDir.isSome()) {
    bool flag;
    nsresult rv = XRE_GetFileFromPath(*appDir, getter_AddRefs(appDirArg));
    if (NS_FAILED(rv) || NS_FAILED(appDirArg->Exists(&flag)) || !flag) {
      NS_WARNING("Invalid application directory passed to content process.");
      appDirArg = nullptr;
    }
  }

  Maybe<bool> safeMode = geckoargs::sSafeMode.Get(aArgc, aArgv);
  if (safeMode.isSome()) {
    gSafeMode = *safeMode;
  }

  Maybe<bool> isForBrowerParam = geckoargs::sIsForBrowser.Get(aArgc, aArgv);
  Maybe<bool> notForBrowserParam = geckoargs::sNotForBrowser.Get(aArgc, aArgv);
  if (isForBrowerParam.isSome()) {
    isForBrowser = Some(true);
  }
  if (notForBrowserParam.isSome()) {
    isForBrowser = Some(false);
  }


  if (isForBrowser.isNothing()) {
    MOZ_CRASH("isForBrowser flag missing");
  }
  if (parentBuildID.isNothing()) {
    MOZ_CRASH("parentBuildID flag missing");
  }

  if (!ProcessChild::InitPrefs(aArgc, aArgv)) {
    MOZ_CRASH("InitPrefs failed");
  }

  if (jsInitHandle &&
      !::mozilla::ipc::ImportSharedJSInit(jsInitHandle.extract())) {
    MOZ_CRASH("ImportSharedJSInit failed");
  }

  mContent.Init(TakeInitialEndpoint(), *parentBuildID, *isForBrowser);

  nsCOMPtr<nsIFile> greDir;
  nsresult rv = GetGREDir(getter_AddRefs(greDir));
  if (NS_FAILED(rv)) {
    MOZ_CRASH("GetGREDir failed");
  }

  nsCOMPtr<nsIFile> xpcomAppDir = appDirArg ? appDirArg : greDir;

  rv = mDirProvider.Initialize(xpcomAppDir, greDir);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("mDirProvider.Initialize failed");
  }

  if (!Omnijar::IsInitialized()) {
    Omnijar::ChildProcessInit(aArgc, aArgv);
  }

  Maybe<bool> disableJit = geckoargs::sDisableJit.Get(aArgc, aArgv);
  if (disableJit && *disableJit) {
    JS::DisableJitBackend();
  }

  rv = NS_InitXPCOM(nullptr, xpcomAppDir, &mDirProvider);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("NS_InitXPCOM failed");
  }

  NS_CreateServicesFromCategory("app-startup", nullptr, "app-startup", nullptr);


  mozilla::ipc::BackgroundChild::Startup();
  mozilla::ipc::BackgroundChild::InitContentStarter(&mContent);
}

void ContentProcess::CleanUp() {
  mDirProvider.DoShutdown();
  NS_ShutdownXPCOM(nullptr);
}

}  
