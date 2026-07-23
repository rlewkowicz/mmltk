/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DeleteContentTransactionBase_h
#define DeleteContentTransactionBase_h

#include "EditTransactionBase.h"

#include "EditorForwards.h"

#include "mozilla/RefPtr.h"

#include "nsCycleCollectionParticipant.h"
#include "nsISupportsImpl.h"

namespace mozilla {

class DeleteContentTransactionBase : public EditTransactionBase {
 public:
  virtual EditorDOMPoint SuggestPointToPutCaret() const = 0;

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(DeleteContentTransactionBase,
                                           EditTransactionBase)

  NS_DECL_EDITTRANSACTIONBASE_GETASMETHODS_OVERRIDE(
      DeleteContentTransactionBase)

 protected:
  explicit DeleteContentTransactionBase(EditorBase& aEditorBase);
  ~DeleteContentTransactionBase() = default;

  RefPtr<EditorBase> mEditorBase;
};

}  

#endif  // #ifndef DeleteContentTransactionBase_h
