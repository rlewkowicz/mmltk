/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2002 University of Southern California
 * Copyright © 2005,2007 Red Hat, Inc.
 * Copyright © 2007 Mathias Hasselmann
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is University of Southern
 * California.
 *
 * Contributor(s):
 *	Carl D. Worth <cworth@cworth.org>
 *	Mathias Hasselmann <mathias.hasselmann@gmx.de>
 *	Behdad Esfahbod <behdad@behdad.org>
 */

#if !defined(CAIRO_MUTEX_IMPL_PRIVATE_H)
#define CAIRO_MUTEX_IMPL_PRIVATE_H

#include "cairo.h"

#include "config.h"

#if HAVE_LOCKDEP
#include <lockdep.h>
#endif

#define CAIRO_MUTEX_IMPL_NOOP	do {/*no-op*/} while (0)
#define CAIRO_MUTEX_IMPL_NOOP1(expr)        do { (void)(expr); } while (0)


#if CAIRO_NO_MUTEX


  typedef int cairo_mutex_impl_t;

# define CAIRO_MUTEX_IMPL_NO 1
# define CAIRO_MUTEX_IMPL_INITIALIZE() CAIRO_MUTEX_IMPL_NOOP
# define CAIRO_MUTEX_IMPL_LOCK(mutex) CAIRO_MUTEX_IMPL_NOOP1(mutex)
# define CAIRO_MUTEX_IMPL_TRY_LOCK(mutex) ((mutex), TRUE)
# define CAIRO_MUTEX_IMPL_UNLOCK(mutex) CAIRO_MUTEX_IMPL_NOOP1(mutex)
# define CAIRO_MUTEX_IMPL_NIL_INITIALIZER 0

# define CAIRO_MUTEX_HAS_RECURSIVE_IMPL 1

  typedef int cairo_recursive_mutex_impl_t;

# define CAIRO_RECURSIVE_MUTEX_IMPL_INIT(mutex)
# define CAIRO_RECURSIVE_MUTEX_IMPL_NIL_INITIALIZER 0

#elif CAIRO_HAS_PTHREAD /* and finally if there are no native mutexes ********/

# include <pthread.h>

  typedef pthread_mutex_t cairo_mutex_impl_t;
  typedef pthread_mutex_t cairo_recursive_mutex_impl_t;

# define CAIRO_MUTEX_IMPL_PTHREAD 1
#if HAVE_LOCKDEP
# define CAIRO_MUTEX_IMPL_INIT(mutex) pthread_mutex_init (&(mutex), NULL)
#endif
# define CAIRO_MUTEX_IMPL_LOCK(mutex) pthread_mutex_lock (&(mutex))
# define CAIRO_MUTEX_IMPL_TRY_LOCK(mutex) (pthread_mutex_trylock (&(mutex)) == 0)
# define CAIRO_MUTEX_IMPL_UNLOCK(mutex) pthread_mutex_unlock (&(mutex))
#if HAVE_LOCKDEP
# define CAIRO_MUTEX_IS_LOCKED(mutex) LOCKDEP_IS_LOCKED (&(mutex))
# define CAIRO_MUTEX_IS_UNLOCKED(mutex) LOCKDEP_IS_UNLOCKED (&(mutex))
#endif
# define CAIRO_MUTEX_IMPL_FINI(mutex) pthread_mutex_destroy (&(mutex))
#if ! HAVE_LOCKDEP
# define CAIRO_MUTEX_IMPL_FINALIZE() CAIRO_MUTEX_IMPL_NOOP
#endif
# define CAIRO_MUTEX_IMPL_NIL_INITIALIZER PTHREAD_MUTEX_INITIALIZER

# define CAIRO_MUTEX_HAS_RECURSIVE_IMPL 1
# define CAIRO_RECURSIVE_MUTEX_IMPL_INIT(mutex) do { \
    pthread_mutexattr_t attr; \
    pthread_mutexattr_init (&attr); \
    pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE); \
    pthread_mutex_init (&(mutex), &attr); \
    pthread_mutexattr_destroy (&attr); \
} while (0)
# define CAIRO_RECURSIVE_MUTEX_IMPL_NIL_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP

#else

# error "XXX: No mutex implementation found.  Cairo will not work with multiple threads.  Define CAIRO_NO_MUTEX to 1 to acknowledge and accept this limitation and compile cairo without thread-safety support."

#endif

#if ! CAIRO_MUTEX_HAS_RECURSIVE_IMPL

# define CAIRO_MUTEX_HAS_RECURSIVE_IMPL 1

  typedef cairo_mutex_impl_t cairo_recursive_mutex_impl_t;

# define CAIRO_RECURSIVE_MUTEX_IMPL_INIT(mutex) CAIRO_MUTEX_IMPL_INIT(mutex)
# define CAIRO_RECURSIVE_MUTEX_IMPL_NIL_INITIALIZER CAIRO_MUTEX_IMPL_NIL_INITIALIZER

#endif

#endif
