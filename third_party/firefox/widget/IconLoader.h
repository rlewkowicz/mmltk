/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_IconLoader_h_
#define mozilla_widget_IconLoader_h_

#include "imgINotificationObserver.h"
#include "mozilla/RefPtr.h"
#include "nsCOMPtr.h"
#include "nsISupports.h"

class nsIURI;
class nsINode;
class imgRequestProxy;
class imgIContainer;

namespace mozilla::widget {


class IconLoader : public imgINotificationObserver {
 public:
  class Listener {
   public:
    virtual nsresult OnComplete(imgIContainer* aContainer) = 0;
  };

  explicit IconLoader(Listener* aListener);

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_IMGINOTIFICATIONOBSERVER

  nsresult LoadIcon(nsIURI* aIconURI, nsINode* aNode,
                    bool aIsInternalIcon = false);

  void Destroy();

 protected:
  virtual ~IconLoader();

 private:
  RefPtr<imgRequestProxy> mIconRequest;

  Listener* mListener;
};

}  
#endif  // mozilla_widget_IconLoader_h_
