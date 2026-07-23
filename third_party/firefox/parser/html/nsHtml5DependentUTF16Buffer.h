/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsHtml5DependentUTF16Buffer_h
#define nsHtml5DependentUTF16Buffer_h

#include "nscore.h"
#include "nsHtml5OwningUTF16Buffer.h"

class MOZ_STACK_CLASS nsHtml5DependentUTF16Buffer : public nsHtml5UTF16Buffer {
 public:
  explicit nsHtml5DependentUTF16Buffer(const nsAString& aToWrap);

  ~nsHtml5DependentUTF16Buffer();

  already_AddRefed<nsHtml5OwningUTF16Buffer> FalliblyCopyAsOwningBuffer();
};

#endif  // nsHtml5DependentUTF16Buffer_h
