/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsHtml5OwningUTF16Buffer_h
#define nsHtml5OwningUTF16Buffer_h

#include "nsHtml5UTF16Buffer.h"
#include "mozilla/Span.h"

class nsHtml5OwningUTF16Buffer : public nsHtml5UTF16Buffer {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(nsHtml5OwningUTF16Buffer)

 private:
  explicit nsHtml5OwningUTF16Buffer(char16_t* aBuffer);

 public:
  explicit nsHtml5OwningUTF16Buffer(void* aKey);

 protected:
  ~nsHtml5OwningUTF16Buffer();

 public:
  RefPtr<nsHtml5OwningUTF16Buffer> next;

  void* key;

  static already_AddRefed<nsHtml5OwningUTF16Buffer> FalliblyCreate(
      int32_t aLength);

  void Swap(nsHtml5OwningUTF16Buffer* aOther);

  mozilla::Span<char16_t> TailAsSpan(int32_t aBufferSize);

  void AdvanceEnd(int32_t aNumberOfCodeUnits);
};

#endif  // nsHtml5OwningUTF16Buffer_h
