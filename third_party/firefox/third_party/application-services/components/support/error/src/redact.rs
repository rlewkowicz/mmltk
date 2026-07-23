/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Functions to redact strings to remove PII before logging them

/// Redact a URL.
///
/// It's tricky to redact an URL without revealing PII.  We check for various known bad URL forms
/// and report them, otherwise we just log "<URL>".
pub fn redact_url(url: &str) -> String {
    if url.is_empty() {
        return "<URL (empty)>".to_string();
    }
    match url.find(':') {
        None => "<URL (no scheme)>".to_string(),
        Some(n) => {
            let mut chars = url[0..n].chars();
            match chars.next() {
                None => return "<URL (empty scheme)>".to_string(),
                Some(c) => {
                    if !c.is_ascii_alphabetic() {
                        return "<URL (invalid scheme)>".to_string();
                    }
                }
            }
            for c in chars {
                if !(c.is_ascii_alphanumeric() || c == '+' || c == '-' || c == '.') {
                    return "<URL (invalid scheme)>".to_string();
                }
            }
            "<URL>".to_string()
        }
    }
}

/// Redact compact jwe string (Five base64 segments, separated by `.` chars)
pub fn redact_compact_jwe(url: &str) -> String {
    url.replace(|ch| ch != '.', "x")
}
