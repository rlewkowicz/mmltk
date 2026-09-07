/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_AutoCopyListener_h)
#define mozilla_AutoCopyListener_h

#include "mozilla/StaticPrefs_clipboard.h"
#include "mozilla/dom/Selection.h"
#include "nsIClipboard.h"

namespace mozilla {

class AutoCopyListener final {
 public:
  static void OnSelectionChange(dom::Document* aDocument,
                                dom::Selection& aSelection, int16_t aReason);

  static bool IsEnabled() {
    return StaticPrefs::clipboard_autocopy();
  }

 private:
  static const nsIClipboard::ClipboardType sClipboardID =
      nsIClipboard::kSelectionClipboard;
};

}  

#endif
