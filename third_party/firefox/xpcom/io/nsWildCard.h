/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsWildCard_h_
#define nsWildCard_h_

#include "nscore.h"



#define NON_SXP -1
#define INVALID_SXP -2
#define VALID_SXP 1

int NS_WildCardValid(const char* aExpr);

int NS_WildCardValid(const char16_t* aExpr);

#define MATCH 0
#define NOMATCH 1
#define ABORTED -1


int NS_WildCardMatch(const char* aStr, const char* aExpr,
                     bool aCaseInsensitive);

int NS_WildCardMatch(const char16_t* aStr, const char16_t* aExpr,
                     bool aCaseInsensitive);

#endif /* nsWildCard_h_ */
