/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(pratom_h___)
#define pratom_h___

#include "prtypes.h"
#include "prlock.h"

PR_BEGIN_EXTERN_C

NSPR_API(PRInt32)   PR_AtomicIncrement(PRInt32 *val);

NSPR_API(PRInt32)   PR_AtomicDecrement(PRInt32 *val);

NSPR_API(PRInt32) PR_AtomicSet(PRInt32 *val, PRInt32 newval);

NSPR_API(PRInt32)   PR_AtomicAdd(PRInt32 *ptr, PRInt32 val);

#if ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)) && \
      ((0 && \
           (defined(__ppc__) || defined(__i386__) || defined(__x86_64__))) || \
       (defined(__linux__) && \
           ((defined(__i386__) && \
           defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)) || \
           defined(__ia64__) || defined(__x86_64__) || \
           defined(__powerpc__) || \
           (defined(__arm__) && \
           defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)) || \
           defined(__aarch64__) || defined(__alpha) || \
           (defined(__riscv) && \
           defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)) || \
           (defined(__mips__) && \
           defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)))))


#define PR_ATOMIC_INCREMENT(val) __sync_add_and_fetch(val, 1)
#define PR_ATOMIC_DECREMENT(val) __sync_sub_and_fetch(val, 1)
#define PR_ATOMIC_SET(val, newval) __sync_lock_test_and_set(val, newval)
#define PR_ATOMIC_ADD(ptr, val) __sync_add_and_fetch(ptr, val)

#else

#define PR_ATOMIC_INCREMENT(val) PR_AtomicIncrement(val)
#define PR_ATOMIC_DECREMENT(val) PR_AtomicDecrement(val)
#define PR_ATOMIC_SET(val, newval) PR_AtomicSet(val, newval)
#define PR_ATOMIC_ADD(ptr, val) PR_AtomicAdd(ptr, val)

#endif

typedef struct PRStackElemStr PRStackElem;

struct PRStackElemStr {
    PRStackElem *prstk_elem_next;   
};

typedef struct PRStackStr PRStack;

NSPR_API(PRStack *) PR_CreateStack(const char *stack_name);

NSPR_API(void)          PR_StackPush(PRStack *stack, PRStackElem *stack_elem);

NSPR_API(PRStackElem *) PR_StackPop(PRStack *stack);

NSPR_API(PRStatus)      PR_DestroyStack(PRStack *stack);

PR_END_EXTERN_C

#endif
