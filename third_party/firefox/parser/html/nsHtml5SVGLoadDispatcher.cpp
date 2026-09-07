/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsHtml5SVGLoadDispatcher.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "nsPresContext.h"

using namespace mozilla;

nsHtml5SVGLoadDispatcher::nsHtml5SVGLoadDispatcher(nsIContent* aElement)
    : Runnable("nsHtml5SVGLoadDispatcher"),
      mElement(aElement),
      mDocument(mElement->OwnerDoc()) {
  mDocument->BlockOnload();
}

MOZ_CAN_RUN_SCRIPT_BOUNDARY NS_IMETHODIMP nsHtml5SVGLoadDispatcher::Run() {
  WidgetEvent event(true, eSVGLoad);
  event.mFlags.mBubbles = false;
  RefPtr<nsPresContext> ctx = mElement->OwnerDoc()->GetPresContext();
  EventDispatcher::Dispatch(mElement, ctx, &event);
  mDocument->UnblockOnload(false);
  return NS_OK;
}
