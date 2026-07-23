/*!
Bounds-checking for SPIR-V output.
*/

use super::{
    helpers::{global_needs_wrapper, map_storage_class},
    selection::Selection,
    Block, BlockContext, Error, IdGenerator, Instruction, Word,
};
use crate::{
    arena::Handle,
    proc::{index::GuardedIndex, BoundsCheckPolicy},
};

/// The results of performing a bounds check.
///
/// On success, [`write_bounds_check`](BlockContext::write_bounds_check)
/// returns a value of this type. The caller can assume that the right
/// policy has been applied, and simply do what the variant says.
#[derive(Debug)]
pub(super) enum BoundsCheckResult {
    /// The index is statically known and in bounds, with the given value.
    KnownInBounds(u32),

    /// The given instruction computes the index to be used.
    ///
    /// When [`BoundsCheckPolicy::Restrict`] is in force, this is a
    /// clamped version of the index the user supplied.
    ///
    /// When [`BoundsCheckPolicy::Unchecked`] is in force, this is
    /// simply the index the user supplied. This variant indicates
    /// that we couldn't prove statically that the index was in
    /// bounds; otherwise we would have returned [`KnownInBounds`].
    ///
    /// [`KnownInBounds`]: BoundsCheckResult::KnownInBounds
    Computed(Word),

    /// The given instruction computes a boolean condition which is true
    /// if the index is in bounds.
    ///
    /// This is returned when [`BoundsCheckPolicy::ReadZeroSkipWrite`]
    /// is in force.
    Conditional {
        /// The access should only be permitted if this value is true.
        condition_id: Word,

        /// The access should use this index value.
        index_id: Word,
    },
}

/// A value that we either know at translation time, or need to compute at runtime.
#[derive(Copy, Clone)]
pub(super) enum MaybeKnown<T> {
    /// The value is known at shader translation time.
    Known(T),

    /// The value is computed by the instruction with the given id.
    Computed(Word),
}

