// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

use std::fs;

use crate::common::{self, FileResult, LintIssue};

pub fn run(fix: bool, linter: &str) {
    let paths = common::read_paths_from_stdin();
    common::par_map_lint_results(&paths, |path| check_file(path, fix, linter));
}

pub fn check_file(path: &str, fix: bool, linter: &str) -> Vec<FileResult> {
    let Some(content) = common::read_file_bytes(path) else {
        return Vec::new();
    };

    let mut results = Vec::new();
    let mut needs_write = false;

    let has_crlf = content.windows(2).any(|w| w == b"\r\n");
    let content = if has_crlf {
        if fix {
            needs_write = true;
            results.push(FileResult::Fixed);
        } else {
            results.push(FileResult::Issue(LintIssue::error(
                path,
                None,
                "Windows line return".to_string(),
                linter,
            )));
        }
        content.iter().copied().filter(|&b| b != b'\r').collect()
    } else {
        content
    };

    let mut lines: Vec<&[u8]> = content.split(|&b| b == b'\n').collect();
    if content.ends_with(b"\n") {
        lines.pop();
    }
    if lines.is_empty() {
        return results;
    }

    let mut fixed_lines: Vec<&[u8]> = Vec::new();

    for (i, line) in lines.iter().enumerate() {
        let trimmed_len = line
            .iter()
            .rposition(|&b| b != b' ' && b != b'\t')
            .map(|p| p + 1)
            .unwrap_or(0);
        if trimmed_len < line.len() {
            if fix {
                fixed_lines.push(&line[..trimmed_len]);
                needs_write = true;
                results.push(FileResult::Fixed);
            } else {
                results.push(FileResult::Issue(LintIssue::error(
                    path,
                    Some(i + 1),
                    "Trailing whitespace".to_string(),
                    linter,
                )));
            }
        } else if fix {
            fixed_lines.push(line);
        }
    }

    let trailing_ws = content
        .iter()
        .rev()
        .take_while(|&&b| matches!(b, b'\n' | b' ' | b'\t'))
        .count();
    let has_empty_end = trailing_ws > 1;
    let missing_final_newline = !content.is_empty() && !content.ends_with(b"\n");

    if has_empty_end {
        if fix {
            while fixed_lines
                .last()
                .is_some_and(|l| l.is_empty() || l.iter().all(|&b| matches!(b, b' ' | b'\t')))
            {
                fixed_lines.pop();
            }
            needs_write = true;
            results.push(FileResult::Fixed);
        } else {
            results.push(FileResult::Issue(LintIssue::error(
                path,
                Some(lines.len()),
                "Empty Lines at end of file".to_string(),
                linter,
            )));
        }
    } else if missing_final_newline {
        if fix {
            needs_write = true;
            results.push(FileResult::Fixed);
        } else {
            results.push(FileResult::Issue(LintIssue::error(
                path,
                Some(lines.len()),
                "File does not end with newline character".to_string(),
                linter,
            )));
        }
    }

    if needs_write {
        let mut output = fixed_lines.join(&b'\n');
        output.push(b'\n');
        if let Err(e) = fs::write(path, &output) {
            eprintln!("Warning: could not write {path}: {e}");
        }
    }

    results
}
