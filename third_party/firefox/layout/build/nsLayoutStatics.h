/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsLayoutStatics_h_
#define nsLayoutStatics_h_

#include "MainThreadUtils.h"
#include "nsDebug.h"
#include "nsISupportsImpl.h"
#include "nscore.h"


class nsLayoutStatics {
 public:
  static nsresult Initialize();

  static void AddRef() {
    NS_ASSERTION(NS_IsMainThread(),
                 "nsLayoutStatics reference counting must be on main thread");

    NS_ASSERTION(sLayoutStaticRefcnt,
                 "nsLayoutStatics already dropped to zero!");

    ++sLayoutStaticRefcnt;
    NS_LOG_ADDREF(&sLayoutStaticRefcnt, sLayoutStaticRefcnt, "nsLayoutStatics",
                  1);
  }
  static void Release() {
    NS_ASSERTION(NS_IsMainThread(),
                 "nsLayoutStatics reference counting must be on main thread");

    --sLayoutStaticRefcnt;
    NS_LOG_RELEASE(&sLayoutStaticRefcnt, sLayoutStaticRefcnt,
                   "nsLayoutStatics");

    if (!sLayoutStaticRefcnt) {
      Shutdown();
    }
  }

  nsLayoutStatics() = delete;

 private:
  static void Shutdown();

  static nsrefcnt sLayoutStaticRefcnt;
};

#endif  // nsLayoutStatics_h_
