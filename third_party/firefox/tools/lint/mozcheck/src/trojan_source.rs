// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

use crate::common::{self, LintIssue};
use unicode_categories::UnicodeCategories;

pub fn run(linter: &str) {
    let paths = common::read_paths_from_stdin();
    common::par_map_lint(&paths, |path| check_file(path, linter));
}

pub fn check_file(path: &str, linter: &str) -> Vec<LintIssue> {
    let Some(content) = common::read_file_bytes(path) else {
        return Vec::new();
    };

    let Ok(text) = String::from_utf8(content) else {
        return vec![LintIssue::error(
            path,
            None,
            "Could not open file as utf-8 - maybe an encoding error".to_string(),
            linter,
        )];
    };

    let mut issues = Vec::new();
    for (lineno, line) in text.lines().enumerate() {
        let disallowed: Vec<char> = line.chars().filter(|c| c.is_other_format()).collect();
        if !disallowed.is_empty() {
            issues.push(LintIssue::error(
                path,
                Some(lineno + 1),
                format!("disallowed characters: {disallowed:?}"),
                linter,
            ));
        }
    }
    issues
}
