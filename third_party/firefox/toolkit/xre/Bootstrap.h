/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_Bootstrap_h)
#define mozilla_Bootstrap_h

#include "mozilla/ResultVariant.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/Variant.h"
#include "nscore.h"
#include "nsXULAppAPI.h"



namespace mozilla {

struct StaticXREAppData;

struct BootstrapConfig {
  const StaticXREAppData* appData;
  const char* appDataPath;
};

class Bootstrap {
 protected:
  Bootstrap() = default;

  virtual ~Bootstrap() = default;

  virtual void Dispose() = 0;

  class BootstrapDelete {
   public:
    constexpr BootstrapDelete() = default;
    void operator()(Bootstrap* aPtr) const { aPtr->Dispose(); }
  };

 public:
  typedef mozilla::UniquePtr<Bootstrap, BootstrapDelete> UniquePtr;

  virtual void NS_LogInit() = 0;

  virtual void NS_LogTerm() = 0;

  virtual void XRE_StartupTimelineRecord(int aEvent,
                                         mozilla::TimeStamp aWhen) = 0;

  virtual int XRE_main(int argc, char* argv[],
                       const BootstrapConfig& aConfig) = 0;

  virtual nsresult XRE_InitChildProcess(int argc, char* argv[],
                                        const XREChildData* aChildData) = 0;

  virtual void XRE_EnableSameExecutableForContentProc() = 0;



#if defined(MOZ_ENABLE_FORKSERVER)
  virtual int XRE_ForkServer(int* argc, char*** argv) = 0;
#endif
};

enum class LibLoadingStrategy {
  NoReadAhead,
  ReadAhead,
};

using DLErrorType = UniqueFreePtr<char>;

using BootstrapError = Variant<nsresult, DLErrorType>;

using BootstrapResult = ::mozilla::Result<Bootstrap::UniquePtr, BootstrapError>;

#if defined(XPCOM_GLUE)
typedef void (*GetBootstrapType)(Bootstrap::UniquePtr&);
BootstrapResult GetBootstrap(
    const char* aXPCOMFile = nullptr,
    LibLoadingStrategy aLibLoadingStrategy = LibLoadingStrategy::NoReadAhead);
#else
extern "C" NS_EXPORT void NS_FROZENCALL
XRE_GetBootstrap(Bootstrap::UniquePtr& b);

inline BootstrapResult GetBootstrap(
    const char* aXPCOMFile = nullptr,
    LibLoadingStrategy aLibLoadingStrategy = LibLoadingStrategy::NoReadAhead) {
  Bootstrap::UniquePtr bootstrap;
  XRE_GetBootstrap(bootstrap);
  return bootstrap;
}
#endif


}  

#endif
