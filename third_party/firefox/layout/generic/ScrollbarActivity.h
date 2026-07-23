/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ScrollbarActivity_h_
#define ScrollbarActivity_h_

#include "mozilla/Assertions.h"
#include "nsCOMPtr.h"
#include "nsIDOMEventListener.h"

class nsIContent;
class nsIScrollbarMediator;
class nsITimer;

namespace mozilla {

namespace dom {
class Element;
class EventTarget;
}  

namespace layout {


class ScrollbarActivity final : public nsIDOMEventListener {
 public:
  explicit ScrollbarActivity(nsIScrollbarMediator* aScrollableFrame)
      : mScrollableFrame(aScrollableFrame) {
    MOZ_ASSERT(mScrollableFrame);
  }

  NS_DECL_ISUPPORTS
  NS_DECL_NSIDOMEVENTLISTENER

  void Destroy();

  void ActivityOccurred();
  void ActivityStarted();
  void ActivityStopped();

  bool IsActive() const { return mNestedActivityCounter; }

 protected:
  virtual ~ScrollbarActivity() = default;

  void StartFadeTimer();
  void CancelFadeTimer();
  void BeginFade();

  void StartListeningForScrollAreaEvents();
  void StopListeningForScrollAreaEvents();

  dom::Element* GetScrollbarContent(bool aVertical);
  dom::Element* GetHorizontalScrollbar() { return GetScrollbarContent(false); }
  dom::Element* GetVerticalScrollbar() { return GetScrollbarContent(true); }

  nsIScrollbarMediator* const mScrollableFrame;
  nsCOMPtr<nsITimer> mFadeTimer;
  uint32_t mNestedActivityCounter = 0;
  bool mScrollbarEffectivelyVisible = false;
  bool mListeningForScrollAreaEvents = false;
};

}  
}  

#endif /* ScrollbarActivity_h_ */
