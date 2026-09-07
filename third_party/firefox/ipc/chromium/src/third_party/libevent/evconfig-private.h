#if !defined(EVCONFIG_PRIVATE_H_INCLUDED_)
#define EVCONFIG_PRIVATE_H_INCLUDED_

#if !defined(_ALL_SOURCE)
# define _ALL_SOURCE 1
#endif
#if !defined(_GNU_SOURCE)
# define _GNU_SOURCE 1
#endif
#if !defined(_POSIX_PTHREAD_SEMANTICS)
# define _POSIX_PTHREAD_SEMANTICS 1
#endif
#if !defined(_TANDEM_SOURCE)
# define _TANDEM_SOURCE 1
#endif
#if !defined(__EXTENSIONS__)
# define __EXTENSIONS__ 1
#endif


#if !defined(_MINIX)
#endif

#if !defined(_POSIX_1_SOURCE)
#endif

#if !defined(_POSIX_SOURCE)
#endif

#if defined(__QNX__)
#if !defined(__EXT_POSIX2)
#  define __EXT_POSIX2
#endif
#endif

#endif
