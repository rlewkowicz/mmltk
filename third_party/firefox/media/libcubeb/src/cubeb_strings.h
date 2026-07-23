/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#ifndef CUBEB_STRINGS_H
#define CUBEB_STRINGS_H

#include "cubeb/cubeb.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct cubeb_strings cubeb_strings;

CUBEB_EXPORT int
cubeb_strings_init(cubeb_strings ** strings);

CUBEB_EXPORT void
cubeb_strings_destroy(cubeb_strings * strings);

CUBEB_EXPORT char const *
cubeb_strings_intern(cubeb_strings * strings, char const * s);

#if defined(__cplusplus)
}
#endif

#endif // !CUBEB_STRINGS_H
