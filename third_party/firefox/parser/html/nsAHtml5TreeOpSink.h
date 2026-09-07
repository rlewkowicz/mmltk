/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsAHtml5TreeOpSink_h
#define nsAHtml5TreeOpSink_h

#include "nsTArrayForwardDeclare.h"

class nsHtml5TreeOperation;

class nsAHtml5TreeOpSink {
 public:
  [[nodiscard]] virtual bool MoveOpsFrom(
      nsTArray<nsHtml5TreeOperation>& aOpQueue) = 0;
};

#endif /* nsAHtml5TreeOpSink_h */
