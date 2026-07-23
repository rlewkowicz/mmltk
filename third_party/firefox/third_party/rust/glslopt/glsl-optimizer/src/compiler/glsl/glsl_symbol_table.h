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

#ifndef GLSL_SYMBOL_TABLE
#define GLSL_SYMBOL_TABLE

#include <new>

#include "program/symbol_table.h"
#include "ir.h"

class symbol_table_entry;
struct glsl_type;

struct glsl_symbol_table {
   DECLARE_RALLOC_CXX_OPERATORS(glsl_symbol_table)

   glsl_symbol_table();
   ~glsl_symbol_table();

   bool separate_function_namespace;

   void push_scope();
   void pop_scope();

   bool name_declared_this_scope(const char *name);

   bool add_variable(ir_variable *v);
   bool add_type(const char *name, const glsl_type *t);
   bool add_function(ir_function *f);
   bool add_interface(const char *name, const glsl_type *i,
                      enum ir_variable_mode mode);
   bool add_default_precision_qualifier(const char *type_name, int precision);

   void add_global_function(ir_function *f);

   ir_variable *get_variable(const char *name);
   const glsl_type *get_type(const char *name);
   ir_function *get_function(const char *name);
   const glsl_type *get_interface(const char *name,
                                  enum ir_variable_mode mode);
   int get_default_precision_qualifier(const char *type_name);

   void disable_variable(const char *name);

   void replace_variable(const char *name, ir_variable *v);

private:
   symbol_table_entry *get_entry(const char *name);

   struct _mesa_symbol_table *table;
   void *mem_ctx;
   void *linalloc;
};

#endif /* GLSL_SYMBOL_TABLE */
