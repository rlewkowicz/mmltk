/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ContentRange.h"
#include "nsContentUtils.h"

mozilla::net::ContentRange::ContentRange(
    const nsContentUtils::ParsedRange& aRangeHeader, uint64_t aSize) {
  MOZ_ASSERT(aRangeHeader.Start().isSome() || aRangeHeader.End().isSome());
  MOZ_ASSERT(aRangeHeader.Start().isNothing() ||
             aRangeHeader.End().isNothing() ||
             *aRangeHeader.Start() <= *aRangeHeader.End());

  if (aRangeHeader.Start().isNothing()) {
    mStart = aSize - *aRangeHeader.End();

    mEnd = mStart + *aRangeHeader.End() - 1;

  } else {
    if (*aRangeHeader.Start() >= aSize) {
      return;
    }
    mStart = *aRangeHeader.Start();

    if (aRangeHeader.End().isNothing() || *aRangeHeader.End() >= aSize) {
      mEnd = aSize - 1;
    } else {
      mEnd = *aRangeHeader.End();
    }
  }
  mSize = aSize;
}

void mozilla::net::ContentRange::AsHeader(nsACString& aOutString) const {
  aOutString.Assign("bytes "_ns);
  aOutString.AppendInt(mStart);
  aOutString.AppendLiteral("-");
  aOutString.AppendInt(mEnd);
  aOutString.AppendLiteral("/");
  aOutString.AppendInt(mSize);
}
