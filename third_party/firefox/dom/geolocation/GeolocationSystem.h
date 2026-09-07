/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_GeolocationSystem_h
#define mozilla_dom_GeolocationSystem_h

#include "GeolocationIPCUtils.h"
#include "mozilla/dom/PContentParent.h"

namespace mozilla::dom {

class BrowsingContext;

namespace geolocation {

SystemGeolocationPermissionBehavior GetGeolocationPermissionBehavior();

class SystemGeolocationPermissionRequest {
 public:
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

  virtual void Stop() = 0;

 protected:
  virtual ~SystemGeolocationPermissionRequest() = default;
};

using ParentRequestResolver =
    PContentParent::RequestGeolocationPermissionFromUserResolver;

already_AddRefed<SystemGeolocationPermissionRequest>
RequestLocationPermissionFromUser(BrowsingContext* aBrowsingContext,
                                  ParentRequestResolver&& aResolver);

}  
}  

#endif /* mozilla_dom_GeolocationSystem_h */
