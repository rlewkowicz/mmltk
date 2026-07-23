/*
 * Copyright 2013, Mozilla Foundation and contributors
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

#ifndef GMP_STORAGE_h_
#define GMP_STORAGE_h_

#include <stdint.h>

#include "gmp-errors.h"

#define GMP_MAX_RECORD_SIZE (10 * 1024 * 1024)

#define GMP_MAX_RECORD_NAME_SIZE 2000

class GMPRecord {
 public:
  virtual GMPErr Open() = 0;

  virtual GMPErr Read() = 0;

  virtual GMPErr Write(const uint8_t* aData, uint32_t aDataSize) = 0;

  virtual GMPErr Close() = 0;

  virtual ~GMPRecord() = default;
};

class GMPRecordClient {
 public:
  virtual void OpenComplete(GMPErr aStatus) = 0;

  virtual void ReadComplete(GMPErr aStatus, const uint8_t* aData,
                            uint32_t aDataSize) = 0;

  virtual void WriteComplete(GMPErr aStatus) = 0;

  virtual ~GMPRecordClient() = default;
};

#endif  // GMP_STORAGE_h_
