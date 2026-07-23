use std::collections::HashMap;
use std::ffi::OsString;
use std::fs;
use std::io::{self, Read};
use std::os::unix::ffi::OsStringExt;
use std::path::{Path, PathBuf};
use std::str;

/// Returns all XDG user directories obtained from $(XDG_CONFIG_HOME)/user-dirs.dirs.
pub fn all(home_dir_path: &Path, user_dir_file_path: &Path) -> HashMap<String, PathBuf> {
    let bytes = read_all(user_dir_file_path).unwrap_or(Vec::new());
    parse_user_dirs(home_dir_path, None, &bytes)
}

/// Returns a single XDG user directory obtained from $(XDG_CONFIG_HOME)/user-dirs.dirs.
pub fn single(home_dir_path: &Path, user_dir_file_path: &Path, user_dir_name: &str) -> HashMap<String, PathBuf> {
    let bytes = read_all(user_dir_file_path).unwrap_or(Vec::new());
    parse_user_dirs(home_dir_path, Some(user_dir_name), &bytes)
}

fn parse_user_dirs(home_dir: &Path, user_dir: Option<&str>, bytes: &[u8]) -> HashMap<String, PathBuf> {
    let mut user_dirs = HashMap::new();

    for line in bytes.split(|b| *b == b'\n') {
        let mut single_dir_found = false;
        let (key, value) = match split_once(line, b'=') {
            Some(kv) => kv,
            None => continue,
        };

        let key = trim_blank(key);
        let key = if key.starts_with(b"XDG_") && key.ends_with(b"_DIR") {
            match str::from_utf8(&key[4..key.len()-4]) {
                Ok(key) =>
                    if user_dir.is_some() && option_contains(user_dir, key) {
                        single_dir_found = true;
                        key
                    } else if user_dir.is_none() {
                        key
                    } else {
                        continue
                    },
                Err(_)  => continue,
            }
        } else {
            continue
        };

        let value = trim_blank(value);
        let mut value = if value.starts_with(b"\"") && value.ends_with(b"\"") {
            &value[1..value.len()-1]
        } else {
            continue
        };

        let is_relative = if value == b"$HOME/" {
            continue
        } else if value.starts_with(b"$HOME/") {
            value = &value[b"$HOME/".len()..];
            true
        } else if value.starts_with(b"/") {
            false
        } else {
            continue
        };

        let value = OsString::from_vec(shell_unescape(value));

        let path = if is_relative {
            let mut path = PathBuf::from(&home_dir);
            path.push(value);
            path
        } else {
            PathBuf::from(value)
        };

        user_dirs.insert(key.to_owned(), path);
        if single_dir_found {
            break;
        }
    }

    user_dirs
}

/// Reads the entire contents of a file into a byte vector.
fn read_all(path: &Path) -> io::Result<Vec<u8>> {
    let mut file = fs::File::open(path)?;
    let mut bytes = Vec::with_capacity(1024);
    file.read_to_end(&mut bytes)?;
    Ok(bytes)
}

/// Returns bytes before and after first occurrence of separator.
fn split_once(bytes: &[u8], separator: u8) -> Option<(&[u8], &[u8])> {
    bytes.iter().position(|b| *b == separator).map(|i| {
        (&bytes[..i], &bytes[i+1..])
    })
}

/// Returns a slice with leading and trailing <blank> characters removed.
fn trim_blank(bytes: &[u8]) -> &[u8] {
    let i = bytes.iter().cloned().take_while(|b| *b == b' ' || *b == b'\t').count();
    let bytes = &bytes[i..];

    let i = bytes.iter().cloned().rev().take_while(|b| *b == b' ' || *b == b'\t').count();
    &bytes[..bytes.len()-i]
}

/// Unescape bytes escaped with POSIX shell double-quotes rules (as used by xdg-user-dirs-update).
fn shell_unescape(escaped: &[u8]) -> Vec<u8> {

    let mut unescaped: Vec<u8> = Vec::with_capacity(escaped.len());
    let mut i = escaped.iter().cloned();

    while let Some(b) = i.next() {
        if b == b'\\' {
            if let Some(b) = i.next() {
                unescaped.push(b);
            }
        } else {
            unescaped.push(b);
        }
    }

    unescaped
}

fn option_contains<T : PartialEq>(option: Option<T>, value: T) -> bool {
    match option {
        Some(val) => val == value,
        None => false
    }
}
