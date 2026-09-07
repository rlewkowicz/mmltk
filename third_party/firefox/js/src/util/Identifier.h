/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_Identifier_h
#define util_Identifier_h

#include <stddef.h>  // size_t

#include "js/TypeDecls.h"  // JS::Latin1Char

class JSLinearString;

namespace js {

bool IsIdentifier(const JSLinearString* str);

bool IsIdentifier(const JS::Latin1Char* chars, size_t length);
bool IsIdentifier(const char16_t* chars, size_t length);

bool IsIdentifierASCII(char c);
bool IsIdentifierASCII(char c1, char c2);

bool IsIdentifierNameOrPrivateName(const JSLinearString* str);

bool IsIdentifierNameOrPrivateName(const JS::Latin1Char* chars, size_t length);
bool IsIdentifierNameOrPrivateName(const char16_t* chars, size_t length);

} 

#endif /* util_Identifier_h */