impl BlockContext<'_> {
    /// Emit code to compute the length of a run-time array.
    ///
    /// Given `array`, an expression referring a runtime-sized array, return the
    /// instruction id for the array's length.
    ///
    /// Runtime-sized arrays may only appear in the values of global
    /// variables, which must have one of the following Naga types:
    ///
    /// 1. A runtime-sized array.
    /// 2. A struct whose last member is a runtime-sized array.
    /// 3. A binding array of 2.
    ///
    /// Thus, the expression `array` has the form of:
    ///
    /// - An optional [`AccessIndex`], for case 2, applied to...
    /// - An optional [`Access`] or [`AccessIndex`], for case 3, applied to...
    /// - A [`GlobalVariable`].
    ///
    /// The generated SPIR-V takes into account wrapped globals; see
    /// [`back::spv::GlobalVariable`] for details.
    ///
    /// [`GlobalVariable`]: crate::Expression::GlobalVariable
    /// [`AccessIndex`]: crate::Expression::AccessIndex
    /// [`Access`]: crate::Expression::Access
    /// [`base`]: crate::Expression::Access::base
    /// [`back::spv::GlobalVariable`]: super::GlobalVariable
    pub(super) fn write_runtime_array_length(
        &mut self,
        array: Handle<crate::Expression>,
        block: &mut Block,
    ) -> Result<Word, Error> {
        let binding_array_index_id: Option<Word>;

        let global_handle: Handle<crate::GlobalVariable>;

        let opt_last_member_index: Option<u32>;

        match self.ir_function.expressions[array] {
            crate::Expression::AccessIndex { base, index } => {
                match self.ir_function.expressions[base] {
                    crate::Expression::AccessIndex {
                        base: base_outer,
                        index: index_outer,
                    } => match self.ir_function.expressions[base_outer] {
                        crate::Expression::GlobalVariable(handle) => {
                            let index_id = self.get_index_constant(index_outer);
                            binding_array_index_id = Some(index_id);
                            global_handle = handle;
                            opt_last_member_index = Some(index);
                        }
                        _ => {
                            return Err(Error::Validation(
                                "array length expression: AccessIndex(AccessIndex(Global))",
                            ))
                        }
                    },
                    crate::Expression::Access {
                        base: base_outer,
                        index: index_outer,
                    } => match self.ir_function.expressions[base_outer] {
                        crate::Expression::GlobalVariable(handle) => {
                            let index_id = self.cached[index_outer];
                            binding_array_index_id = Some(index_id);
                            global_handle = handle;
                            opt_last_member_index = Some(index);
                        }
                        _ => {
                            return Err(Error::Validation(
                                "array length expression: AccessIndex(Access(Global))",
                            ))
                        }
                    },
                    crate::Expression::GlobalVariable(handle) => {
                        binding_array_index_id = None;
                        global_handle = handle;
                        opt_last_member_index = Some(index);
                    }
                    _ => {
                        return Err(Error::Validation(
                            "array length expression: AccessIndex(<unexpected>)",
                        ))
                    }
                }
            }
            crate::Expression::GlobalVariable(handle) => {
                binding_array_index_id = None;
                global_handle = handle;
                opt_last_member_index = None;
            }
            _ => return Err(Error::Validation("array length expression case-4")),
        };

        let global = &self.ir_module.global_variables[global_handle];
        match (
            &self.ir_module.types[global.ty].inner,
            binding_array_index_id,
        ) {
            (&crate::TypeInner::BindingArray { .. }, Some(_)) => {}
            (_, None) => {}
            _ => {
                return Err(Error::Validation(
                    "array length expression: bad binding array inference",
                ))
            }
        }

        let gvar = self.writer.global_variables[global_handle].clone();
        let global = &self.ir_module.global_variables[global_handle];
        let needs_wrapper = global_needs_wrapper(self.ir_module, global);
        let (last_member_index, gvar_id) = match (opt_last_member_index, needs_wrapper) {
            (Some(index), false) => {
                (index, gvar.access_id)
            }
            (None, true) => {
                (0, gvar.var_id)
            }
            _ => {
                return Err(Error::Validation(
                    "array length expression: bad SPIR-V wrapper struct inference",
                ));
            }
        };

        let structure_id = match binding_array_index_id {
            Some(index_id) => {
                let element_type_id = match self.ir_module.types[global.ty].inner {
                    crate::TypeInner::BindingArray { base, size: _ } => {
                        let base_id = self.get_handle_type_id(base);
                        let class = map_storage_class(global.space);
                        self.get_pointer_type_id(base_id, class)
                    }
                    _ => return Err(Error::Validation("array length expression case-5")),
                };
                let structure_id = self.gen_id();
                block.body.push(Instruction::access_chain(
                    element_type_id,
                    structure_id,
                    gvar_id,
                    &[index_id],
                ));
                structure_id
            }
            None => gvar_id,
        };
        let length_id = self.gen_id();
        block.body.push(Instruction::array_length(
            self.writer.get_u32_type_id(),
            length_id,
            structure_id,
            last_member_index,
        ));

        Ok(length_id)
    }

    /// If `sequence` refers to an unbounded binding array global, return its
    /// layout size from the SPIR-V binding map.
    fn binding_array_layout_size(&self, sequence: Handle<crate::Expression>) -> Option<u32> {
        let global_handle = match self.ir_function.expressions[sequence] {
            crate::Expression::GlobalVariable(handle) => handle,
            _ => self.ir_function.originating_global(sequence)?,
        };
        let global = &self.ir_module.global_variables[global_handle];
        let crate::TypeInner::BindingArray { .. } = self.ir_module.types[global.ty].inner else {
            return None;
        };
        let binding = global.binding?;
        let bind_target = self.writer.resolve_resource_binding(&binding).ok()?;
        bind_target.binding_array_size
    }

    /// Compute the length of a subscriptable value.
    ///
    /// Given `sequence`, an expression referring to some indexable type, return
    /// its length. The result may either be computed by SPIR-V instructions, or
    /// known at shader translation time.
    ///
    /// `sequence` may be a `Vector`, `Matrix`, or `Array`, a `Pointer` to any
    /// of those, or a `ValuePointer`. An array may be fixed-size, dynamically
    /// sized, or use a specializable constant as its length.
    fn write_sequence_length(
        &mut self,
        sequence: Handle<crate::Expression>,
        block: &mut Block,
    ) -> Result<MaybeKnown<u32>, Error> {
        let sequence_ty = self.fun_info[sequence].ty.inner_with(&self.ir_module.types);
        match sequence_ty.indexable_length_resolved(self.ir_module) {
            Ok(crate::proc::IndexableLength::Known(known_length)) => {
                Ok(MaybeKnown::Known(known_length))
            }
            Ok(crate::proc::IndexableLength::Dynamic) => {
                if let Some(size) = self.binding_array_layout_size(sequence) {
                    return Ok(MaybeKnown::Known(size));
                }
                let length_id = self.write_runtime_array_length(sequence, block)?;
                Ok(MaybeKnown::Computed(length_id))
            }
            Err(err) => {
                log::error!("Sequence length for {sequence:?} failed: {err}");
                Err(Error::Validation("indexable length"))
            }
        }
    }

    /// Compute the maximum valid index of a subscriptable value.
    ///
    /// Given `sequence`, an expression referring to some indexable type, return
    /// its maximum valid index - one less than its length. The result may
    /// either be computed, or known at shader translation time.
    ///
    /// `sequence` may be a `Vector`, `Matrix`, or `Array`, a `Pointer` to any
    /// of those, or a `ValuePointer`. An array may be fixed-size, dynamically
    /// sized, or use a specializable constant as its length.
    fn write_sequence_max_index(
        &mut self,
        sequence: Handle<crate::Expression>,
        block: &mut Block,
    ) -> Result<MaybeKnown<u32>, Error> {
        match self.write_sequence_length(sequence, block)? {
            MaybeKnown::Known(known_length) => {
                assert!(known_length > 0);
                Ok(MaybeKnown::Known(known_length - 1))
            }
            MaybeKnown::Computed(length_id) => {
                let const_one_id = self.get_index_constant(1);
                let max_index_id = self.gen_id();
                block.body.push(Instruction::binary(
                    spirv::Op::ISub,
                    self.writer.get_u32_type_id(),
                    max_index_id,
                    length_id,
                    const_one_id,
                ));
                Ok(MaybeKnown::Computed(max_index_id))
            }
        }
    }

    /// Restrict an index to be in range for a vector, matrix, or array.
    ///
    /// This is used to implement `BoundsCheckPolicy::Restrict`. An in-bounds
    /// index is left unchanged. An out-of-bounds index is replaced with some
    /// arbitrary in-bounds index. Note,this is not necessarily clamping; for
    /// example, negative indices might be changed to refer to the last element
    /// of the sequence, not the first, as clamping would do.
    ///
    /// Either return the restricted index value, if known, or add instructions
    /// to `block` to compute it, and return the id of the result. See the
    /// documentation for `BoundsCheckResult` for details.
    ///
    /// The `sequence` expression may be a `Vector`, `Matrix`, or `Array`, a
    /// `Pointer` to any of those, or a `ValuePointer`. An array may be
    /// fixed-size, dynamically sized, or use a specializable constant as its
    /// length.
    pub(super) fn write_restricted_index(
        &mut self,
        sequence: Handle<crate::Expression>,
        index: GuardedIndex,
        block: &mut Block,
    ) -> Result<BoundsCheckResult, Error> {
        let max_index = self.write_sequence_max_index(sequence, block)?;

        if let (GuardedIndex::Known(index), MaybeKnown::Known(max_index)) = (index, max_index) {
            let restricted = core::cmp::min(index, max_index);
            return Ok(BoundsCheckResult::KnownInBounds(restricted));
        }

        let index_id = match index {
            GuardedIndex::Known(value) => self.get_index_constant(value),
            GuardedIndex::Expression(expr) => self.cached[expr],
        };

        let max_index_id = match max_index {
            MaybeKnown::Known(value) => self.get_index_constant(value),
            MaybeKnown::Computed(id) => id,
        };

        let restricted_index_id = self.gen_id();
        block.body.push(Instruction::ext_inst_gl_op(
            self.writer.gl450_ext_inst_id,
            spirv::GlslStd450Op::UMin,
            self.writer.get_u32_type_id(),
            restricted_index_id,
            &[index_id, max_index_id],
        ));
        Ok(BoundsCheckResult::Computed(restricted_index_id))
    }

    /// Write an index bounds comparison to `block`, if needed.
    ///
    /// This is used to implement [`BoundsCheckPolicy::ReadZeroSkipWrite`].
    ///
    /// If we're able to determine statically that `index` is in bounds for
    /// `sequence`, return `KnownInBounds(value)`, where `value` is the actual
    /// value of the index. (In principle, one could know that the index is in
    /// bounds without knowing its specific value, but in our simple-minded
    /// situation, we always know it.)
    ///
    /// If instead we must generate code to perform the comparison at run time,
    /// return `Conditional(comparison_id)`, where `comparison_id` is an
    /// instruction producing a boolean value that is true if `index` is in
    /// bounds for `sequence`.
    ///
    /// The `sequence` expression may be a `Vector`, `Matrix`, or `Array`, a
    /// `Pointer` to any of those, or a `ValuePointer`. An array may be
    /// fixed-size, dynamically sized, or use a specializable constant as its
    /// length.
    fn write_index_comparison(
        &mut self,
        sequence: Handle<crate::Expression>,
        index: GuardedIndex,
        block: &mut Block,
    ) -> Result<BoundsCheckResult, Error> {
        let length = self.write_sequence_length(sequence, block)?;

        if let (GuardedIndex::Known(index), MaybeKnown::Known(length)) = (index, length) {
            if index < length {
                return Ok(BoundsCheckResult::KnownInBounds(index));
            }

        }

        let index_id = match index {
            GuardedIndex::Known(value) => self.get_index_constant(value),
            GuardedIndex::Expression(expr) => self.cached[expr],
        };

        let length_id = match length {
            MaybeKnown::Known(value) => self.get_index_constant(value),
            MaybeKnown::Computed(id) => id,
        };

        let condition_id = self.gen_id();
        block.body.push(Instruction::binary(
            spirv::Op::ULessThan,
            self.writer.get_bool_type_id(),
            condition_id,
            index_id,
            length_id,
        ));

        Ok(BoundsCheckResult::Conditional {
            condition_id,
            index_id,
        })
    }

    /// Emit a conditional load for `BoundsCheckPolicy::ReadZeroSkipWrite`.
    ///
    /// Generate code to load a value of `result_type` if `condition` is true,
    /// and generate a null value of that type if it is false. Call `emit_load`
    /// to emit the instructions to perform the load. Return the id of the
    /// merged value of the two branches.
    pub(super) fn write_conditional_indexed_load<F>(
        &mut self,
        result_type: Word,
        condition: Word,
        block: &mut Block,
        emit_load: F,
    ) -> Word
    where
        F: FnOnce(&mut IdGenerator, &mut Block) -> Word,
    {
        let null_id = self.writer.get_constant_null(result_type);

        let mut selection = Selection::start(block, result_type);


        selection.if_true(self, condition, null_id);

        let loaded_value = emit_load(&mut self.writer.id_gen, selection.block());

        selection.finish(self, loaded_value)
    }

    /// Emit code for bounds checks for an array, vector, or matrix access.
    ///
    /// This tries to handle all the critical steps for bounds checks:
    ///
    /// - First, select the appropriate bounds check policy for `base`,
    ///   depending on its address space.
    ///
    /// - Next, analyze `index` to see if its value is known at
    ///   compile time, in which case we can decide statically whether
    ///   the index is in bounds.
    ///
    /// - If the index's value is not known at compile time, emit code to:
    ///
    ///     - restrict its value (for [`BoundsCheckPolicy::Restrict`]), or
    ///
    ///     - check whether it's in bounds (for
    ///       [`BoundsCheckPolicy::ReadZeroSkipWrite`]).
    ///
    /// Return a [`BoundsCheckResult`] indicating how the index should be
    /// consumed. See that type's documentation for details.
    pub(super) fn write_bounds_check(
        &mut self,
        base: Handle<crate::Expression>,
        mut index: GuardedIndex,
        block: &mut Block,
    ) -> Result<BoundsCheckResult, Error> {
        index.try_resolve_to_constant(&self.ir_function.expressions, self.ir_module);

        let policy = self.writer.bounds_check_policies.choose_policy(
            base,
            &self.ir_module.types,
            self.fun_info,
        );

        Ok(match policy {
            BoundsCheckPolicy::Restrict => self.write_restricted_index(base, index, block)?,
            BoundsCheckPolicy::ReadZeroSkipWrite => {
                self.write_index_comparison(base, index, block)?
            }
            BoundsCheckPolicy::Unchecked => match index {
                GuardedIndex::Known(value) => BoundsCheckResult::KnownInBounds(value),
                GuardedIndex::Expression(expr) => BoundsCheckResult::Computed(self.cached[expr]),
            },
        })
    }

    /// Emit code to subscript a vector by value with a computed index.
    ///
    /// Return the id of the element value.
    ///
    /// If `base_id_override` is provided, it is used as the vector expression
    /// to be subscripted into, rather than the cached value of `base`.
    pub(super) fn write_vector_access(
        &mut self,
        result_type_id: Word,
        base: Handle<crate::Expression>,
        base_id_override: Option<Word>,
        index: GuardedIndex,
        block: &mut Block,
    ) -> Result<Word, Error> {
        let base_id = base_id_override.unwrap_or_else(|| self.cached[base]);

        let result_id = match self.write_bounds_check(base, index, block)? {
            BoundsCheckResult::KnownInBounds(known_index) => {
                let result_id = self.gen_id();
                block.body.push(Instruction::composite_extract(
                    result_type_id,
                    result_id,
                    base_id,
                    &[known_index],
                ));
                result_id
            }
            BoundsCheckResult::Computed(computed_index_id) => {
                let result_id = self.gen_id();
                block.body.push(Instruction::vector_extract_dynamic(
                    result_type_id,
                    result_id,
                    base_id,
                    computed_index_id,
                ));
                result_id
            }
            BoundsCheckResult::Conditional {
                condition_id,
                index_id,
            } => {
                self.write_conditional_indexed_load(
                    result_type_id,
                    condition_id,
                    block,
                    |id_gen, block| {
                        let element_id = id_gen.next();
                        block.body.push(Instruction::vector_extract_dynamic(
                            result_type_id,
                            element_id,
                            base_id,
                            index_id,
                        ));
                        element_id
                    },
                )
            }
        };

        Ok(result_id)
    }
}
