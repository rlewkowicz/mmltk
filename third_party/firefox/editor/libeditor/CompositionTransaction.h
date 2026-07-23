/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CompositionTransaction_h
#define CompositionTransaction_h

#include "EditTransactionBase.h"  // base class

#include "EditorForwards.h"

#include "mozilla/WeakPtr.h"
#include "mozilla/dom/Text.h"
#include "nsCycleCollectionParticipant.h"  // various macros
#include "nsString.h"                      // mStringToInsert

namespace mozilla {
class TextComposition;
class TextRangeArray;

class CompositionTransaction : public EditTransactionBase,
                               public SupportsWeakPtr {
 protected:
  CompositionTransaction(EditorBase& aEditorBase,
                         const nsAString& aStringToInsert,
                         const EditorDOMPointInText& aPointToInsert);

 public:
  static already_AddRefed<CompositionTransaction> Create(
      EditorBase& aEditorBase, const nsAString& aStringToInsert,
      const EditorDOMPointInText& aPointToInsert);

  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(CompositionTransaction,
                                           EditTransactionBase)

  NS_DECL_ISUPPORTS_INHERITED

  NS_DECL_EDITTRANSACTIONBASE
  NS_DECL_EDITTRANSACTIONBASE_GETASMETHODS_OVERRIDE(CompositionTransaction)

  MOZ_CAN_RUN_SCRIPT NS_IMETHOD RedoTransaction() override;
  NS_IMETHOD Merge(nsITransaction* aOtherTransaction, bool* aDidMerge) override;

  dom::Text* GetTextNode() const;

  void MarkFixed();

  MOZ_CAN_RUN_SCRIPT static nsresult SetIMESelection(
      EditorBase& aEditorBase, dom::Text* aTextNode, uint32_t aOffsetInNode,
      uint32_t aLengthOfCompositionString, const TextRangeArray* aRanges);

  friend std::ostream& operator<<(std::ostream& aStream,
                                  const CompositionTransaction& aTransaction);

 protected:
  virtual ~CompositionTransaction() = default;

  virtual void UpdateTextNodeAndOffset(dom::Text& aText, uint32_t aOffset) {
    MOZ_DIAGNOSTIC_ASSERT(GetTextNode() == &aText,
                          "If mEditorBase is a TextEditor, we should work only "
                          "with the single Text");
    mOffset = aOffset;
  }

  MOZ_CAN_RUN_SCRIPT nsresult SetSelectionForRanges(dom::Text& aText,
                                                    uint32_t aOffset);

  uint32_t mOffset;
  uint32_t mReplaceOffset;
  uint32_t mReplaceLength;

  RefPtr<TextRangeArray> mRanges;

  nsString mStringToInsert;

  RefPtr<EditorBase> mEditorBase;

  bool mFixed;
};

class CompositionInTextNodeTransaction final : CompositionTransaction {
 public:
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(CompositionInTextNodeTransaction,
                                           CompositionTransaction)
  NS_DECL_ISUPPORTS_INHERITED

  friend std::ostream& operator<<(
      std::ostream& aStream,
      const CompositionInTextNodeTransaction& aTransaction);

 private:
  NS_DECL_EDITTRANSACTIONBASE_GETASMETHODS_OVERRIDE(
      CompositionInTextNodeTransaction)

  CompositionInTextNodeTransaction(EditorBase& aEditorBase,
                                   const nsAString& aStringToInsert,
                                   const EditorDOMPointInText& aPointToInsert);
  virtual ~CompositionInTextNodeTransaction() = default;

  RefPtr<dom::Text> mTextNode;

  void UpdateTextNodeAndOffset(dom::Text& aText, uint32_t aOffset) final {
    mTextNode = &aText;
    CompositionTransaction::UpdateTextNodeAndOffset(aText, aOffset);
  }

  friend class CompositionTransaction;
};

}  

#endif  // #ifndef CompositionTransaction_h
