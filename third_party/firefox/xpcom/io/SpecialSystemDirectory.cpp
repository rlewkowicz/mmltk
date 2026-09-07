/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpecialSystemDirectory.h"
#include "mozilla/Try.h"
#include "nsComponentManagerUtils.h"
#include "nsString.h"
#include "nsDependentString.h"
#include "nsIXULAppInfo.h"

#if defined(XP_UNIX)

#  include <limits.h>
#  include <unistd.h>
#  include <stdlib.h>
#  include <sys/param.h>
#  include "prenv.h"
#if defined(MOZ_WIDGET_GTK)
#    include "mozilla/WidgetUtilsGtk.h"
#endif

#endif

#if !defined(MAXPATHLEN)
#if defined(PATH_MAX)
#    define MAXPATHLEN PATH_MAX
#elif defined(MAX_PATH)
#    define MAXPATHLEN MAX_PATH
#elif defined(_MAX_PATH)
#    define MAXPATHLEN _MAX_PATH
#elif defined(CCHMAXPATH)
#    define MAXPATHLEN CCHMAXPATH
#else
#    define MAXPATHLEN 1024
#endif
#endif


#if defined(XP_UNIX)
static nsresult GetUnixHomeDir(nsIFile** aFile) {
  return NS_NewNativeLocalFile(nsDependentCString(PR_GetEnv("HOME")), aFile);
}

static nsresult GetUnixSystemConfigDir(nsIFile** aFile) {
  nsAutoCString appName;
  if (nsCOMPtr<nsIXULAppInfo> appInfo =
          do_GetService("@mozilla.org/xre/app-info;1")) {
    MOZ_TRY(appInfo->GetName(appName));
  } else {
    appName.AssignLiteral(MOZ_APP_BASENAME);
  }

  ToLowerCase(appName);

  nsDependentCString sysConfigDir;
#if defined(MOZ_WIDGET_GTK)
  if (sysConfigDir.IsEmpty() && mozilla::widget::IsRunningUnderFlatpak()) {
    sysConfigDir.Assign(nsLiteralCString("/app/etc"));
  }
#endif
  if (sysConfigDir.IsEmpty()) {
    sysConfigDir.Assign(nsLiteralCString("/etc"));
  }
  MOZ_TRY(NS_NewNativeLocalFile(sysConfigDir, aFile));
  MOZ_TRY((*aFile)->AppendNative(appName));
  return NS_OK;
}

