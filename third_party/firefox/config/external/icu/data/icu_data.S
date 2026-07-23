/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


.global ICU_DATA_SYMBOL
#if defined(__wasi__)
.section .rodata,"",@
#else
.section .rodata
#endif
.balign 16
ICU_DATA_SYMBOL:
.incbin ICU_DATA_FILE
#if defined(__wasi__)
.size ICU_DATA_SYMBOL, . - ICU_DATA_SYMBOL
#endif
