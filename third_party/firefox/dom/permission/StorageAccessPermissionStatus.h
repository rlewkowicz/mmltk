/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StorageAccessPermissionStatus_h_
#define mozilla_dom_StorageAccessPermissionStatus_h_

#include "mozilla/dom/PermissionStatus.h"

namespace mozilla::dom {

class StorageAccessPermissionStatus final : public PermissionStatus {
 public:
  explicit StorageAccessPermissionStatus(nsIGlobalObject* aGlobal);

 private:
  already_AddRefed<PermissionStatusSink> CreateSink() override;
};

}  

#endif  // mozilla_dom_StorageAccessPermissionStatus_h_
