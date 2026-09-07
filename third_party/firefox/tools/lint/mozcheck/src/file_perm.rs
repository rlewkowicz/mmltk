// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

use std::fs;
use std::io::Read;
use std::os::unix::fs::PermissionsExt;

use crate::common::{self, FileResult, LintIssue};

pub fn run(allow_shebang: bool, fix: bool, linter: &str) {
    let paths = common::read_paths_from_stdin();
    common::par_map_lint_results(&paths, |path| {
        check_file(path, allow_shebang, fix, linter)
            .into_iter()
            .collect()
    });
}

pub fn check_file(path: &str, allow_shebang: bool, fix: bool, linter: &str) -> Option<FileResult> {
    let metadata = match fs::metadata(path) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("Warning: could not stat {path}: {e}");
            return None;
        }
    };

    let mode = metadata.permissions().mode();
    if mode & 0o111 == 0 {
        return None;
    }

    if allow_shebang && has_shebang(path) {
        return None;
    }

    if fix {
        if let Err(e) = fs::set_permissions(path, fs::Permissions::from_mode(0o644)) {
            eprintln!("Warning: could not chmod {path}: {e}");
            return None;
        }
        return Some(FileResult::Fixed);
    }

    Some(FileResult::Issue(LintIssue::error(
        path,
        None,
        "Execution permissions on a source file".to_string(),
        linter,
    )))
}

fn has_shebang(path: &str) -> bool {
    let Ok(mut file) = fs::File::open(path) else {
        return false;
    };
    let mut buf = [0u8; 2];
    match file.read_exact(&mut buf) {
        Ok(()) => buf == *b"#!",
        Err(_) => false,
    }
}
