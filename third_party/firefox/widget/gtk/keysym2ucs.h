/*
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WIDGET_GTK_KEYSYM2UCS_H_
#define WIDGET_GTK_KEYSYM2UCS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

long keysym2ucs(uint32_t keysym);

#ifdef __cplusplus
} 
#endif

#endif  // WIDGET_GTK_KEYSYM2UCS_H_
