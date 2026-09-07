/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_EditorController_h
#define mozilla_EditorController_h

#include "nscore.h"

class nsControllerCommandTable;

namespace mozilla {


class EditorController final {
 public:
  static void RegisterEditorCommands(nsControllerCommandTable* aCommandTable);
  static void RegisterEditingCommands(nsControllerCommandTable* aCommandTable);
  static void Shutdown();
};

}  

#endif  // #ifndef mozilla_EditorController_h
