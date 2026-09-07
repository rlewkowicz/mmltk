/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ImageBlocker.h"

#include "mozilla/StaticPrefs_permissions.h"
#include "nsContentUtils.h"
#include "nsIPermissionManager.h"
#include "nsNetUtil.h"

using namespace mozilla;
using namespace mozilla::image;

NS_IMPL_ISUPPORTS(ImageBlocker, nsIContentPolicy)

NS_IMETHODIMP
ImageBlocker::ShouldLoad(nsIURI* aContentLocation, nsILoadInfo* aLoadInfo,
                         int16_t* aShouldLoad) {
  *aShouldLoad = nsIContentPolicy::ACCEPT;

  if (!aContentLocation) {
    return NS_OK;
  }

  if (!nsContentUtils::IsImageType(aLoadInfo->GetExternalContentPolicyType())) {
    return NS_OK;
  }

  if (ImageBlocker::ShouldBlock(aContentLocation)) {
    NS_SetRequestBlockingReason(
        aLoadInfo, nsILoadInfo::BLOCKING_REASON_CONTENT_POLICY_CONTENT_BLOCKED);
    *aShouldLoad = nsIContentPolicy::REJECT_TYPE;
  }

  return NS_OK;
}

NS_IMETHODIMP
ImageBlocker::ShouldProcess(nsIURI* aContentLocation, nsILoadInfo* aLoadInfo,
                            int16_t* aShouldProcess) {
  *aShouldProcess = nsIContentPolicy::ACCEPT;
  return NS_OK;
}

bool ImageBlocker::ShouldBlock(nsIURI* aContentLocation) {
  if (StaticPrefs::permissions_default_image() !=
      nsIPermissionManager::DENY_ACTION) {
    return false;
  }

  return net::SchemeIsHttpOrHttps(aContentLocation);
}
