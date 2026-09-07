/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FileDescriptorUtils.h"

#include "nsIEventTarget.h"

#include "nsCOMPtr.h"
#include "nsDebug.h"
#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "prio.h"
#include "private/pprio.h"

#include <errno.h>
#  include <unistd.h>

using mozilla::ipc::CloseFileRunnable;

CloseFileRunnable::CloseFileRunnable(const FileDescriptor& aFileDescriptor)
    : Runnable("CloseFileRunnable"), mFileDescriptor(aFileDescriptor) {
  MOZ_ASSERT(aFileDescriptor.IsValid());
}

CloseFileRunnable::~CloseFileRunnable() {
  if (mFileDescriptor.IsValid()) {
    CloseFile();
  }
}

NS_IMPL_ISUPPORTS_INHERITED0(CloseFileRunnable, Runnable)

void CloseFileRunnable::Dispatch() {
  nsCOMPtr<nsIEventTarget> eventTarget =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  NS_ENSURE_TRUE_VOID(eventTarget);

  nsresult rv = eventTarget->Dispatch(this, NS_DISPATCH_NORMAL);
  NS_ENSURE_SUCCESS_VOID(rv);
}

void CloseFileRunnable::CloseFile() {
  mFileDescriptor = FileDescriptor();
}

NS_IMETHODIMP
CloseFileRunnable::Run() {
  MOZ_ASSERT(!NS_IsMainThread());

  CloseFile();
  return NS_OK;
}

namespace mozilla {
namespace ipc {

FILE* FileDescriptorToFILE(const FileDescriptor& aDesc, const char* aOpenMode) {
  if (!aDesc.IsValid()) {
    errno = EBADF;
    return nullptr;
  }
  auto handle = aDesc.ClonePlatformHandle();
  int fd = handle.release();
  FILE* file = fdopen(fd, aOpenMode);
  if (!file) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
  }
  return file;
}

FileDescriptor FILEToFileDescriptor(FILE* aStream) {
  if (!aStream) {
    errno = EBADF;
    return FileDescriptor();
  }
  return FileDescriptor(fileno(aStream));
}

}  
}  
