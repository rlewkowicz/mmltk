/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef FileDescriptorFile_h
#define FileDescriptorFile_h

#include "mozilla/ipc/FileDescriptor.h"
#include "nsIFile.h"
#include "private/pprio.h"

namespace mozilla {
namespace net {

class FileDescriptorFile final : public nsIFile {
  typedef mozilla::ipc::FileDescriptor FileDescriptor;

 public:
  FileDescriptorFile(const FileDescriptor& aFD, nsIFile* aFile);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIFILE

 private:
  ~FileDescriptorFile() = default;

  FileDescriptorFile(const FileDescriptorFile& other);

  nsCOMPtr<nsIFile> mFile;
  FileDescriptor mFD;
};

}  
}  

#endif  // FileDescriptorFile_h