/*
  The following license applies to the xdg_user_dir_lookup function:

  Copyright (c) 2007 Red Hat, Inc.

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

static char* xdg_user_dir_lookup(const char* aType) {
  FILE* file;
  char* home_dir;
  char* config_home;
  char* config_file;
  char buffer[512];
  char* user_dir;
  char* p;
  char* d;
  int len;
  int relative;

  home_dir = getenv("HOME");

  if (!home_dir) {
    goto error;
  }

  config_home = getenv("XDG_CONFIG_HOME");
  if (!config_home || config_home[0] == 0) {
    config_file =
        (char*)malloc(strlen(home_dir) + strlen("/.config/user-dirs.dirs") + 1);
    if (!config_file) {
      goto error;
    }

    strcpy(config_file, home_dir);
    strcat(config_file, "/.config/user-dirs.dirs");
  } else {
    config_file =
        (char*)malloc(strlen(config_home) + strlen("/user-dirs.dirs") + 1);
    if (!config_file) {
      goto error;
    }

    strcpy(config_file, config_home);
    strcat(config_file, "/user-dirs.dirs");
  }

  file = fopen(config_file, "r");
  free(config_file);
  if (!file) {
    goto error;
  }

  user_dir = nullptr;
  while (fgets(buffer, sizeof(buffer), file)) {
    len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
      buffer[len - 1] = 0;
    }

    p = buffer;
    while (*p == ' ' || *p == '\t') {
      p++;
    }

    if (strncmp(p, "XDG_", 4) != 0) {
      continue;
    }
    p += 4;
    if (strncmp(p, aType, strlen(aType)) != 0) {
      continue;
    }
    p += strlen(aType);
    if (strncmp(p, "_DIR", 4) != 0) {
      continue;
    }
    p += 4;

    while (*p == ' ' || *p == '\t') {
      p++;
    }

    if (*p != '=') {
      continue;
    }
    p++;

    while (*p == ' ' || *p == '\t') {
      p++;
    }

    if (*p != '"') {
      continue;
    }
    p++;

    relative = 0;
    if (strncmp(p, "$HOME/", 6) == 0) {
      p += 6;
      relative = 1;
    } else if (*p != '/') {
      continue;
    }

    if (relative) {
      user_dir = (char*)malloc(strlen(home_dir) + 1 + strlen(p) + 1);
      if (!user_dir) {
        goto error2;
      }

      strcpy(user_dir, home_dir);
      strcat(user_dir, "/");
    } else {
      user_dir = (char*)malloc(strlen(p) + 1);
      if (!user_dir) {
        goto error2;
      }

      *user_dir = 0;
    }

    d = user_dir + strlen(user_dir);
    while (*p && *p != '"') {
      if ((*p == '\\') && (*(p + 1) != 0)) {
        p++;
      }
      *d++ = *p++;
    }
    *d = 0;
  }
error2:
  fclose(file);

  if (user_dir) {
    return user_dir;
  }

error:
  return nullptr;
}

static const char xdg_user_dirs[] =
    "DESKTOP\0"
    "DOCUMENTS\0"
    "DOWNLOAD\0"
    "MUSIC\0"
    "PICTURES\0"
    "PUBLICSHARE\0"
    "TEMPLATES\0"
    "VIDEOS";

static const uint8_t xdg_user_dir_offsets[] = {0, 8, 18, 27, 33, 42, 54, 64};

static nsresult GetUnixXDGUserDirectory(SystemDirectories aSystemDirectory,
                                        nsIFile** aFile) {
  char* dir = xdg_user_dir_lookup(
      xdg_user_dirs +
      xdg_user_dir_offsets[aSystemDirectory - Unix_XDG_Desktop]);

  nsresult rv;
  nsCOMPtr<nsIFile> file;
  bool exists;
  if (dir) {
    rv = NS_NewNativeLocalFile(nsDependentCString(dir), getter_AddRefs(file));
    free(dir);

    if (NS_FAILED(rv)) {
      return rv;
    }

    rv = file->Exists(&exists);
    if (NS_FAILED(rv)) {
      return rv;
    }

    if (!exists) {
      rv = file->Create(nsIFile::DIRECTORY_TYPE, 0755);
      if (NS_FAILED(rv)) {
        return rv;
      }
    }
  } else if (Unix_XDG_Desktop == aSystemDirectory) {
    nsCOMPtr<nsIFile> home;
    rv = GetUnixHomeDir(getter_AddRefs(home));
    if (NS_FAILED(rv)) {
      return rv;
    }

    rv = home->Clone(getter_AddRefs(file));
    if (NS_FAILED(rv)) {
      return rv;
    }

    rv = file->AppendNative("Desktop"_ns);
    if (NS_FAILED(rv)) {
      return rv;
    }

    rv = file->Exists(&exists);
    if (NS_FAILED(rv)) {
      return rv;
    }

    if (!exists) {
      file = std::move(home);
    }
  } else {
    return NS_ERROR_FAILURE;
  }

  *aFile = nullptr;
  file.swap(*aFile);

  return NS_OK;
}
#endif

nsresult GetSpecialSystemDirectory(SystemDirectories aSystemSystemDirectory,
                                   nsIFile** aFile) {
  char path[MAXPATHLEN];

  switch (aSystemSystemDirectory) {
    case OS_CurrentWorkingDirectory:
      if (!getcwd(path, MAXPATHLEN)) {
        return NS_ERROR_FAILURE;
      }

      return NS_NewNativeLocalFile(nsDependentCString(path), aFile);

    case OS_TemporaryDirectory:
#if defined(XP_UNIX)
    {
      static const char* tPath = nullptr;
      if (!tPath) {
        tPath = PR_GetEnv("TMPDIR");
        if (!tPath || !*tPath) {
          tPath = PR_GetEnv("TMP");
          if (!tPath || !*tPath) {
            tPath = PR_GetEnv("TEMP");
            if (!tPath || !*tPath) {
              tPath = "/tmp/";
            }
          }
        }
      }
      return NS_NewNativeLocalFile(nsDependentCString(tPath), aFile);
    }
#else
      break;
#endif

#if defined(XP_UNIX)
    case Unix_HomeDirectory:
      return GetUnixHomeDir(aFile);

    case Unix_XDG_Desktop:
    case Unix_XDG_Documents:
    case Unix_XDG_Download:
      return GetUnixXDGUserDirectory(aSystemSystemDirectory, aFile);

    case Unix_SystemConfigDirectory:
      return GetUnixSystemConfigDir(aFile);
#endif

    default:
      break;
  }
  return NS_ERROR_NOT_AVAILABLE;
}

nsresult GetSpecialSystemDirectoryList(
    SystemDirectoryLists aSystemDirectoryLists,
    nsCOMArray<nsIFile>& aDirectories) {
  switch (aSystemDirectoryLists) {
    default:
      break;
  }
  return NS_ERROR_NOT_AVAILABLE;
}
