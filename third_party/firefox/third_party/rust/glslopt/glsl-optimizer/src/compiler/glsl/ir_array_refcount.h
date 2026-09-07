/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */


#ifndef GLSL_IR_ARRAY_REFCOUNT_H
#define GLSL_IR_ARRAY_REFCOUNT_H

#include "ir.h"
#include "ir_visitor.h"
#include "linker_util.h"
#include "compiler/glsl_types.h"
#include "util/bitset.h"

class ir_array_refcount_entry
{
public:
   ir_array_refcount_entry(ir_variable *var);
   ~ir_array_refcount_entry();

   ir_variable *var; 

   bool is_referenced;

   unsigned array_depth;

   BITSET_WORD *bits;

   bool is_linearized_index_referenced(unsigned linearized_index) const
   {
      assert(bits != 0);
      assert(linearized_index <= num_bits);

      return BITSET_TEST(bits, linearized_index);
   }

private:

   unsigned num_bits;

   friend class array_refcount_test;
};

class ir_array_refcount_visitor : public ir_hierarchical_visitor {
public:
   ir_array_refcount_visitor(void);
   ~ir_array_refcount_visitor(void);

   virtual ir_visitor_status visit(ir_dereference_variable *);

   virtual ir_visitor_status visit_enter(ir_function_signature *);
   virtual ir_visitor_status visit_enter(ir_dereference_array *);

   ir_array_refcount_entry *get_variable_entry(ir_variable *var);

   struct hash_table *ht;

   void *mem_ctx;

private:
   array_deref_range *get_array_deref();

   ir_dereference_array *last_array_deref;

   array_deref_range *derefs;

   unsigned num_derefs;

   unsigned derefs_size;
};

#endif /* GLSL_IR_ARRAY_REFCOUNT_H */
