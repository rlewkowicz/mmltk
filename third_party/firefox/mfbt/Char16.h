/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_Char16_h)
#define mozilla_Char16_h

#if defined(__cplusplus)



typedef const char16_t* char16ptr_t;


static_assert(sizeof(char16_t) == 2, "Is char16_t type 16 bits?");
static_assert(char16_t(-1) > char16_t(0), "Is char16_t type unsigned?");
static_assert(sizeof(u'A') == 2, "Is unicode char literal 16 bits?");
static_assert(sizeof(u""[0]) == 2, "Is unicode string char 16 bits?");

#endif

#endif
