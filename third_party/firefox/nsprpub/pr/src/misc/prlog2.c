/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "prbit.h"

PR_IMPLEMENT(PRIntn) PR_CeilingLog2(PRUint32 n) {
  PRIntn log2;
  PR_CEILING_LOG2(log2, n);
  return log2;
}

PR_IMPLEMENT(PRIntn) PR_FloorLog2(PRUint32 n) {
  PRIntn log2;
  PR_FLOOR_LOG2(log2, n);
  return log2;
}
