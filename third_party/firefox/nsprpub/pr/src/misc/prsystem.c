/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "primpl.h"
#include "prsystem.h"
#include "prprf.h"
#include "prlong.h"

#if defined(FREEBSD) || defined(NETBSD) || defined(OPENBSD) || \
    defined(DRAGONFLY) || 0
#  define _PR_HAVE_SYSCTL
#  include <sys/param.h>
#  include <sys/sysctl.h>
#endif



#if defined(XP_UNIX)
#  include <unistd.h>
#  include <sys/utsname.h>
#endif

#if defined(LINUX)
#  include <string.h>
#  include <ctype.h>
#  define MAX_LINE 512
#endif


PR_IMPLEMENT(char) PR_GetDirectorySeparator(void) {
  return PR_DIRECTORY_SEPARATOR;
} 

PR_IMPLEMENT(char) PR_GetDirectorySepartor(void) {
#if defined(DEBUG)
  static PRBool warn = PR_TRUE;
  if (warn) {
    warn =
        _PR_Obsolete("PR_GetDirectorySepartor()", "PR_GetDirectorySeparator()");
  }
#endif
  return PR_GetDirectorySeparator();
} 

PR_IMPLEMENT(char) PR_GetPathSeparator(void) {
  return PR_PATH_SEPARATOR;
} 

PR_IMPLEMENT(PRStatus)
PR_GetSystemInfo(PRSysInfo cmd, char* buf, PRUint32 buflen) {
  PRUintn len = 0;

  if (!_pr_initialized) {
    _PR_ImplicitInitialization();
  }

  switch (cmd) {
    case PR_SI_HOSTNAME:
    case PR_SI_HOSTNAME_UNTRUNCATED:
      if (PR_FAILURE == _PR_MD_GETHOSTNAME(buf, (PRUintn)buflen)) {
        return PR_FAILURE;
      }

      if (cmd == PR_SI_HOSTNAME_UNTRUNCATED) {
        break;
      }
#if !defined(_PR_GET_HOST_ADDR_AS_NAME)
      while (buf[len] && (len < buflen)) {
        if (buf[len] == '.') {
          buf[len] = '\0';
          break;
        }
        len += 1;
      }
#endif
      break;

    case PR_SI_SYSNAME:
#if defined(XP_UNIX) || 0
      if (PR_FAILURE == _PR_MD_GETSYSINFO(cmd, buf, (PRUintn)buflen)) {
        return PR_FAILURE;
      }
#else
      (void)PR_snprintf(buf, buflen, _PR_SI_SYSNAME);
#endif
      break;

    case PR_SI_RELEASE:
#if defined(XP_UNIX) || 0
      if (PR_FAILURE == _PR_MD_GETSYSINFO(cmd, buf, (PRUintn)buflen)) {
        return PR_FAILURE;
      }
#endif
      break;

    case PR_SI_RELEASE_BUILD:
#if defined(XP_UNIX) || 0
      if (PR_FAILURE == _PR_MD_GETSYSINFO(cmd, buf, (PRUintn)buflen)) {
        return PR_FAILURE;
      }
#else
      if (buflen) {
        *buf = 0;
      }
#endif
      break;

    case PR_SI_ARCHITECTURE:
      (void)PR_snprintf(buf, buflen, _PR_SI_ARCHITECTURE);
      break;
    default:
      PR_SetError(PR_INVALID_ARGUMENT_ERROR, 0);
      return PR_FAILURE;
  }
  return PR_SUCCESS;
}

PR_IMPLEMENT(PRInt32) PR_GetNumberOfProcessors(void) {
  PRInt32 numCpus;
#if defined(_PR_HAVE_SYSCTL)
  int mib[2];
  int rc;
  size_t len = sizeof(numCpus);

  mib[0] = CTL_HW;
#if defined(HW_NCPUONLINE)
  mib[1] = HW_NCPUONLINE;
#else
  mib[1] = HW_NCPU;
#endif
  rc = sysctl(mib, 2, &numCpus, &len, NULL, 0);
  if (-1 == rc) {
    numCpus = -1; 
    _PR_MD_MAP_DEFAULT_ERROR(_MD_ERRNO());
  }
#elif defined(RISCOS)
  numCpus = 1;
#elif defined(LINUX)
  char buf[MAX_LINE];
  FILE* fin;
  const char* cpu_present = "/sys/devices/system/cpu/present";
  size_t strsize;
  numCpus = 0;
  fin = fopen(cpu_present, "r");
  if (fin != NULL) {
    if (fgets(buf, MAX_LINE, fin) != NULL) {
      if (buf[0] == '0') {
        strsize = strlen(buf);
        if (strsize == 1) {
          numCpus = 1;
        } else if (strsize >= 3 && strsize <= 5) {
          if (buf[1] == '-' && isdigit(buf[2])) {
            numCpus = 1 + atoi(buf + 2);
          }
        }
      }
    }
    fclose(fin);
  }
  if (!numCpus) {
    numCpus = sysconf(_SC_NPROCESSORS_CONF);
  }
#elif defined(XP_UNIX)
  numCpus = sysconf(_SC_NPROCESSORS_CONF);
#else
#  error "An implementation is required"
#endif
  return (numCpus);
} 

PR_IMPLEMENT(PRUint64) PR_GetPhysicalMemorySize(void) {
  PRUint64 bytes = 0;

#if defined(LINUX) || 0

  long pageSize = sysconf(_SC_PAGESIZE);
  long pageCount = sysconf(_SC_PHYS_PAGES);
  if (pageSize >= 0 && pageCount >= 0) {
    bytes = (PRUint64)pageSize * pageCount;
  }

#elif defined(NETBSD) || defined(OPENBSD) || defined(FREEBSD) || \
    defined(DRAGONFLY)

  int mib[2];
  int rc;
#if defined(HW_PHYSMEM64)
  uint64_t memSize;
#else
  unsigned long memSize;
#endif
  size_t len = sizeof(memSize);

  mib[0] = CTL_HW;
#if defined(HW_PHYSMEM64)
  mib[1] = HW_PHYSMEM64;
#else
  mib[1] = HW_PHYSMEM;
#endif
  rc = sysctl(mib, 2, &memSize, &len, NULL, 0);
  if (-1 != rc) {
    bytes = memSize;
  }

#else

  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);

#endif

  return bytes;
} 
