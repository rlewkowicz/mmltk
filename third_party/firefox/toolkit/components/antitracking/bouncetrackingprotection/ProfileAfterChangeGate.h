/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ProfileAfterChangeGate_h
#define mozilla_ProfileAfterChangeGate_h

#include "nsIObserver.h"


class ProfileAfterChangeGate final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
 private:
  ~ProfileAfterChangeGate() = default;
};

nsresult EnsurePastProfileAfterChange();

#endif
