/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_WIDGET_LSB_UTILS_H
#define MOZILLA_WIDGET_LSB_UTILS_H

#include "nsString.h"

namespace mozilla {
namespace widget {
namespace lsb {

bool GetLSBRelease(nsACString& aDistributor, nsACString& aDescription,
                   nsACString& aRelease, nsACString& aCodename);

}  
}  
}  

#endif  // MOZILLA_WIDGET_LSB_UTILS_H
