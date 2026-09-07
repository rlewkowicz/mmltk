/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(nsDirectoryServiceDefs_h_)
#define nsDirectoryServiceDefs_h_


#define NS_OS_HOME_DIR "Home"

#define NS_OS_TEMP_DIR "TmpD"
#define NS_OS_CURRENT_WORKING_DIR "CurWorkD"
#define NS_OS_DESKTOP_DIR "Desk"
#define NS_OS_DOCUMENTS_DIR "Docs"

#define NS_OS_DEFAULT_DOWNLOAD_DIR "DfltDwnld"

#define NS_OS_CURRENT_PROCESS_DIR "CurProcD"

#define NS_XPCOM_CURRENT_PROCESS_DIR "XCurProcD"

#define NS_XPCOM_LIBRARY_FILE "XpcomLib"

#define NS_GRE_DIR "GreD"

#define NS_GRE_BIN_DIR "GreBinD"


#if !defined(XP_UNIX) || 0
#  define NS_OS_SYSTEM_DIR "SysD"
#endif

#if defined(XP_UNIX)
#  define NS_UNIX_HOME_DIR NS_OS_HOME_DIR
#endif

#if defined(MOZ_WIDGET_GTK)
#  define NS_OS_SYSTEM_CONFIG_DIR "SysConfD"
#endif

#endif
