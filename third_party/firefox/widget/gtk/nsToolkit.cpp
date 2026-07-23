/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nscore.h"  // needed for 'nullptr'
#include "nsGTKToolkit.h"

nsGTKToolkit* nsGTKToolkit::gToolkit = nullptr;

nsGTKToolkit* nsGTKToolkit::GetToolkit() {
  if (!gToolkit) {
    gToolkit = new nsGTKToolkit();
  }

  return gToolkit;
}
