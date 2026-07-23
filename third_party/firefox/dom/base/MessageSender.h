/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MessageSender_h
#define mozilla_dom_MessageSender_h

#include "mozilla/dom/MessageListenerManager.h"

namespace mozilla::dom {

class MessageBroadcaster;

class MessageSender : public MessageListenerManager {
 public:
  void InitWithCallback(ipc::MessageManagerCallback* aCallback);

 protected:
  MessageSender(ipc::MessageManagerCallback* aCallback,
                MessageBroadcaster* aParentManager, MessageManagerFlags aFlags)
      : MessageListenerManager(aCallback, aParentManager, aFlags) {}
};

}  

#endif  // mozilla_dom_MessageSender_h
