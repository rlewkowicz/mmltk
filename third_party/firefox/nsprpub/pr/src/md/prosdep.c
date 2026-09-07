/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "prbit.h"
#include "prsystem.h"

#if defined(XP_UNIX)
#  include <unistd.h>
#endif

PRInt32 _pr_pageShift;
PRInt32 _pr_pageSize;

static void GetPageSize(void) {
  PRInt32 pageSize;

#if defined(XP_UNIX)
#if 0 || defined LINUX || defined __GNU__ || defined __GLIBC__ || \
      defined FREEBSD || defined NETBSD || defined OPENBSD || 0
  _pr_pageSize = getpagesize();
#else
  _pr_pageSize = sysconf(_SC_PAGESIZE);
#endif
#endif

#if defined(XP_PC)
  _pr_pageSize = 4096;
#endif

  pageSize = _pr_pageSize;
  PR_CEILING_LOG2(_pr_pageShift, pageSize);
}

PR_IMPLEMENT(PRInt32) PR_GetPageShift(void) {
  if (!_pr_pageSize) {
    GetPageSize();
  }
  return _pr_pageShift;
}

PR_IMPLEMENT(PRInt32) PR_GetPageSize(void) {
  if (!_pr_pageSize) {
    GetPageSize();
  }
  return _pr_pageSize;
}
