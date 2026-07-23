/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __expat_config_h__
#define __expat_config_h__

#include "expat_config_moz.h"

#define HAVE_MEMMOVE 1

#define XML_POOR_ENTROPY 1

#define XMLCALL
#define XML_STATIC
#ifdef HAVE_VISIBILITY_HIDDEN_ATTRIBUTE
#  undef XMLIMPORT
#  define XMLIMPORT __attribute__((visibility("hidden")))
#endif

#define XML_UNICODE
typedef char XML_LChar;
#ifdef __cplusplus
typedef char16_t XML_Char;
#define XML_T(x) (char16_t)x
#else
#include <stdint.h>
typedef uint16_t XML_Char;
#define XML_T(x) (uint16_t)x
#endif

#define XML_DTD
#define XML_GE 1
#define XML_NS
#define XML_CONTEXT_BYTES 0

#endif /* __expat_config_h__ */
