/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_EarlyHintRegistrar_h_
#define mozilla_net_EarlyHintRegistrar_h_

#include "mozilla/RefCounted.h"
#include "mozilla/dom/ipc/IdType.h"
#include "nsRefPtrHashtable.h"
#include "mozilla/AlreadyAddRefed.h"

class nsIParentChannel;

namespace mozilla::net {

class EarlyHintPreloader;

class EarlyHintRegistrar final : public RefCounted<EarlyHintRegistrar> {
  using EarlyHintHashtable =
      nsRefPtrHashtable<nsUint64HashKey, EarlyHintPreloader>;

 public:
  MOZ_DECLARE_REFCOUNTED_TYPENAME(EarlyHintRegistrar)

  EarlyHintRegistrar();
  ~EarlyHintRegistrar();

  void RegisterEarlyHint(uint64_t aEarlyHintPreloaderId,
                         EarlyHintPreloader* aEhp);

  bool LinkParentChannel(dom::ContentParentId aCpId,
                         uint64_t aEarlyHintPreloaderId,
                         nsIParentChannel* aParent);

  void DeleteEntry(dom::ContentParentId aCpId, uint64_t aEarlyHintPreloaderId);

  static void CleanUp();

  static already_AddRefed<EarlyHintRegistrar> GetOrCreate();

 private:
  EarlyHintHashtable mEarlyHint;
};

}  

#endif  // mozilla_net_EarlyHintRegistrar_h_
