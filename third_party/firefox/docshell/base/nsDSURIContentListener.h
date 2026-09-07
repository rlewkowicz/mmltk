/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsDSURIContentListener_h_
#define nsDSURIContentListener_h_

#include "nsCOMPtr.h"
#include "nsIURIContentListener.h"
#include "nsWeakReference.h"
#include "nsITimer.h"
#include "mozilla/WeakPtr.h"
#include "nsDocShell.h"

class nsIInterfaceRequestor;
class nsIWebNavigationInfo;
class nsPIDOMWindowOuter;

class MaybeCloseWindowHelper final : public nsITimerCallback, public nsINamed {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSITIMERCALLBACK
  NS_DECL_NSINAMED

  explicit MaybeCloseWindowHelper(
      mozilla::dom::BrowsingContext* aContentContext);

  mozilla::dom::BrowsingContext* MaybeCloseWindow();

  void SetShouldCloseWindow(bool aShouldCloseWindow);

 protected:
  ~MaybeCloseWindowHelper();

 private:
  already_AddRefed<mozilla::dom::BrowsingContext> ChooseNewBrowsingContext(
      mozilla::dom::BrowsingContext* aBC);

  RefPtr<mozilla::dom::BrowsingContext> mBrowsingContext;

  RefPtr<mozilla::dom::BrowsingContext> mBCToClose;
  nsCOMPtr<nsITimer> mTimer;

  bool mShouldCloseWindow;
};

class nsDSURIContentListener final : public nsIURIContentListener,
                                     public nsSupportsWeakReference {
  friend class nsDocShell;

 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIURICONTENTLISTENER

 protected:
  explicit nsDSURIContentListener(nsDocShell* aDocShell);
  virtual ~nsDSURIContentListener();

  void DropDocShellReference() {
    mDocShell = nullptr;
    mExistingJPEGRequest = nullptr;
    mExistingJPEGStreamListener = nullptr;
  }

 protected:
  mozilla::MainThreadWeakPtr<nsDocShell> mDocShell;
  nsCOMPtr<nsIStreamListener> mExistingJPEGStreamListener;
  nsCOMPtr<nsIChannel> mExistingJPEGRequest;

  nsWeakPtr mWeakParentContentListener;
  nsIURIContentListener* mParentContentListener;
};

#endif /* nsDSURIContentListener_h_ */
