/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HttpAuthUtils_h_
#define HttpAuthUtils_h_

class nsIURI;

namespace mozilla {
namespace net {
namespace auth {

bool URIMatchesPrefPattern(nsIURI* uri, const char* pref);

}  
}  
}  

#endif  // HttpAuthUtils_h_
