/*
 * Copyright 2015, Mozilla Foundation and contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include <string>
#include <vector>

#include "ClearKeyCDM.h"
#include "ClearKeySessionManager.h"
#include "content_decryption_module.h"
#include "content_decryption_module_ext.h"
#include "mozilla/dom/KeySystemNames.h"
#include "nss.h"

#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

#if defined(ENABLE_WMF)
#  include "WMFUtils.h"
#endif

extern "C" {

CDM_API
void INITIALIZE_CDM_MODULE() {}

static bool sCanReadHostVerificationFiles = false;

CDM_API
void* CreateCdmInstance(int cdm_interface_version, const char* key_system,
                        uint32_t key_system_size,
                        GetCdmHostFunc get_cdm_host_func, void* user_data) {
  CK_LOGE("ClearKey CreateCDMInstance");

  if (cdm_interface_version != cdm::ContentDecryptionModule_11::kVersion) {
    CK_LOGE(
        "ClearKey CreateCDMInstance failed due to requesting unsupported "
        "version %d.",
        cdm_interface_version);
    return nullptr;
  }
#if defined(ENABLE_WMF)
  if (!wmf::EnsureLibs()) {
    CK_LOGE("Required libraries were not found");
    return nullptr;
  }
#endif

  if (NSS_NoDB_Init(nullptr) == SECFailure) {
    CK_LOGE("Unable to initialize NSS");
    return nullptr;
  }

#if defined(MOZILLA_OFFICIAL)
  if (!sCanReadHostVerificationFiles) {
    return nullptr;
  }
#endif

  cdm::Host_11* host = static_cast<cdm::Host_11*>(
      get_cdm_host_func(cdm_interface_version, user_data));
  ClearKeyCDM* clearKey = new ClearKeyCDM(host);

  CK_LOGE("Created ClearKeyCDM instance!");

  return clearKey;
}

const size_t TEST_READ_SIZE = 16 * 1024;

bool CanReadSome(cdm::PlatformFile aFile) {
  std::vector<uint8_t> data;
  data.resize(TEST_READ_SIZE);
  return read(aFile, &data.front(), TEST_READ_SIZE) > 0;
}

void ClosePlatformFile(cdm::PlatformFile aFile) {
  close(aFile);
}

static uint32_t NumExpectedHostFiles(const cdm::HostFile* aHostFiles,
                                     uint32_t aNumFiles) {
  return 4;
}

CDM_API
bool VerifyCdmHost_0(const cdm::HostFile* aHostFiles, uint32_t aNumFiles) {
  bool rv = (aNumFiles == NumExpectedHostFiles(aHostFiles, aNumFiles));
  for (uint32_t i = 0; i < aNumFiles; i++) {
    const cdm::HostFile& hostFile = aHostFiles[i];
    if (hostFile.file != cdm::kInvalidPlatformFile) {
      if (!CanReadSome(hostFile.file)) {
        rv = false;
      }
      ClosePlatformFile(hostFile.file);
    }
    if (hostFile.sig_file != cdm::kInvalidPlatformFile) {
      ClosePlatformFile(hostFile.sig_file);
    }
  }
  sCanReadHostVerificationFiles = rv;
  return rv;
}

}  
