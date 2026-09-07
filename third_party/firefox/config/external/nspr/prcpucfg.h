/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(NSPR_PRCPUCFG_H_)
#define NSPR_PRCPUCFG_H_

#if defined(__linux__)
#  include "md/_linux.cfg"
#else
#  error "Unsupported platform!"
#endif

#endif
