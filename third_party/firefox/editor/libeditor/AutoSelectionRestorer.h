/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AutoSelectionRestorer_h_
#define mozilla_AutoSelectionRestorer_h_

#include "EditorBase.h"

namespace mozilla {

class AutoSelectionRestorer final {
 public:
  AutoSelectionRestorer() = delete;
  explicit AutoSelectionRestorer(const AutoSelectionRestorer& aOther) = delete;
  AutoSelectionRestorer(AutoSelectionRestorer&& aOther) = delete;

  MOZ_CAN_RUN_SCRIPT_BOUNDARY explicit AutoSelectionRestorer(
      EditorBase* aEditor);

  MOZ_CAN_RUN_SCRIPT_BOUNDARY ~AutoSelectionRestorer();

  void Abort();

  bool MaybeRestoreSelectionLater() const { return !!mEditor; }

 protected:
  MOZ_KNOWN_LIVE EditorBase* mEditor = nullptr;
};
}  

#endif
