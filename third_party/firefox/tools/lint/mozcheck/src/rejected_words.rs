// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

use regex::RegexBuilder;

use crate::common::{self, LintIssue};

pub fn run(
    pattern: &str,
    ignore_case: bool,
    linter: &str,
    message: &str,
    rule: &str,
) -> Result<(), String> {
    let re = RegexBuilder::new(pattern)
        .case_insensitive(ignore_case)
        .build()
        .map_err(|e| format!("Invalid regex pattern '{pattern}': {e}"))?;

    let paths = common::read_paths_from_stdin();
    common::par_map_lint(&paths, |path| {
        check_reject_words(path, &re, linter, message, rule)
    });
    Ok(())
}

pub fn check_reject_words(
    path: &str,
    re: &regex::Regex,
    linter: &str,
    message: &str,
    rule: &str,
) -> Vec<LintIssue> {
    let content = match std::fs::read_to_string(path) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("Warning: could not read {path}: {e}");
            return Vec::new();
        }
    };

    let mut issues = Vec::new();
    for (lineno, line) in content.lines().enumerate() {
        for m in re.find_iter(line) {
            issues.push(LintIssue {
                path: path.to_string(),
                lineno: Some(lineno + 1),
                column: Some(m.start() + 1),
                message: message.to_string(),
                level: "error".to_string(),
                linter: linter.to_string(),
                rule: Some(rule.to_string()),
            });
        }
    }
    issues
}
