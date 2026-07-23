/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_FileDescriptorUtils_h
#define mozilla_ipc_FileDescriptorUtils_h

#include "mozilla/ipc/FileDescriptor.h"
#include "nsThreadUtils.h"
#include <stdio.h>

namespace mozilla {
namespace ipc {

class CloseFileRunnable final : public Runnable {
  typedef mozilla::ipc::FileDescriptor FileDescriptor;

  FileDescriptor mFileDescriptor;

 public:
  explicit CloseFileRunnable(const FileDescriptor& aFileDescriptor);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE

  void Dispatch();

 private:
  ~CloseFileRunnable();

  void CloseFile();
};

FILE* FileDescriptorToFILE(const FileDescriptor& aDesc, const char* aOpenMode);

FileDescriptor FILEToFileDescriptor(FILE* aStream);

}  
}  

#endif  // mozilla_ipc_FileDescriptorUtils_h
