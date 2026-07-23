/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNativeAppSupportBase.h"
#include "nsComponentManagerUtils.h"
#include "nsCOMPtr.h"
#include "nsXPCOM.h"
#include "nsISupportsPrimitives.h"
#include "nsIObserverService.h"
#include "nsIAppStartup.h"
#include "nsServiceManagerUtils.h"
#include "prlink.h"
#include "nsXREDirProvider.h"
#include "nsReadableUtils.h"

#include "nsIFile.h"
#include "nsDirectoryServiceDefs.h"
#include "nsPIDOMWindow.h"
#include "nsIWidget.h"
#include "mozilla/Logging.h"
#include "mozilla/Services.h"
#include "mozilla/XREAppData.h"

#include <stdlib.h>
#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>


#ifdef MOZ_ENABLE_DBUS
#  include <dbus/dbus.h>
#endif


class nsNativeAppSupportUnix : public nsNativeAppSupportBase {
 public:
  NS_IMETHOD Start(bool* aRetVal) override;
  NS_IMETHOD Enable() override;

 private:
};



NS_IMETHODIMP
nsNativeAppSupportUnix::Start(bool* aRetVal) {
  NS_ASSERTION(gAppData, "gAppData must not be null.");

#ifdef MOZ_ENABLE_DBUS
  dbus_threads_init_default();
#endif

  *aRetVal = true;


  return NS_OK;
}

NS_IMETHODIMP
nsNativeAppSupportUnix::Enable() { return NS_OK; }

nsresult NS_CreateNativeAppSupport(nsINativeAppSupport** aResult) {
  nsNativeAppSupportBase* native = new nsNativeAppSupportUnix();
  if (!native) return NS_ERROR_OUT_OF_MEMORY;

  *aResult = native;
  NS_ADDREF(*aResult);

  return NS_OK;
}
