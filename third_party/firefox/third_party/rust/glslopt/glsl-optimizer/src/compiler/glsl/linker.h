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

#ifndef GLSL_LINKER_H
#define GLSL_LINKER_H

#include "linker_util.h"

struct gl_shader_program;
struct gl_shader;
struct gl_linked_shader;

extern bool
link_function_calls(gl_shader_program *prog, gl_linked_shader *main,
                    gl_shader **shader_list, unsigned num_shaders);

extern void
link_invalidate_variable_locations(exec_list *ir);

extern void
link_assign_uniform_locations(struct gl_shader_program *prog,
                              struct gl_context *ctx);

extern void
link_set_uniform_initializers(struct gl_shader_program *prog,
                              unsigned int boolean_true);

extern int
link_cross_validate_uniform_block(void *mem_ctx,
                                  struct gl_uniform_block **linked_blocks,
                                  unsigned int *num_linked_blocks,
                                  struct gl_uniform_block *new_block);

extern void
link_uniform_blocks(void *mem_ctx,
                    struct gl_context *ctx,
                    struct gl_shader_program *prog,
                    struct gl_linked_shader *shader,
                    struct gl_uniform_block **ubo_blocks,
                    unsigned *num_ubo_blocks,
                    struct gl_uniform_block **ssbo_blocks,
                    unsigned *num_ssbo_blocks);

bool
validate_intrastage_arrays(struct gl_shader_program *prog,
                           ir_variable *const var,
                           ir_variable *const existing,
                           bool match_precision = true);

void
validate_intrastage_interface_blocks(struct gl_shader_program *prog,
                                     const gl_shader **shader_list,
                                     unsigned num_shaders);

void
validate_interstage_inout_blocks(struct gl_shader_program *prog,
                                 const gl_linked_shader *producer,
                                 const gl_linked_shader *consumer);

void
validate_interstage_uniform_blocks(struct gl_shader_program *prog,
                                   gl_linked_shader **stages);

extern void
link_assign_atomic_counter_resources(struct gl_context *ctx,
                                     struct gl_shader_program *prog);

extern void
link_check_atomic_counter_resources(struct gl_context *ctx,
                                    struct gl_shader_program *prog);


extern struct gl_linked_shader *
link_intrastage_shaders(void *mem_ctx,
                        struct gl_context *ctx,
                        struct gl_shader_program *prog,
                        struct gl_shader **shader_list,
                        unsigned num_shaders,
                        bool allow_missing_main);

extern unsigned
link_calculate_matrix_stride(const glsl_type *matrix, bool row_major,
                             enum glsl_interface_packing packing);

class program_resource_visitor {
public:
   void process(ir_variable *var, bool use_std430_as_default);

   void process(ir_variable *var, const glsl_type *var_type,
                bool use_std430_as_default);

   void process(const glsl_type *type, const char *name,
                bool use_std430_as_default);

protected:
   virtual void visit_field(const glsl_type *type, const char *name,
                            bool row_major, const glsl_type *record_type,
                            const enum glsl_interface_packing packing,
                            bool last_field) = 0;

   virtual void enter_record(const glsl_type *type, const char *name,
                             bool row_major, const enum glsl_interface_packing packing);

   virtual void leave_record(const glsl_type *type, const char *name,
                             bool row_major, const enum glsl_interface_packing packing);

   virtual void set_buffer_offset(unsigned offset);

   virtual void set_record_array_count(unsigned record_array_count);

private:
   void recursion(const glsl_type *t, char **name, size_t name_length,
                  bool row_major, const glsl_type *record_type,
                  const enum glsl_interface_packing packing,
                  bool last_field, unsigned record_array_count,
                  const glsl_struct_field *named_ifc_member);
};

#endif /* GLSL_LINKER_H */
