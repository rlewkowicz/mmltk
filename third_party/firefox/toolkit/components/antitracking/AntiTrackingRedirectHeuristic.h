/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_antitrackingredirectheuristic_h
#define mozilla_antitrackingredirectheuristic_h

class nsIChannel;
class nsIURI;

namespace mozilla {

void PrepareForAntiTrackingRedirectHeuristic(nsIChannel* aOldChannel,
                                             nsIURI* aOldURI,
                                             nsIChannel* aNewChannel,
                                             nsIURI* aNewURI);

void FinishAntiTrackingRedirectHeuristic(nsIChannel* aNewChannel,
                                         nsIURI* aNewURI);

}  

#endif  // mozilla_antitrackingredirectheuristic_h
