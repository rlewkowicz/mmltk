// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

//! Path utilities for expanding file lists with exclusion and extension filtering.
//!
//! This module resolves a mixed list of files and directories into a flat list
//! of files, applying glob-based exclusions and extension filters. It is used
//! by the batch runner to turn mozlint's input paths into the concrete set of
//! files each checker should process.

use std::path::Path;

use globset::{Glob, GlobSet, GlobSetBuilder};
use ignore::overrides::OverrideBuilder;
use ignore::types::TypesBuilder;
use ignore::WalkBuilder;

/// Expands a list of files and directories into individual file paths,
/// filtering by `extensions` and removing entries matching `exclude` patterns.
///
/// Paths are resolved relative to `root`. When `find_dotfiles` is true,
/// hidden files and directories (starting with `.`) are included.
pub fn expand_exclusions(
    paths: &[String],
    extensions: &[String],
    exclude: &[String],
    root: &str,
    find_dotfiles: bool,
) -> Vec<String> {
    let root_path = Path::new(root);
    let excludes: Vec<String> = exclude.iter().map(|e| normalize(e, root_path)).collect();
    let glob_excludes = build_glob_excludes(&excludes);

    let mut result = Vec::new();
    for path in paths {
        let p = Path::new(path);
        if p.is_file() {
            if is_excluded(path, &excludes, &glob_excludes) {
                continue;
            }
            result.push(path.clone());
        } else if p.is_dir() {
            walk_directory(path, extensions, &excludes, find_dotfiles, &mut result);
        }
    }
    result
}

/// Converts a relative path to absolute by joining it with `root`.
fn normalize(path: &str, root: &Path) -> String {
    if Path::new(path).is_absolute() {
        path.to_string()
    } else {
        root.join(path).to_string_lossy().to_string()
    }
}

/// Builds a [`GlobSet`] from exclude patterns that contain wildcards.
fn build_glob_excludes(excludes: &[String]) -> GlobSet {
    let mut builder = GlobSetBuilder::new();
    for pattern in excludes.iter().filter(|e| e.contains('*')) {
        if let Ok(glob) = Glob::new(pattern) {
            builder.add(glob);
        }
    }
    builder
        .build()
        .unwrap_or_else(|_| GlobSetBuilder::new().build().unwrap())
}

/// Returns `true` if `path` matches any literal prefix or glob in the exclude list.
fn is_excluded(path: &str, excludes: &[String], glob_excludes: &GlobSet) -> bool {
    let p = Path::new(path);
    excludes
        .iter()
        .any(|e| !e.contains('*') && p.starts_with(e))
        || glob_excludes.is_match(p)
}

/// Recursively walks `dir`, collecting files that match `extensions` while
/// skipping paths in `excludes`. Uses the `ignore` crate for efficient
/// directory traversal with override-based filtering.
fn walk_directory(
    dir: &str,
    extensions: &[String],
    excludes: &[String],
    find_dotfiles: bool,
    result: &mut Vec<String>,
) {
    if extensions.is_empty() {
        return;
    }

    let mut builder = WalkBuilder::new(dir);
    builder
        .hidden(!find_dotfiles)
        .ignore(false)
        .git_ignore(false)
        .git_global(false)
        .git_exclude(false);

    let mut types = TypesBuilder::new();
    for ext in extensions {
        let _ = types.add("lint", &format!("*.{ext}"));
    }
    types.select("lint");
    if let Ok(t) = types.build() {
        builder.types(t);
    }

    let mut ob = OverrideBuilder::new("/");
    if !find_dotfiles {
        let _ = ob.add("!.*");
        let _ = ob.add("!.*/**");
    }
    for pattern in excludes {
        let _ = ob.add(&format!("!{pattern}"));
        if !pattern.contains('*') {
            let _ = ob.add(&format!("!{pattern}/**"));
        }
    }
    if let Ok(overrides) = ob.build() {
        builder.overrides(overrides);
    }

    for entry in builder.build().flatten() {
        let Some(ft) = entry.file_type() else {
            continue;
        };
        if ft.is_file() {
            result.push(entry.path().to_string_lossy().to_string());
        }
    }
}
