#if !defined(ASMNAMES_H)
#define ASMNAMES_H

#define C2(X, Y)  X ## Y
#define C1(X, Y)  C2(X, Y)
#if defined(__USER_LABEL_PREFIX__)
# define C(X)     C1(__USER_LABEL_PREFIX__, X)
#else
# define C(X)     X
#endif

# define L(X)     C1(.L, X)

#if defined(__ELF__) && defined(__PIC__)
# define PLT(X)	  X@PLT
#else
# define PLT(X)	  X
#endif

#if defined(__ELF__)
# define ENDF(X)  .type	X,@function; .size X, . - X
#else
# define ENDF(X)
#endif

#endif
