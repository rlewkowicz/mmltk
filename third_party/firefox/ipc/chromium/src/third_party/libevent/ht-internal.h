/* Copyright 2002 Christopher Clark */
/* Copyright 2005-2012 Nick Mathewson */
/* Copyright 2009-2012 Niels Provos and Nick Mathewson */
/* See license at end. */


#ifndef HT_INTERNAL_H_INCLUDED_
#define HT_INTERNAL_H_INCLUDED_

#define HT_HEAD(name, type)                                             \
  struct name {                                                         \
                                            \
    struct type **hth_table;                                            \
                                       \
    unsigned hth_table_length;                                          \
                         \
    unsigned hth_n_entries;                                             \
     \
    unsigned hth_load_limit;                                            \
                 \
    int hth_prime_idx;                                                  \
  }

#define HT_INITIALIZER()                        \
  { NULL, 0, 0, 0, -1 }

#ifdef HT_NO_CACHE_HASH_VALUES
#define HT_ENTRY(type)                          \
  struct {                                      \
    struct type *hte_next;                      \
  }
#else
#define HT_ENTRY(type)                          \
  struct {                                      \
    struct type *hte_next;                      \
    unsigned hte_hash;                          \
  }
#endif

#define HT_EMPTY(head)                          \
  ((head)->hth_n_entries == 0)

#define HT_SIZE(head)                           \
  ((head)->hth_n_entries)

#define HT_MEM_USAGE(head)                         \
  (sizeof(*head) + (head)->hth_table_length * sizeof(void*))

#define HT_FIND(name, head, elm)     name##_HT_FIND((head), (elm))
#define HT_INSERT(name, head, elm)   name##_HT_INSERT((head), (elm))
#define HT_REPLACE(name, head, elm)  name##_HT_REPLACE((head), (elm))
#define HT_REMOVE(name, head, elm)   name##_HT_REMOVE((head), (elm))
#define HT_START(name, head)         name##_HT_START(head)
#define HT_NEXT(name, head, elm)     name##_HT_NEXT((head), (elm))
#define HT_NEXT_RMV(name, head, elm) name##_HT_NEXT_RMV((head), (elm))
#define HT_CLEAR(name, head)         name##_HT_CLEAR(head)
#define HT_INIT(name, head)          name##_HT_INIT(head)
static inline unsigned
ht_improve_hash_(unsigned h)
{
  h += ~(h << 9);
  h ^=  ((h >> 14) | (h << 18)); 
  h +=  (h << 4);
  h ^=  ((h >> 10) | (h << 22)); 
  return h;
}

#if 0
static inline unsigned
ht_string_hash_(const char *s)
{
  unsigned h = 0;
  int m = 1;
  while (*s) {
    h += ((signed char)*s++)*m;
    m = (m<<5)-1; 
  }
  return h;
}
#endif

static inline unsigned
ht_string_hash_(const char *s)
{
  unsigned h;
  const unsigned char *cp = (const unsigned char *)s;
  h = *cp << 7;
  while (*cp) {
    h = (1000003*h) ^ *cp++;
  }
  h ^= (unsigned)(cp-(const unsigned char*)s);
  return h;
}

#ifndef HT_NO_CACHE_HASH_VALUES
#define HT_SET_HASH_(elm, field, hashfn)        \
	do { (elm)->field.hte_hash = hashfn(elm); } while (0)
#define HT_SET_HASHVAL_(elm, field, val)	\
	do { (elm)->field.hte_hash = (val); } while (0)
#define HT_ELT_HASH_(elm, field, hashfn)	\
	((elm)->field.hte_hash)
#else
#define HT_SET_HASH_(elm, field, hashfn)	\
	((void)0)
#define HT_ELT_HASH_(elm, field, hashfn)	\
	(hashfn(elm))
#define HT_SET_HASHVAL_(elm, field, val)	\
        ((void)0)
#endif

#define HT_BUCKET_(head, field, elm, hashfn)				\
	((head)->hth_table[HT_ELT_HASH_(elm,field,hashfn) % head->hth_table_length])

#define HT_FOREACH(x, name, head)                 \
  for ((x) = HT_START(name, head);                \
       (x) != NULL;                               \
       (x) = HT_NEXT(name, head, x))

