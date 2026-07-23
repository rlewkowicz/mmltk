/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsGtkHtmlUtils_h_
#define nsGtkHtmlUtils_h_

#include "mozilla/Span.h"
#include "nsString.h"

namespace mozilla::widget {

inline constexpr char kHTMLMarkupPrefix[] =
    R"(<meta http-equiv="content-type" content="text/html; charset=utf-8">)";

bool GetHTMLCharset(Span<const char> aData, nsCString& aFoundCharset);

bool DecodeHTMLData(Span<const char> aData, nsString& aOutDecoded);

}  

#endif  // nsGtkHtmlUtils_h_
