/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef InsertNodeTransaction_h
#define InsertNodeTransaction_h

#include "EditTransactionBase.h"  // for EditTransactionBase, etc.

#include "EditorDOMPoint.h"  // for EditorDOMPoint
#include "EditorForwards.h"

#include "nsCOMPtr.h"  // for nsCOMPtr
#include "nsCycleCollectionParticipant.h"
#include "nsIContent.h"       // for nsIContent
#include "nsISupportsImpl.h"  // for NS_DECL_ISUPPORTS_INHERITED

namespace mozilla {

class EditorBase;

class InsertNodeTransaction final : public EditTransactionBase {
 protected:
  template <typename PT, typename CT>
  InsertNodeTransaction(EditorBase& aEditorBase, nsIContent& aContentToInsert,
                        const EditorDOMPointBase<PT, CT>& aPointToInsert);

 public:
  template <typename PT, typename CT>
  static already_AddRefed<InsertNodeTransaction> Create(
      EditorBase& aEditorBase, nsIContent& aContentToInsert,
      const EditorDOMPointBase<PT, CT>& aPointToInsert);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(InsertNodeTransaction,
                                           EditTransactionBase)

  NS_DECL_EDITTRANSACTIONBASE
  NS_DECL_EDITTRANSACTIONBASE_GETASMETHODS_OVERRIDE(InsertNodeTransaction)

  MOZ_CAN_RUN_SCRIPT NS_IMETHOD RedoTransaction() override;

  template <typename EditorDOMPointType>
  EditorDOMPointType SuggestPointToPutCaret() const {
    if (MOZ_UNLIKELY(!mPointToInsert.IsSet() || !mContentToInsert)) {
      return EditorDOMPointType();
    }
    return EditorDOMPointType::After(mContentToInsert);
  }

  friend std::ostream& operator<<(std::ostream& aStream,
                                  const InsertNodeTransaction& aTransaction);

 protected:
  virtual ~InsertNodeTransaction() = default;

  nsCOMPtr<nsIContent> mContentToInsert;

  EditorDOMPoint mPointToInsert;

  RefPtr<EditorBase> mEditorBase;
};

}  

#endif  // #ifndef InsertNodeTransaction_h
