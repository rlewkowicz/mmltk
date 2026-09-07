use alloc::{vec, vec::Vec};
use core::iter;

use spirv::{Op, Word, MAGIC_NUMBER};

use super::{Instruction, LogicalLayout, PhysicalLayout};


const GENERATOR: Word = 28;

impl PhysicalLayout {
    pub(super) const fn new(major_version: u8, minor_version: u8) -> Self {
        let version = ((major_version as u32) << 16) | ((minor_version as u32) << 8);
        PhysicalLayout {
            magic_number: MAGIC_NUMBER,
            version,
            generator: GENERATOR,
            bound: 0,
            instruction_schema: 0x0u32,
        }
    }

    pub(super) fn in_words(&self, sink: &mut impl Extend<Word>) {
        sink.extend(iter::once(self.magic_number));
        sink.extend(iter::once(self.version));
        sink.extend(iter::once(self.generator));
        sink.extend(iter::once(self.bound));
        sink.extend(iter::once(self.instruction_schema));
    }

    /// Returns `(major, minor)`.
    pub(super) const fn lang_version(&self) -> (u8, u8) {
        let major = (self.version >> 16) as u8;
        let minor = (self.version >> 8) as u8;
        (major, minor)
    }
}

impl super::reclaimable::Reclaimable for PhysicalLayout {
    fn reclaim(self) -> Self {
        PhysicalLayout {
            magic_number: self.magic_number,
            version: self.version,
            generator: self.generator,
            instruction_schema: self.instruction_schema,
            bound: 0,
        }
    }
}

impl LogicalLayout {
    pub(super) fn in_words(&self, sink: &mut impl Extend<Word>) {
        sink.extend(self.capabilities.iter().cloned());
        sink.extend(self.extensions.iter().cloned());
        sink.extend(self.ext_inst_imports.iter().cloned());
        sink.extend(self.memory_model.iter().cloned());
        sink.extend(self.entry_points.iter().cloned());
        sink.extend(self.execution_modes.iter().cloned());
        sink.extend(self.debugs.iter().cloned());
        sink.extend(self.annotations.iter().cloned());
        sink.extend(self.declarations.iter().cloned());
        sink.extend(self.function_declarations.iter().cloned());
        sink.extend(self.function_definitions.iter().cloned());
    }
}

impl super::reclaimable::Reclaimable for LogicalLayout {
    fn reclaim(self) -> Self {
        Self {
            capabilities: self.capabilities.reclaim(),
            extensions: self.extensions.reclaim(),
            ext_inst_imports: self.ext_inst_imports.reclaim(),
            memory_model: self.memory_model.reclaim(),
            entry_points: self.entry_points.reclaim(),
            execution_modes: self.execution_modes.reclaim(),
            debugs: self.debugs.reclaim(),
            annotations: self.annotations.reclaim(),
            declarations: self.declarations.reclaim(),
            function_declarations: self.function_declarations.reclaim(),
            function_definitions: self.function_definitions.reclaim(),
        }
    }
}

impl Instruction {
    pub(super) const fn new(op: Op) -> Self {
        Instruction {
            op,
            wc: 1, 
            type_id: None,
            result_id: None,
            operands: vec![],
        }
    }

    pub(super) fn set_type(&mut self, id: Word) {
        assert!(self.type_id.is_none(), "Type can only be set once");
        self.type_id = Some(id);
        self.wc += 1;
    }

    pub(super) fn set_result(&mut self, id: Word) {
        assert!(self.result_id.is_none(), "Result can only be set once");
        self.result_id = Some(id);
        self.wc += 1;
    }

    pub(super) fn add_operand(&mut self, operand: Word) {
        self.operands.push(operand);
        self.wc += 1;
    }

    pub(super) fn add_operands(&mut self, operands: Vec<Word>) {
        for operand in operands.into_iter() {
            self.add_operand(operand)
        }
    }

    pub(super) fn to_words(&self, sink: &mut impl Extend<Word>) {
        sink.extend(Some((self.wc << 16) | self.op as u32));
        sink.extend(self.type_id);
        sink.extend(self.result_id);
        sink.extend(self.operands.iter().cloned());
    }
}

impl Instruction {
}
