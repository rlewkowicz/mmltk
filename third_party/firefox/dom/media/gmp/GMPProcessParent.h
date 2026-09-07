/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GMPProcessParent_h
#define GMPProcessParent_h 1

#include "base/basictypes.h"
#include "base/file_path.h"
#include "base/thread.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"
#include "mozilla/media/MediaUtils.h"
#include "nsIFile.h"

class nsIRunnable;

namespace mozilla::gmp {

class GMPProcessParent final : public mozilla::ipc::GeckoChildProcessHost {
 public:
  explicit GMPProcessParent(const std::string& aGMPPath);

  bool Launch(int32_t aTimeoutMs);

  void Delete(nsCOMPtr<nsIRunnable> aCallback = nullptr);

  bool CanShutdown() override { return true; }
  const std::string& GetPluginFilePath() { return mGMPPath; }
  bool UseXPCOM() const { return mUseXpcom; }


  using mozilla::ipc::GeckoChildProcessHost::GetChildProcessHandle;

 private:
  ~GMPProcessParent();

  void DoDelete();

  std::string mGMPPath;
  nsCOMPtr<nsIRunnable> mDeletedCallback;

  bool mUseXpcom;


  UniquePtr<media::ShutdownBlockingTicket> mShutdownBlocker;

  static nsresult NormalizePath(const char* aPath, PathString& aNormalizedPath);

  DISALLOW_COPY_AND_ASSIGN(GMPProcessParent);
};

}  

#endif  // ifndef GMPProcessParent_h
