/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ChromiumCDMAdapter.h"

#include <utility>

#include "GMPLog.h"
#include "WidevineUtils.h"
#include "content_decryption_module.h"
#include "content_decryption_module_ext.h"
#include "gmp-api/gmp-entrypoints.h"
#include "gmp-api/gmp-video-codec.h"
#include "mozilla/HelperMacros.h"
#include "mozilla/PodOperations.h"
#include "mozilla/dom/KeySystemNames.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

const GMPPlatformAPI* sPlatform = nullptr;

namespace mozilla {

ChromiumCDMAdapter::ChromiumCDMAdapter(
    nsTArray<std::pair<nsCString, nsCString>>&& aHostPathPairs) {
  PopulateHostFiles(std::move(aHostPathPairs));
}

void ChromiumCDMAdapter::SetAdaptee(PRLibrary* aLib) { mLib = aLib; }

void* ChromiumCdmHost(int aHostInterfaceVersion, void* aUserData) {
  GMP_LOG_DEBUG("ChromiumCdmHostFunc({}, {})", aHostInterfaceVersion,
                fmt::ptr(aUserData));
  if (aHostInterfaceVersion != cdm::Host_11::kVersion) {
    return nullptr;
  }
  return aUserData;
}

void* ChromiumCdmHostCompat(int aHostInterfaceVersion, void* aUserData) {
  GMP_LOG_DEBUG("ChromiumCdmHostCompatFunc({}, {})", aHostInterfaceVersion,
                fmt::ptr(aUserData));
  if (aHostInterfaceVersion != cdm::Host_10::kVersion) {
    return nullptr;
  }
  return aUserData;
}

#ifdef MOZILLA_OFFICIAL
static cdm::HostFile TakeToCDMHostFile(HostFileData& aHostFileData) {
  return cdm::HostFile(aHostFileData.mBinary.Path().get(),
                       aHostFileData.mBinary.TakePlatformFile(),
                       aHostFileData.mSig.TakePlatformFile());
}
#endif

GMPErr ChromiumCDMAdapter::GMPInit(const GMPPlatformAPI* aPlatformAPI) {
  GMP_LOG_DEBUG("ChromiumCDMAdapter::GMPInit");
  sPlatform = aPlatformAPI;
  if (NS_WARN_IF(!mLib)) {
    MOZ_CRASH("Missing library!");
    return GMPGenericErr;
  }

#ifdef MOZILLA_OFFICIAL
  auto verify = reinterpret_cast<decltype(::VerifyCdmHost_0)*>(
      PR_FindFunctionSymbol(mLib, MOZ_STRINGIFY(VerifyCdmHost_0)));
  if (verify) {
    nsTArray<cdm::HostFile> files;
    for (HostFileData& hostFile : mHostFiles) {
      files.AppendElement(TakeToCDMHostFile(hostFile));
    }
    bool result = verify(files.Elements(), files.Length());
    GMP_LOG_DEBUG("{} VerifyCdmHost_0 returned {}", __func__, result);
    MOZ_DIAGNOSTIC_ASSERT(result, "Verification failed!");
  }
#endif

  auto init = reinterpret_cast<decltype(::INITIALIZE_CDM_MODULE)*>(
      PR_FindFunctionSymbol(mLib, MOZ_STRINGIFY(INITIALIZE_CDM_MODULE)));
  if (!init) {
    MOZ_CRASH("Missing init method!");
    return GMPGenericErr;
  }

  GMP_LOG_DEBUG(MOZ_STRINGIFY(INITIALIZE_CDM_MODULE) "()");
  init();

  return GMPNoErr;
}

GMPErr ChromiumCDMAdapter::GMPGetAPI(const char* aAPIName, void* aHostAPI,
                                     void** aPluginAPI,
                                     const nsACString& aKeySystem) {
  MOZ_ASSERT(
      aKeySystem.EqualsLiteral(kWidevineKeySystemName) ||
          aKeySystem.EqualsLiteral(kClearKeyKeySystemName) ||
          aKeySystem.EqualsLiteral("fake"),
      "Should not get an unrecognized key system. Why didn't it get "
      "blocked by MediaKeySystemAccess?");
  GMP_LOG_DEBUG("ChromiumCDMAdapter::GMPGetAPI({}, 0x{}, 0x{}, {}) this=0x{}",
                aAPIName, fmt::ptr(aHostAPI), fmt::ptr(aPluginAPI),
                PromiseFlatCString(aKeySystem).get(), fmt::ptr(this));

  int version;
  GetCdmHostFunc getCdmHostFunc;
  if (!strcmp(aAPIName, CHROMIUM_CDM_API)) {
    version = cdm::ContentDecryptionModule_11::kVersion;
    getCdmHostFunc = &ChromiumCdmHost;
  } else if (!strcmp(aAPIName, CHROMIUM_CDM_API_BACKWARD_COMPAT)) {
    version = cdm::ContentDecryptionModule_10::kVersion;
    getCdmHostFunc = &ChromiumCdmHostCompat;
  } else {
    MOZ_ASSERT_UNREACHABLE("We only support and expect cdm10/11!");
    GMP_LOG_DEBUG(
        "ChromiumCDMAdapter::GMPGetAPI({}, 0x{}, 0x{}) this=0x{} got "
        "unsupported CDM version!",
        aAPIName, fmt::ptr(aHostAPI), fmt::ptr(aPluginAPI), fmt::ptr(this));
    return GMPGenericErr;
  }

  auto create = reinterpret_cast<decltype(::CreateCdmInstance)*>(
      PR_FindFunctionSymbol(mLib, "CreateCdmInstance"));
  if (!create) {
    GMP_LOG_DEBUG(
        "ChromiumCDMAdapter::GMPGetAPI({}, 0x{}, 0x{}) this=0x{} "
        "FAILED to find CreateCdmInstance",
        aAPIName, fmt::ptr(aHostAPI), fmt::ptr(aPluginAPI), fmt::ptr(this));
    return GMPGenericErr;
  }

  void* cdm = create(version, aKeySystem.BeginReading(), aKeySystem.Length(),
                     getCdmHostFunc, aHostAPI);
  if (!cdm) {
    GMP_LOG_DEBUG(
        "ChromiumCDMAdapter::GMPGetAPI({}, 0x{}, 0x{}) this=0x{} "
        "FAILED to create cdm version {}",
        aAPIName, fmt::ptr(aHostAPI), fmt::ptr(aPluginAPI), fmt::ptr(this),
        version);
    return GMPGenericErr;
  }
  GMP_LOG_DEBUG("cdm: 0x{}, version: {}", fmt::ptr(cdm), version);
  *aPluginAPI = cdm;

  return *aPluginAPI ? GMPNoErr : GMPNotImplementedErr;
}

void ChromiumCDMAdapter::GMPShutdown() {
  GMP_LOG_DEBUG("ChromiumCDMAdapter::GMPShutdown()");

  decltype(::DeinitializeCdmModule)* deinit;
  deinit =
      (decltype(deinit))(PR_FindFunctionSymbol(mLib, "DeinitializeCdmModule"));
  if (deinit) {
    GMP_LOG_DEBUG("DeinitializeCdmModule()");
    deinit();
  }
}

bool ChromiumCDMAdapter::Supports(int32_t aModuleVersion,
                                  int32_t aInterfaceVersion,
                                  int32_t aHostVersion) {
  return aModuleVersion == CDM_MODULE_VERSION &&
         ((aInterfaceVersion == cdm::ContentDecryptionModule_11::kVersion &&
           aHostVersion == cdm::Host_11::kVersion) ||
          (aInterfaceVersion == cdm::ContentDecryptionModule_10::kVersion &&
           aHostVersion == cdm::Host_10::kVersion));
}

HostFile::HostFile(HostFile&& aOther)
    : mPath(aOther.mPath), mFile(aOther.TakePlatformFile()) {}

HostFile::~HostFile() {
  if (mFile != cdm::kInvalidPlatformFile) {
    close(mFile);
    mFile = cdm::kInvalidPlatformFile;
  }
}

HostFile::HostFile(const nsCString& aPath) : mPath(aPath) {
  mFile = open(aPath.get(), O_RDONLY);
}

cdm::PlatformFile HostFile::TakePlatformFile() {
  cdm::PlatformFile f = mFile;
  mFile = cdm::kInvalidPlatformFile;
  return f;
}

void ChromiumCDMAdapter::PopulateHostFiles(
    nsTArray<std::pair<nsCString, nsCString>>&& aHostPathPairs) {
  for (const auto& pair : aHostPathPairs) {
    mHostFiles.AppendElement(HostFileData(mozilla::HostFile(pair.first),
                                          mozilla::HostFile(pair.second)));
  }
}

}  
