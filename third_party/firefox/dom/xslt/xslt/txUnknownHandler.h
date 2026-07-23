/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef txUnknownHandler_h_
#define txUnknownHandler_h_

#include "txBufferingHandler.h"
#include "txOutputFormat.h"

class txExecutionState;

class txUnknownHandler : public txBufferingHandler {
 public:
  explicit txUnknownHandler(txExecutionState* aEs);
  virtual ~txUnknownHandler();

  TX_DECL_TXAXMLEVENTHANDLER

 private:
  nsresult createHandlerAndFlush(bool aHTMLRoot, const nsAString& aName,
                                 const int32_t aNsID);

  txExecutionState* mEs;

  bool mFlushed;
};

#endif /* txUnknownHandler_h_ */
