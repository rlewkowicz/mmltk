/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef MOZILLA_DOUBLECONVERSION_H
#define MOZILLA_DOUBLECONVERSION_H

#include "mozilla/Maybe.h"
#include "mozilla/Span.h"

#include <string>

namespace mozilla {

Maybe<double> StringToDouble(Span<const char> aStringSpan);

}

#endif // MOZILLA_DOUBLECONVERSION_H
