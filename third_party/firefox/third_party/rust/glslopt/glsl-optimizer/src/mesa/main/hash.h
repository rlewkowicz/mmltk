
/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2006  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#ifndef HASH_H
#define HASH_H


#include <stdbool.h>
#include <stdint.h>
#include "glheader.h"

#include "c11/threads.h"

#define DELETED_KEY_VALUE 1

static inline bool
uint_key_compare(const void *a, const void *b)
{
   return a == b;
}

static inline uint32_t
uint_hash(GLuint id)
{
   return id;
}

static inline uint32_t
uint_key_hash(const void *key)
{
   return uint_hash((uintptr_t)key);
}

static inline void *
uint_key(GLuint id)
{
   return (void *)(uintptr_t) id;
}

struct _mesa_HashTable {
   struct hash_table *ht;
   GLuint MaxKey;                        
   mtx_t Mutex;                          
   GLboolean InDeleteAll;                
   void *deleted_key_data;
};

extern struct _mesa_HashTable *_mesa_NewHashTable(void);

extern void _mesa_DeleteHashTable(struct _mesa_HashTable *table);

extern void *_mesa_HashLookup(struct _mesa_HashTable *table, GLuint key);

extern void _mesa_HashInsert(struct _mesa_HashTable *table, GLuint key, void *data);

extern void _mesa_HashRemove(struct _mesa_HashTable *table, GLuint key);

static inline void
_mesa_HashLockMutex(struct _mesa_HashTable *table)
{
   assert(table);
   mtx_lock(&table->Mutex);
}


static inline void
_mesa_HashUnlockMutex(struct _mesa_HashTable *table)
{
   assert(table);
   mtx_unlock(&table->Mutex);
}

extern void *_mesa_HashLookupLocked(struct _mesa_HashTable *table, GLuint key);

extern void _mesa_HashInsertLocked(struct _mesa_HashTable *table,
                                   GLuint key, void *data);

extern void _mesa_HashRemoveLocked(struct _mesa_HashTable *table, GLuint key);

extern void
_mesa_HashDeleteAll(struct _mesa_HashTable *table,
                    void (*callback)(GLuint key, void *data, void *userData),
                    void *userData);

extern void
_mesa_HashWalk(const struct _mesa_HashTable *table,
               void (*callback)(GLuint key, void *data, void *userData),
               void *userData);

extern void
_mesa_HashWalkLocked(const struct _mesa_HashTable *table,
                     void (*callback)(GLuint key, void *data, void *userData),
                     void *userData);

extern void _mesa_HashPrint(const struct _mesa_HashTable *table);

extern GLuint _mesa_HashFindFreeKeyBlock(struct _mesa_HashTable *table, GLuint numKeys);

extern GLuint
_mesa_HashNumEntries(const struct _mesa_HashTable *table);

extern void _mesa_test_hash_functions(void);


#endif