#define HT_PROTOTYPE(name, type, field, hashfn, eqfn)                   \
  int name##_HT_GROW(struct name *ht, unsigned min_capacity);           \
  void name##_HT_CLEAR(struct name *ht);                                \
  int name##_HT_REP_IS_BAD_(const struct name *ht);			\
  static inline void                                                    \
  name##_HT_INIT(struct name *head) {                                   \
    head->hth_table_length = 0;                                         \
    head->hth_table = NULL;                                             \
    head->hth_n_entries = 0;                                            \
    head->hth_load_limit = 0;                                           \
    head->hth_prime_idx = -1;                                           \
  }                                                                     \
                       \
  static inline struct type **                                          \
  name##_HT_FIND_P_(struct name *head, struct type *elm)		\
  {                                                                     \
    struct type **p;                                                    \
    if (!head->hth_table)                                               \
      return NULL;                                                      \
    p = &HT_BUCKET_(head, field, elm, hashfn);				\
    while (*p) {                                                        \
      if (eqfn(*p, elm))                                                \
        return p;                                                       \
      p = &(*p)->field.hte_next;                                        \
    }                                                                   \
    return p;                                                           \
  }                                                                     \
                                 \
  static inline struct type *                                           \
  name##_HT_FIND(const struct name *head, struct type *elm)             \
  {                                                                     \
    struct type **p;                                                    \
    struct name *h = (struct name *) head;                              \
    HT_SET_HASH_(elm, field, hashfn);                                   \
    p = name##_HT_FIND_P_(h, elm);					\
    return p ? *p : NULL;                                               \
  }                                                                     \
   \
  static inline void                                                    \
  name##_HT_INSERT(struct name *head, struct type *elm)                 \
  {                                                                     \
    struct type **p;                                                    \
    if (!head->hth_table || head->hth_n_entries >= head->hth_load_limit) \
      name##_HT_GROW(head, head->hth_n_entries+1);                      \
    ++head->hth_n_entries;                                              \
    HT_SET_HASH_(elm, field, hashfn);                                   \
    p = &HT_BUCKET_(head, field, elm, hashfn);				\
    elm->field.hte_next = *p;                                           \
    *p = elm;                                                           \
  }                                                                     \
                                                               \
  static inline struct type *                                           \
  name##_HT_REPLACE(struct name *head, struct type *elm)                \
  {                                                                     \
    struct type **p, *r;                                                \
    if (!head->hth_table || head->hth_n_entries >= head->hth_load_limit) \
      name##_HT_GROW(head, head->hth_n_entries+1);                      \
    HT_SET_HASH_(elm, field, hashfn);                                   \
    p = name##_HT_FIND_P_(head, elm);					\
    r = *p;                                                             \
    *p = elm;                                                           \
    if (r && (r!=elm)) {                                                \
      elm->field.hte_next = r->field.hte_next;                          \
      r->field.hte_next = NULL;                                         \
      return r;                                                         \
    } else {                                                            \
      ++head->hth_n_entries;                                            \
      return NULL;                                                      \
    }                                                                   \
  }                                                                     \
            \
  static inline struct type *                                           \
  name##_HT_REMOVE(struct name *head, struct type *elm)                 \
  {                                                                     \
    struct type **p, *r;                                                \
    HT_SET_HASH_(elm, field, hashfn);                                   \
    p = name##_HT_FIND_P_(head,elm);					\
    if (!p || !*p)                                                      \
      return NULL;                                                      \
    r = *p;                                                             \
    *p = r->field.hte_next;                                             \
    r->field.hte_next = NULL;                                           \
    --head->hth_n_entries;                                              \
    return r;                                                           \
  }                                                                     \
                                               \
  static inline void                                                    \
  name##_HT_FOREACH_FN(struct name *head,                               \
                       int (*fn)(struct type *, void *),                \
                       void *data)                                      \
  {                                                                     \
    unsigned idx;                                                       \
    struct type **p, **nextp, *next;                                    \
    if (!head->hth_table)                                               \
      return;                                                           \
    for (idx=0; idx < head->hth_table_length; ++idx) {                  \
      p = &head->hth_table[idx];                                        \
      while (*p) {                                                      \
        nextp = &(*p)->field.hte_next;                                  \
        next = *nextp;                                                  \
        if (fn(*p, data)) {                                             \
          --head->hth_n_entries;                                        \
          *p = next;                                                    \
        } else {                                                        \
          p = nextp;                                                    \
        }                                                               \
      }                                                                 \
    }                                                                   \
  }                                                                     \
         \
  static inline struct type **                                          \
  name##_HT_START(struct name *head)                                    \
  {                                                                     \
    unsigned b = 0;                                                     \
    while (b < head->hth_table_length) {                                \
      if (head->hth_table[b])                                           \
        return &head->hth_table[b];                                     \
      ++b;                                                              \
    }                                                                   \
    return NULL;                                                        \
  }                                                                     \
                                                                     \
  static inline struct type **                                          \
  name##_HT_NEXT(struct name *head, struct type **elm)                  \
  {                                                                     \
    if ((*elm)->field.hte_next) {                                       \
      return &(*elm)->field.hte_next;                                   \
    } else {                                                            \
      unsigned b = (HT_ELT_HASH_(*elm, field, hashfn) % head->hth_table_length)+1; \
      while (b < head->hth_table_length) {                              \
        if (head->hth_table[b])                                         \
          return &head->hth_table[b];                                   \
        ++b;                                                            \
      }                                                                 \
      return NULL;                                                      \
    }                                                                   \
  }                                                                     \
  static inline struct type **                                          \
  name##_HT_NEXT_RMV(struct name *head, struct type **elm)              \
  {                                                                     \
    unsigned h = HT_ELT_HASH_(*elm, field, hashfn);		        \
    *elm = (*elm)->field.hte_next;                                      \
    --head->hth_n_entries;                                              \
    if (*elm) {                                                         \
      return elm;                                                       \
    } else {                                                            \
      unsigned b = (h % head->hth_table_length)+1;                      \
      while (b < head->hth_table_length) {                              \
        if (head->hth_table[b])                                         \
          return &head->hth_table[b];                                   \
        ++b;                                                            \
      }                                                                 \
      return NULL;                                                      \
    }                                                                   \
  }

