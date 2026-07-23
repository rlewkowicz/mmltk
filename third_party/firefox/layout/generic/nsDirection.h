/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsDirection_h_
#define nsDirection_h_

#include <ostream>


enum nsDirection { eDirNext = 0, eDirPrevious = 1 };

std::string format_as(nsDirection);
std::ostream& operator<<(std::ostream&, nsDirection);

#endif
