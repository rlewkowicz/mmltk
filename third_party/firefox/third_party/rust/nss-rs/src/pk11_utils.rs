// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use crate::err::{Error, Res};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pkcs11Uri {
    pub token: Option<String>,
}

fn percent_decode(s: &str) -> String {
    let mut result = Vec::with_capacity(s.len());
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%'
            && i + 2 < bytes.len()
            && let Ok(byte) =
                u8::from_str_radix(std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap_or(""), 16)
        {
            result.push(byte);
            i += 3;
            continue;
        }
        result.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&result).into_owned()
}

fn percent_encode(s: &str) -> String {
    let mut result = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => {
                result.push(b as char);
            }
            _ => {
                use std::fmt::Write as _;
                write!(result, "%{b:02X}").expect("write to String");
            }
        }
    }
    result
}

/// Parse a PKCS#11 URI (RFC 7512).
/// Expects the input to start with "pkcs11:".
pub fn parse(uri: &str) -> Res<Pkcs11Uri> {
    let path = uri.strip_prefix("pkcs11:").ok_or(Error::InvalidInput)?;

    let mut token = None;
    for attr in path.split(';') {
        if let Some((key, value)) = attr.split_once('=')
            && key == "token"
        {
            token = Some(percent_decode(value));
        }
    }

    Ok(Pkcs11Uri { token })
}

/// Build a PKCS#11 URI from a token name.
#[must_use]
pub fn build(token_name: &str) -> String {
    format!("pkcs11:token={}", percent_encode(token_name))
}