#define HT_GENERATE(name, type, field, hashfn, eqfn, load, mallocfn,    \
                    reallocfn, freefn)                                  \
  static unsigned name##_PRIMES[] = {                                   \
    53, 97, 193, 389,                                                   \
    769, 1543, 3079, 6151,                                              \
    12289, 24593, 49157, 98317,                                         \
    196613, 393241, 786433, 1572869,                                    \
    3145739, 6291469, 12582917, 25165843,                               \
    50331653, 100663319, 201326611, 402653189,                          \
    805306457, 1610612741                                               \
  };                                                                    \
  static unsigned name##_N_PRIMES =                                     \
    (unsigned)(sizeof(name##_PRIMES)/sizeof(name##_PRIMES[0]));         \
                                                          \
  int                                                                   \
  name##_HT_GROW(struct name *head, unsigned size)                      \
  {                                                                     \
    unsigned new_len, new_load_limit;                                   \
    int prime_idx;                                                      \
    struct type **new_table;                                            \
    if (head->hth_prime_idx == (int)name##_N_PRIMES - 1)                \
      return 0;                                                         \
    if (head->hth_load_limit > size)                                    \
      return 0;                                                         \
    prime_idx = head->hth_prime_idx;                                    \
    do {                                                                \
      new_len = name##_PRIMES[++prime_idx];                             \
      new_load_limit = (unsigned)(load*new_len);                        \
    } while (new_load_limit <= size &&                                  \
             prime_idx < (int)name##_N_PRIMES);                         \
    if ((new_table = mallocfn(new_len*sizeof(struct type*)))) {         \
      unsigned b;                                                       \
      memset(new_table, 0, new_len*sizeof(struct type*));               \
      for (b = 0; b < head->hth_table_length; ++b) {                    \
        struct type *elm, *next;                                        \
        unsigned b2;                                                    \
        elm = head->hth_table[b];                                       \
        while (elm) {                                                   \
          next = elm->field.hte_next;                                   \
          b2 = HT_ELT_HASH_(elm, field, hashfn) % new_len;              \
          elm->field.hte_next = new_table[b2];                          \
          new_table[b2] = elm;                                          \
          elm = next;                                                   \
        }                                                               \
      }                                                                 \
      if (head->hth_table)                                              \
        freefn(head->hth_table);                                        \
      head->hth_table = new_table;                                      \
    } else {                                                            \
      unsigned b, b2;                                                   \
      new_table = reallocfn(head->hth_table, new_len*sizeof(struct type*)); \
      if (!new_table) return -1;                                        \
      memset(new_table + head->hth_table_length, 0,                     \
             (new_len - head->hth_table_length)*sizeof(struct type*));  \
      for (b=0; b < head->hth_table_length; ++b) {                      \
        struct type *e, **pE;                                           \
        for (pE = &new_table[b], e = *pE; e != NULL; e = *pE) {         \
          b2 = HT_ELT_HASH_(e, field, hashfn) % new_len;                \
          if (b2 == b) {                                                \
            pE = &e->field.hte_next;                                    \
          } else {                                                      \
            *pE = e->field.hte_next;                                    \
            e->field.hte_next = new_table[b2];                          \
            new_table[b2] = e;                                          \
          }                                                             \
        }                                                               \
      }                                                                 \
      head->hth_table = new_table;                                      \
    }                                                                   \
    head->hth_table_length = new_len;                                   \
    head->hth_prime_idx = prime_idx;                                    \
    head->hth_load_limit = new_load_limit;                              \
    return 0;                                                           \
  }                                                                     \
                                              \
  void                                                                  \
  name##_HT_CLEAR(struct name *head)                                    \
  {                                                                     \
    if (head->hth_table)                                                \
      freefn(head->hth_table);                                          \
    name##_HT_INIT(head);                                               \
  }                                                                     \
                                            \
  int                                                                   \
  name##_HT_REP_IS_BAD_(const struct name *head)			\
  {                                                                     \
    unsigned n, i;                                                      \
    struct type *elm;                                                   \
    if (!head->hth_table_length) {                                      \
      if (!head->hth_table && !head->hth_n_entries &&                   \
          !head->hth_load_limit && head->hth_prime_idx == -1)           \
        return 0;                                                       \
      else                                                              \
        return 1;                                                       \
    }                                                                   \
    if (!head->hth_table || head->hth_prime_idx < 0 ||                  \
        !head->hth_load_limit)                                          \
      return 2;                                                         \
    if (head->hth_n_entries > head->hth_load_limit)                     \
      return 3;                                                         \
    if (head->hth_table_length != name##_PRIMES[head->hth_prime_idx])   \
      return 4;                                                         \
    if (head->hth_load_limit != (unsigned)(load*head->hth_table_length)) \
      return 5;                                                         \
    for (n = i = 0; i < head->hth_table_length; ++i) {                  \
      for (elm = head->hth_table[i]; elm; elm = elm->field.hte_next) {  \
        if (HT_ELT_HASH_(elm, field, hashfn) != hashfn(elm))	        \
          return 1000 + i;                                              \
        if ((HT_ELT_HASH_(elm, field, hashfn) % head->hth_table_length) != i) \
          return 10000 + i;                                             \
        ++n;                                                            \
      }                                                                 \
    }                                                                   \
    if (n != head->hth_n_entries)                                       \
      return 6;                                                         \
    return 0;                                                           \
  }

#define HT_FIND_OR_INSERT_(name, field, hashfn, head, eltype, elm, var, y, n) \
  {                                                                     \
    struct name *var##_head_ = head;                                    \
    struct eltype **var;                                                \
    if (!var##_head_->hth_table ||                                      \
        var##_head_->hth_n_entries >= var##_head_->hth_load_limit)      \
      name##_HT_GROW(var##_head_, var##_head_->hth_n_entries+1);        \
    HT_SET_HASH_((elm), field, hashfn);                                 \
    var = name##_HT_FIND_P_(var##_head_, (elm));                        \
    if (*var) {                                                         \
      y;                                                                \
    } else {                                                            \
      n;                                                                \
    }                                                                   \
  }
#define HT_FOI_INSERT_(field, head, elm, newent, var)       \
  {                                                         \
    HT_SET_HASHVAL_(newent, field, (elm)->field.hte_hash);  \
    newent->field.hte_next = NULL;                          \
    *var = newent;                                          \
    ++((head)->hth_n_entries);                              \
  }

/*
 * Copyright 2005, Nick Mathewson.  Implementation logic is adapted from code
 * by Christopher Clark, retrofit to allow drop-in memory management, and to
 * use the same interface as Niels Provos's tree.h.  This is probably still
 * a derived work, so the original license below still applies.
 *
 * Copyright (c) 2002, Christopher Clark
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#endif

