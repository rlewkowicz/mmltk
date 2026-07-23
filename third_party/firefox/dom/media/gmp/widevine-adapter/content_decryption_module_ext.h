// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(CDM_CONTENT_DECRYPTION_MODULE_EXT_H_)
#define CDM_CONTENT_DECRYPTION_MODULE_EXT_H_

#include <cstdint>


#include "content_decryption_module_export.h"

namespace cdm {

typedef char FilePathCharType;
typedef int PlatformFile;
const PlatformFile kInvalidPlatformFile = -1;

struct HostFile {
  HostFile(const FilePathCharType* file_path,
           PlatformFile file,
           PlatformFile sig_file)
      : file_path(file_path), file(file), sig_file(sig_file) {}

  const FilePathCharType* file_path = nullptr;
  PlatformFile file = kInvalidPlatformFile;

  PlatformFile sig_file = kInvalidPlatformFile;
};

}  

extern "C" {


CDM_API bool VerifyCdmHost_0(const cdm::HostFile* host_files,
                             uint32_t num_files);
}

#endif
