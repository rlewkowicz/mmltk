/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_InputStreamLengthHelper_h
#define mozilla_InputStreamLengthHelper_h

#include <functional>

#include "nsISupportsImpl.h"
#include "nsIInputStreamLength.h"
#include "nsThreadUtils.h"

class nsIInputStream;

namespace mozilla {


class InputStreamLengthHelper final : public Runnable,
                                      public nsIInputStreamLengthCallback {
 public:
  NS_DECL_ISUPPORTS_INHERITED

  static bool GetSyncLength(nsIInputStream* aStream, int64_t* aLength);

  static void GetAsyncLength(
      nsIInputStream* aStream,
      const std::function<void(int64_t aLength)>& aCallback);

 private:
  NS_DECL_NSIINPUTSTREAMLENGTHCALLBACK

  InputStreamLengthHelper(
      nsIInputStream* aStream,
      const std::function<void(int64_t aLength)>& aCallback);

  ~InputStreamLengthHelper();

  NS_IMETHOD
  Run() override;

  void ExecCallback(int64_t aLength);

  nsCOMPtr<nsIInputStream> mStream;
  std::function<void(int64_t aLength)> mCallback;
};

}  

#endif  // mozilla_InputStreamLengthHelper_h
