/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsLoadGroup_h_
#define nsLoadGroup_h_

#include "nsILoadGroup.h"
#include "nsILoadGroupChild.h"
#include "nsIObserver.h"
#include "nsCOMPtr.h"
#include "nsWeakReference.h"
#include "nsISupportsPriority.h"
#include "nsTHashSet.h"

class nsIRequestContext;
class nsIRequestContextService;

namespace mozilla {
namespace net {

class nsLoadGroup : public nsILoadGroup,
                    public nsILoadGroupChild,
                    public nsIObserver,
                    public nsISupportsPriority,
                    public nsSupportsWeakReference {
 public:
  NS_DECL_ISUPPORTS

  NS_DECL_NSIREQUEST

  NS_DECL_NSILOADGROUP

  NS_DECL_NSILOADGROUPCHILD

  NS_DECL_NSISUPPORTSPRIORITY

  NS_DECL_NSIOBSERVER


  nsLoadGroup();

  nsresult Init();
  nsresult InitWithRequestContextId(const uint64_t& aRequestContextId);

  void SetGroupObserver(nsIRequestObserver* aObserver,
                        bool aIncludeBackgroundRequests);

  static constexpr nsLoadFlags kInheritedLoadFlags =
      LOAD_BACKGROUND | LOAD_BYPASS_CACHE | LOAD_FROM_CACHE | VALIDATE_ALWAYS |
      VALIDATE_ONCE_PER_SESSION | VALIDATE_NEVER;

 protected:
  virtual ~nsLoadGroup();

  nsresult MergeLoadFlags(nsIRequest* aRequest, nsLoadFlags& flags);
  nsresult MergeDefaultLoadFlags(nsIRequest* aRequest, nsLoadFlags& flags);

 private:
  nsresult RemoveRequestFromHashtable(nsIRequest* aRequest, nsresult aStatus);
  nsresult NotifyRemovalObservers(nsIRequest* aRequest, nsresult aStatus);

 protected:
  uint32_t mForegroundCount{0};
  uint32_t mLoadFlags{LOAD_NORMAL};
  uint32_t mDefaultLoadFlags{0};
  int32_t mPriority{PRIORITY_NORMAL};

  nsCOMPtr<nsILoadGroup> mLoadGroup;  
  nsCOMPtr<nsIInterfaceRequestor> mCallbacks;
  nsCOMPtr<nsIRequestContext> mRequestContext;
  nsCOMPtr<nsIRequestContextService> mRequestContextService;

  nsCOMPtr<nsIRequest> mDefaultLoadRequest;
  nsTHashSet<RefPtr<nsIRequest>> mRequests;

  nsWeakPtr mObserver;
  nsWeakPtr mParentLoadGroup;

  nsresult mStatus{NS_OK};
  bool mIsCanceling{false};
  bool mBrowsingContextDiscarded{false};
  bool mExternalRequestContext{false};
  bool mNotifyObserverAboutBackgroundRequests{false};

  uint64_t mPendingKeepaliveRequestSize{0};

};

}  
}  

#endif  // nsLoadGroup_h_
