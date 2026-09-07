/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsGDKErrorHandler.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nsDebug.h"
#include "nsString.h"

#include "prenv.h"

static void GdkErrorHandler(const gchar* log_domain, GLogLevelFlags log_level,
                            const gchar* message, gpointer user_data) {
  {
    g_log_default_handler(log_domain, log_level, message, user_data);
    MOZ_CRASH_UNSAFE(message);
  }
}

void InstallGdkErrorHandler() {
  g_log_set_handler("Gdk",
                    (GLogLevelFlags)(G_LOG_LEVEL_ERROR | G_LOG_FLAG_FATAL |
                                     G_LOG_FLAG_RECURSION),
                    GdkErrorHandler, nullptr);
}
