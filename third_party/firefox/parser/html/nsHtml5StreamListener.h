/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsHtml5StreamListener_h
#define nsHtml5StreamListener_h

#include "nsIStreamListener.h"
#include "nsIThreadRetargetableStreamListener.h"
#include "nsHtml5StreamParser.h"
#include "mozilla/ReentrantMonitor.h"

class nsHtml5StreamListener : public nsIThreadRetargetableStreamListener {
 public:
  explicit nsHtml5StreamListener(nsHtml5StreamParser* aDelegate);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSITHREADRETARGETABLESTREAMLISTENER

  nsHtml5StreamParser* GetDelegate();

  void DropDelegate();

 private:
  void DropDelegateImpl();
  virtual ~nsHtml5StreamListener();

  mozilla::ReentrantMonitor mDelegateMonitor MOZ_UNANNOTATED;
  mozilla::Atomic<nsHtml5StreamParser*, mozilla::ReleaseAcquire> mDelegate;
};

#endif  // nsHtml5StreamListener_h
