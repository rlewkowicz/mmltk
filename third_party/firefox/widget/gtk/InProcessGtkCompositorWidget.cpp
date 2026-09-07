/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/widget/PlatformWidgetTypes.h"

#include "InProcessGtkCompositorWidget.h"
#include "VsyncDispatcher.h"
#include "nsWindow.h"

namespace mozilla::widget {

RefPtr<CompositorWidget> CompositorWidget::CreateLocal(
    const CompositorWidgetInitData& aInitData,
    const layers::CompositorOptions& aOptions, nsIWidget* aWidget) {
  return new InProcessGtkCompositorWidget(
      aInitData.get_GtkCompositorWidgetInitData(), aOptions,
      nsWindow::FromWidget(aWidget));
}

InProcessGtkCompositorWidget::InProcessGtkCompositorWidget(
    const GtkCompositorWidgetInitData& aInitData,
    const layers::CompositorOptions& aOptions, nsWindow* aWindow)
    : GtkCompositorWidget(aInitData, aOptions, aWindow) {}

void InProcessGtkCompositorWidget::ObserveVsync(VsyncObserver* aObserver) {
  if (RefPtr<CompositorVsyncDispatcher> cvd =
          mWidget->GetCompositorVsyncDispatcher()) {
    cvd->SetCompositorVsyncObserver(aObserver);
  }
}

}  
