/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsGlobalWindowCommands_h_
#define nsGlobalWindowCommands_h_

#include "nsStringFwd.h"
#include "nscore.h"

namespace mozilla::layers {
struct KeyboardScrollAction;
}  

class nsControllerCommandTable;

class nsWindowCommandRegistration {
 public:
  static void RegisterWindowCommands(nsControllerCommandTable* aCommandTable);
};

class nsGlobalWindowCommands {
 public:
  using KeyboardScrollAction = mozilla::layers::KeyboardScrollAction;

  static bool FindScrollCommand(const nsACString& aCommandName,
                                KeyboardScrollAction* aOutAction);
};

#endif  // nsGlobalWindowCommands_h_
