/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_EME_MEDIAKEYSYSTEMACCESSPERMISSIONREQUEST_H_
#define DOM_MEDIA_EME_MEDIAKEYSYSTEMACCESSPERMISSIONREQUEST_H_

#include "mozilla/MozPromise.h"
#include "nsContentPermissionHelper.h"

class nsGlobalWindowInner;

namespace mozilla::dom {

class MediaKeySystemAccessPermissionRequest
    : public ContentPermissionRequestBase {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(
      MediaKeySystemAccessPermissionRequest, ContentPermissionRequestBase)

  using RequestPromise = MozPromise<bool, bool, true >;

  static already_AddRefed<MediaKeySystemAccessPermissionRequest> Create(
      nsPIDOMWindowInner* aWindow);

  already_AddRefed<RequestPromise> GetPromise();

  nsresult Start();

  NS_IMETHOD Cancel(void) override;
  NS_IMETHOD Allow(JS::Handle<JS::Value> choices) override;

 private:
  explicit MediaKeySystemAccessPermissionRequest(nsGlobalWindowInner* aWindow);
  ~MediaKeySystemAccessPermissionRequest();

  MozPromiseHolder<RequestPromise> mPromiseHolder;
};

}  

#endif  // DOM_MEDIA_EME_MEDIAKEYSYSTEMACCESSPERMISSIONREQUEST_H_
