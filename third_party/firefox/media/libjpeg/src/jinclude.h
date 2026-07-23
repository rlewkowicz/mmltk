/*
 * jinclude.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1994, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2022-2023, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file exists to provide a single place to fix any problems with
 * including the wrong system include files.  (Common problems are taken
 * care of by the standard jconfig symbols, but on really weird systems
 * you may have to edit this file.)
 *
 * NOTE: this file is NOT intended to be included by applications using the
 * JPEG library.  Most applications need only include jpeglib.h.
 */

#if !defined(__JINCLUDE_H__)
#define __JINCLUDE_H__


#include "jconfig.h"            /* auto configuration options */
#include "jconfigint.h"
#define JCONFIG_INCLUDED        /* so that jpeglib.h doesn't do it again */


#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>



#if defined(_MSC_VER)

#define SNPRINTF(str, n, format, ...) \
  _snprintf_s(str, n, _TRUNCATE, format, ##__VA_ARGS__)

#else

#define SNPRINTF  snprintf

#endif


#if !defined(NO_GETENV)

#if defined(_MSC_VER)

static INLINE int GETENV_S(char *buffer, size_t buffer_size, const char *name)
{
  size_t required_size;

  return (int)getenv_s(&required_size, buffer, buffer_size, name);
}

#else

#include <errno.h>


static INLINE int GETENV_S(char *buffer, size_t buffer_size, const char *name)
{
  char *env;

  if (!buffer) {
    if (buffer_size == 0)
      return 0;
    else
      return (errno = EINVAL);
  }
  if (buffer_size == 0)
    return (errno = EINVAL);
  if (!name) {
    *buffer = 0;
    return 0;
  }

  env = getenv(name);
  if (!env)
  {
    *buffer = 0;
    return 0;
  }

  if (strlen(env) + 1 > buffer_size) {
    *buffer = 0;
    return ERANGE;
  }

  strncpy(buffer, env, buffer_size);

  return 0;
}

#endif

#endif


#if !defined(NO_PUTENV)


#include <errno.h>


static INLINE int PUTENV_S(const char *name, const char *value)
{
  if (!name || !value)
    return (errno = EINVAL);

  setenv(name, value, 1);

  return errno;
}


#endif


#endif
