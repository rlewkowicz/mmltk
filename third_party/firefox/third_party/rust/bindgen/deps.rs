/// Generating build depfiles from parsed bindings.
use std::{collections::BTreeSet, path::PathBuf};

#[derive(Clone, Debug)]
pub(crate) struct DepfileSpec {
    pub output_module: String,
    pub depfile_path: PathBuf,
}

impl DepfileSpec {
    pub fn write(&self, deps: &BTreeSet<Box<str>>) -> std::io::Result<()> {
        std::fs::write(&self.depfile_path, self.to_string(deps))
    }

    fn to_string(&self, deps: &BTreeSet<Box<str>>) -> String {
        let escape = |s: &str| s.replace('\\', "\\\\").replace(' ', "\\ ");

        let mut buf = format!("{}:", escape(&self.output_module));
        for file in deps {
            buf = format!("{buf} {}", escape(file));
        }
        buf
    }
}
