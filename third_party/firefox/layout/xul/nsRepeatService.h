/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nsRepeatService_h_)
#define nsRepeatService_h_

#include "nsCOMPtr.h"
#include "nsITimer.h"
#include "nsString.h"

#define INITAL_REPEAT_DELAY 250

#  define REPEAT_DELAY 50

class nsITimer;

namespace mozilla {
namespace dom {
class Document;
}
}  

class nsRepeatService final {
 public:
  typedef void (*Callback)(void* aData);

  ~nsRepeatService();

  void Start(Callback aCallback, void* aCallbackData,
             mozilla::dom::Document* aDocument, const nsACString& aCallbackName,
             uint32_t aInitialDelay = INITAL_REPEAT_DELAY);
  void Stop(Callback aCallback, void* aData);

  static nsRepeatService* GetInstance();
  static void Shutdown();

 protected:
  nsRepeatService();

 private:
  void InitTimerCallback(uint32_t aInitialDelay);

  Callback mCallback;
  void* mCallbackData;
  nsCString mCallbackName;
  nsCOMPtr<nsITimer> mRepeatTimer;

};  

#endif
