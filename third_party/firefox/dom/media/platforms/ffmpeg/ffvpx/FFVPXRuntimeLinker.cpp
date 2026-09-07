/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FFVPXRuntimeLinker.h"

#include "FFmpegLibWrapper.h"
#include "FFmpegLog.h"
#include "mozilla/FileUtils.h"
#include "mozilla/ToString.h"
#include "nsLocalFile.h"
#include "nsXPCOMPrivate.h"
#include "prlink.h"

namespace mozilla {

template <int V>
class FFmpegDecoderModule {
 public:
  static void Init(const FFmpegLibWrapper*);
  static already_AddRefed<PlatformDecoderModule> Create(
      const FFmpegLibWrapper*);
};

template <int V>
class FFmpegEncoderModule {
 public:
  static void Init(const FFmpegLibWrapper*);
  static already_AddRefed<PlatformEncoderModule> Create(
      const FFmpegLibWrapper*);
};

static FFmpegLibWrapper sFFVPXLib;

StaticMutex FFVPXRuntimeLinker::sMutex;

FFVPXRuntimeLinker::LinkStatus FFVPXRuntimeLinker::sLinkStatus =
    LinkStatus_INIT;

static PRLibrary* MozAVLink(nsIFile* aFile) {
  PRLibSpec lspec;
  PathString path = aFile->NativePath();
  lspec.type = PR_LibSpec_Pathname;
  lspec.value.pathname = path.get();
  PRLibrary* lib = PR_LoadLibraryWithFlags(lspec, PR_LD_NOW | PR_LD_LOCAL);
  if (!lib) {
    FFMPEGV_LOG("unable to load library {}", aFile->HumanReadablePath().get());
  }
  return lib;
}

void FFVPXRuntimeLinker::PrefCallbackLogLevel(const char* aPref, void* aData) {
  sFFVPXLib.UpdateLogLevel();
}

bool FFVPXRuntimeLinker::Init() {
  StaticMutexAutoLock lock(sMutex);

  if (sLinkStatus) {
    if (sLinkStatus == LinkStatus_SUCCEEDED) {
      FFmpegDecoderModule<FFVPX_VERSION>::Init(&sFFVPXLib);
      FFmpegEncoderModule<FFVPX_VERSION>::Init(&sFFVPXLib);
    }
    return sLinkStatus == LinkStatus_SUCCEEDED;
  }

  sLinkStatus = LinkStatus_FAILED;

  PathString path =
      GetLibraryFilePathname(XUL_DLL, (PRFuncPtr)&FFVPXRuntimeLinker::Init);
  if (path.IsEmpty()) {
    return false;
  }
  nsCOMPtr<nsIFile> libFile;
  if (NS_FAILED(NS_NewPathStringLocalFile(path, getter_AddRefs(libFile)))) {
    return false;
  }


  if (NS_FAILED(libFile->SetNativeLeafName(MOZ_DLL_PREFIX
                                           "mozavutil" MOZ_DLL_SUFFIX ""_ns))) {
    return false;
  }
  sFFVPXLib.mAVUtilLib = MozAVLink(libFile);

  if (NS_FAILED(libFile->SetNativeLeafName(
          MOZ_DLL_PREFIX "mozavcodec" MOZ_DLL_SUFFIX ""_ns))) {
    return false;
  }
  sFFVPXLib.mAVCodecLib = MozAVLink(libFile);
  FFmpegLibWrapper::LinkResult res = sFFVPXLib.Link();
  FFMPEGP_LOG("Link result: {}", ToString(res).c_str());
  if (res == FFmpegLibWrapper::LinkResult::Success) {
    sLinkStatus = LinkStatus_SUCCEEDED;
    FFmpegDecoderModule<FFVPX_VERSION>::Init(&sFFVPXLib);
    FFmpegEncoderModule<FFVPX_VERSION>::Init(&sFFVPXLib);
    return true;
  }
  return false;
}

already_AddRefed<PlatformDecoderModule> FFVPXRuntimeLinker::CreateDecoder() {
  if (!Init()) {
    return nullptr;
  }
  return FFmpegDecoderModule<FFVPX_VERSION>::Create(&sFFVPXLib);
}

already_AddRefed<PlatformEncoderModule> FFVPXRuntimeLinker::CreateEncoder() {
  if (!Init()) {
    return nullptr;
  }
  return FFmpegEncoderModule<FFVPX_VERSION>::Create(&sFFVPXLib);
}

void FFVPXRuntimeLinker::GetFFTFuncs(FFmpegFFTFuncs* aOutFuncs) {
  []() MOZ_NO_THREAD_SAFETY_ANALYSIS {
    MOZ_ASSERT(sLinkStatus != LinkStatus_INIT);
  }();
  MOZ_ASSERT(sFFVPXLib.av_tx_init && sFFVPXLib.av_tx_uninit);
  aOutFuncs->init = sFFVPXLib.av_tx_init;
  aOutFuncs->uninit = sFFVPXLib.av_tx_uninit;
}

}  
