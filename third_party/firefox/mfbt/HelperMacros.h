/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_HelperMacros_h
#define mozilla_HelperMacros_h

#define MOZ_STRINGIFY_NO_EXPANSION(x) #x

#define MOZ_STRINGIFY(x) MOZ_STRINGIFY_NO_EXPANSION(x)

#endif  // mozilla_HelperMacros_h
