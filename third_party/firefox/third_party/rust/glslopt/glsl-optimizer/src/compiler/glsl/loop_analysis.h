/*
 * Copyright © 2010 Intel Corporation
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

#ifndef LOOP_ANALYSIS_H
#define LOOP_ANALYSIS_H

#include "ir.h"
#include "util/hash_table.h"

extern class loop_state *
analyze_loop_variables(exec_list *instructions);

static inline bool
is_break(ir_instruction *ir)
{
   return ir != NULL && ir->ir_type == ir_type_loop_jump &&
      ((ir_loop_jump *) ir)->is_break();
}


extern bool
unroll_loops(exec_list *instructions, loop_state *ls,
             const struct gl_shader_compiler_options *options);


class loop_variable_state : public exec_node {
public:
   class loop_variable *get(const ir_variable *);
   class loop_variable *insert(ir_variable *);
   class loop_variable *get_or_insert(ir_variable *, bool in_assignee);
   class loop_terminator *insert(ir_if *, bool continue_from_then);


   exec_list variables;

   exec_list constants;

   exec_list induction_variables;

   exec_list terminators;

   loop_terminator *limiting_terminator;

   hash_table *var_hash;

   unsigned num_loop_jumps;

   bool contains_calls;

   loop_variable_state()
   {
      this->num_loop_jumps = 0;
      this->contains_calls = false;
      this->var_hash = _mesa_pointer_hash_table_create(NULL);
      this->limiting_terminator = NULL;
   }

   ~loop_variable_state()
   {
      _mesa_hash_table_destroy(this->var_hash, NULL);
   }

   DECLARE_RALLOC_CXX_OPERATORS(loop_variable_state)
};


class loop_variable : public exec_node {
public:
   ir_variable *var;

   bool read_before_write;

   bool rhs_clean;

   bool conditional_or_nested_assignment;

   ir_assignment *first_assignment;

   unsigned num_assignments;

   ir_rvalue *increment;


   inline bool is_induction_var() const
   {
      return this->increment != NULL;
   }


   inline bool is_loop_constant() const
   {
      const bool is_const = (this->num_assignments == 0)
         || (((this->num_assignments == 1)
	     && !this->conditional_or_nested_assignment
	     && !this->read_before_write
             && this->rhs_clean) || this->var->data.read_only);

      assert((this->rhs_clean && (this->num_assignments == 1))
	     || !this->rhs_clean);

      return is_const;
   }

   void record_reference(bool in_assignee,
                         bool in_conditional_code_or_nested_loop,
                         ir_assignment *current_assignment);
};


class loop_terminator : public exec_node {
public:
   loop_terminator()
      : ir(NULL), iterations(-1)
   {
   }

   ir_if *ir;

   int iterations;

   bool continue_from_then;
};


class loop_state {
public:
   ~loop_state();

   loop_variable_state *get(const ir_loop *);

   loop_variable_state *insert(ir_loop *ir);

   bool loop_found;

private:
   loop_state();

   hash_table *ht;

   void *mem_ctx;

   friend loop_state *analyze_loop_variables(exec_list *instructions);
};

#endif /* LOOP_ANALYSIS_H */
