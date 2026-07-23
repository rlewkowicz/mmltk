/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GTKTOOLKIT_H
#define GTKTOOLKIT_H

#include "nsString.h"
#include <gtk/gtk.h>


class nsGTKToolkit final {
 public:
  nsGTKToolkit() = default;

  static nsGTKToolkit* GetToolkit();
  static void Shutdown() {
    delete gToolkit;
    gToolkit = nullptr;
  }

  void SetActivationToken(const nsACString& aToken) {
    mActivationToken = aToken;
  }
  const nsCString& GetActivationToken() const { return mActivationToken; }

  void SetFocusTimestamp(uint32_t aTimestamp) { mFocusTimestamp = aTimestamp; }
  uint32_t GetFocusTimestamp() const { return mFocusTimestamp; }

 private:
  static nsGTKToolkit* gToolkit;

  nsCString mActivationToken;
  uint32_t mFocusTimestamp = 0;
};

#endif  // GTKTOOLKIT_H
