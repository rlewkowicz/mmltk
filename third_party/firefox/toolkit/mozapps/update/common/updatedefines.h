/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(UPDATEDEFINES_H)
#define UPDATEDEFINES_H

#include <stdio.h>
#include <stdarg.h>
#include "readstrings.h"

#  include <sys/wait.h>
#  include <unistd.h>

#if defined(HAVE_FTS_H)
#    include <fts.h>
#else
#    include <sys/stat.h>
#endif
#  include <dirent.h>


#  define LOG_S "%s"
#  define NS_T(str) str
#  define NS_SLASH NS_T('/')
#  define NS_tsnprintf snprintf
#  define NS_taccess access
#  define NS_tatoi atoi
#  define NS_tchdir chdir
#  define NS_tchmod chmod
#  define NS_tfopen fopen
#  define NS_tmkdir mkdir
#  define NS_tpid int
#  define NS_tremove remove
#  define NS_trename rename
#  define NS_trmdir rmdir
#  define NS_tstat stat
#  define NS_tstat_t stat
#  define NS_tlstat lstat
#  define NS_tstrcat strcat
#  define NS_tstrcmp strcmp
#  define NS_tstricmp strcasecmp
#  define NS_tstrncmp strncmp
#  define NS_tstrcpy strcpy
#  define NS_tstrncpy strncpy
#  define NS_tstrlen strlen
#  define NS_tstrrchr strrchr
#  define NS_tstrstr strstr
#  define NS_tDIR DIR
#  define NS_tdirent dirent
#  define NS_topendir opendir
#  define NS_tclosedir closedir
#  define NS_treaddir readdir

#define BACKUP_EXT NS_T(".moz-backup")

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

static inline bool NS_tvsnprintf(NS_tchar* dest, size_t count,
                                 const NS_tchar* fmt, ...) {
  va_list varargs;
  va_start(varargs, fmt);
  int result = vsnprintf(dest, count, fmt, varargs);
  va_end(varargs);
  return result >= 0 && (size_t)result < count;
}

#endif
