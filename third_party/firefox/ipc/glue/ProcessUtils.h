/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_ProcessUtils_h
#define mozilla_ipc_ProcessUtils_h

#include "mozilla/GeckoArgs.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "mozilla/ipc/SharedMemoryHandle.h"
#include "mozilla/ipc/SharedMemoryMapping.h"
#include "mozilla/Preferences.h"
#include "nsXULAppAPI.h"

namespace mozilla {
namespace ipc {

class GeckoChildProcessHost;

void SetThisProcessName(const char* aName);

class SharedPreferenceSerializer final {
 public:
  explicit SharedPreferenceSerializer();
  SharedPreferenceSerializer(SharedPreferenceSerializer&& aOther);
  ~SharedPreferenceSerializer();

  bool SerializeToSharedMemory(const GeckoProcessType aDestinationProcessType,
                               const nsACString& aDestinationRemoteType);

  const ReadOnlySharedMemoryHandle& GetPrefsHandle() const {
    return mPrefsHandle;
  }

  const ReadOnlySharedMemoryHandle& GetPrefMapHandle() const {
    return mPrefMapHandle;
  }

  void AddSharedPrefCmdLineArgs(GeckoChildProcessHost& procHost,
                                geckoargs::ChildProcessArgs& aExtraOpts) const;

 private:
  DISALLOW_COPY_AND_ASSIGN(SharedPreferenceSerializer);
  ReadOnlySharedMemoryHandle mPrefMapHandle;
  ReadOnlySharedMemoryHandle mPrefsHandle;
};

class SharedPreferenceDeserializer final {
 public:
  SharedPreferenceDeserializer();
  ~SharedPreferenceDeserializer();

  bool DeserializeFromSharedMemory(ReadOnlySharedMemoryHandle&& aPrefsHandle,
                                   ReadOnlySharedMemoryHandle&& aPrefMapHandle);

 private:
  DISALLOW_COPY_AND_ASSIGN(SharedPreferenceDeserializer);
  ReadOnlySharedMemoryHandle mPrefMapHandle;
  ReadOnlySharedMemoryMapping mShmem;
};

void ExportSharedJSInit(GeckoChildProcessHost& procHost,
                        geckoargs::ChildProcessArgs& aExtraOpts);

bool ImportSharedJSInit(ReadOnlySharedMemoryHandle&& aJsInitHandle);

}  
}  

#endif  // ifndef mozilla_ipc_ProcessUtils_h
