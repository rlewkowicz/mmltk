/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use super::*;
/// Implements renaming of items in the CI via toml configuration.
/// Intended to be called by bindings to update the local names of items
/// to be generated but not touching names in the ffi we need to use.
use crate::VisitMut;
use std::collections::HashMap;

pub fn rename(ci: &mut ComponentInterface, renames: &HashMap<String, toml::Table>) {
    let this_module_path = ci.crate_name().to_string();
    ci.visit_mut(&TomlRenamer {
        this_module_path,
        renames,
    })
}

struct TomlRenamer<'a> {
    this_module_path: String,
    renames: &'a HashMap<String, toml::Table>,
}

impl TomlRenamer<'_> {
    fn new_name(&self, module_path: &str, name: &str) -> Option<String> {
        self.renames
            .get(module_path)
            .and_then(|rename_table| rename_table.get(name))
            .and_then(|v| v.as_str())
            .map(|s| s.to_string())
    }
}

impl VisitMut for TomlRenamer<'_> {
    fn visit_record(&self, record: &mut Record) {
        let module_path = &record.module_path;
        let record_name = record.name().to_string();
        for field in &mut record.fields {
            let field_path = format!("{}.{}", record_name, field.name);
            if let Some(new_name) = self.new_name(module_path, &field_path) {
                field.name = new_name;
            }
        }
        if let Some(new_name) = self.new_name(module_path, &record_name) {
            record.name = new_name;
        }
    }

    fn visit_object(&self, object: &mut Object) {
        let module_path = &object.module_path;
        if let Some(new_name) = self.new_name(module_path, object.name()) {
            object.name = new_name;
        }
    }

    fn visit_callback_interface(&self, iface: &mut CallbackInterface) {
        let module_path = &iface.module_path;
        if let Some(new_name) = self.new_name(module_path, &iface.name) {
            iface.name = new_name;
        }
    }

    fn visit_enum(&self, _is_error: bool, enum_: &mut Enum) {
        let module_path = &enum_.module_path;
        let enum_name = enum_.name().to_string();
        for variant in &mut enum_.variants {
            let variant_name = variant.name.clone();
            let variant_path = format!("{}.{}", enum_name, variant_name);
            for field in &mut variant.fields {
                let field_path = format!("{}.{}", variant_path, field.name);
                if let Some(new_name) = self.new_name(module_path, &field_path) {
                    field.name = new_name;
                }
            }
            if let Some(new_name) = self.new_name(module_path, &variant_path) {
                variant.name = new_name;
            }
        }
        if let Some(new_name) = self.new_name(module_path, &enum_name) {
            enum_.name = new_name;
        }
    }

    fn visit_type(&self, type_: &mut Type) {
        let module_path = type_.module_path().unwrap_or(&self.this_module_path);
        let self_renames = self.renames.get(module_path);
        type_.rename_recursive(&|name| {
            self_renames
                .and_then(|renames| renames.get(name))
                .and_then(|value| value.as_str())
                .unwrap_or(name)
                .to_string()
        });
    }

    fn visit_method(&self, object_name: &str, method: &mut Method) {
        let method_name = format!("{}.{}", object_name, method.name());
        if let Some(new_name) = self.new_name(&self.this_module_path, &method_name) {
            method.name = new_name;
        }
        for arg in &mut method.arguments {
            let arg_path = format!("{}.{}", method_name, arg.name);
            if let Some(new_name) = self.new_name(&self.this_module_path, &arg_path) {
                arg.name = new_name;
            }
        }
    }

    fn visit_constructor(&self, object_name: &str, constructor: &mut Constructor) {
        let method_name = format!("{}.{}", object_name, constructor.name());
        if let Some(new_name) = self.new_name(&self.this_module_path, &method_name) {
            constructor.name = new_name;
        }
        for arg in &mut constructor.arguments {
            let arg_path = format!("{}.{}", method_name, arg.name);
            if let Some(new_name) = self.new_name(&self.this_module_path, &arg_path) {
                arg.name = new_name;
            }
        }
    }

    fn visit_function(&self, function: &mut Function) {
        let original_function_name = function.name.clone();
        if let Some(new_name) = self.new_name(&self.this_module_path, &function.name) {
            function.name = new_name;
        }
        for arg in &mut function.arguments {
            let arg_path = format!("{}.{}", original_function_name, arg.name);
            if let Some(new_name) = self.new_name(&self.this_module_path, &arg_path) {
                arg.name = new_name;
            }
        }
    }

    fn visit_error_name(&self, name: &mut String) {
        if let Some(new_name) = self.new_name(&self.this_module_path, name) {
            *name = new_name;
        }
    }
}
