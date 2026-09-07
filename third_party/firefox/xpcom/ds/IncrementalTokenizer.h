/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef INCREMENTAL_TOKENIZER_H_
#define INCREMENTAL_TOKENIZER_H_

#include "mozilla/Tokenizer.h"

#include "nsError.h"
#include <functional>

class nsIInputStream;

namespace mozilla {

class IncrementalTokenizer : public TokenizerBase<char> {
 public:
  typedef std::function<nsresult(Token const&, IncrementalTokenizer& i)>
      Consumer;

  explicit IncrementalTokenizer(Consumer&& aConsumer,
                                const char* aWhitespaces = nullptr,
                                const char* aAdditionalWordChars = nullptr,
                                uint32_t aRawMinBuffered = 1024);

  nsresult FeedInput(const nsACString& aInput);
  nsresult FeedInput(nsIInputStream* aInput, uint32_t aCount);
  nsresult FinishInput();

  [[nodiscard]] bool Next(Token& aToken);

  void NeedMoreInput();

  void Rollback();

 private:
  nsresult Process();

#ifdef DEBUG
  bool mConsuming{false};
#endif  // DEBUG
  bool mNeedMoreInput{false};
  bool mRollback{false};
  nsCString mInput;
  nsCString::index_type mInputCursor{0};
  Consumer mConsumer;
};

}  

#endif
