// Copyright Mozilla Foundation
// Licensed under the Apache License (Version 2.0), or the MIT license,
// (the "Licenses") at your option. You may not use this file except in
// compliance with one of the Licenses. You may obtain copies of the
// Licenses at:
//    http://www.apache.org/licenses/LICENSE-2.0
//    http://opensource.org/licenses/MIT
// Unless required by applicable law or agreed to in writing, software
// distributed under the Licenses is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the Licenses for the specific language governing permissions and
// limitations under the Licenses.

//! `xmldecl::parse()` extracts an encoding from an ASCII-based bogo-XML
//! declaration in `text/html` in a WebKit-compatible way.

extern crate encoding_rs;

fn position(needle: u8, haystack: &[u8]) -> Option<usize> {
    haystack.iter().position(|&x| x == needle)
}

fn skip_encoding(hay: &[u8]) -> Option<&[u8]> {
    let mut haystack = hay;
    loop {
        if let Some(g) = position(b'g', haystack) {
            let (head, tail) = haystack.split_at(g + 1);
            if let Some(_) = head.strip_suffix(b"encoding") {
                return Some(tail);
            }
            haystack = tail;
        } else {
            return None;
        }
    }
}

/// Extracts an encoding from an ASCII-based bogo-XML declaration.
/// `bytes` must the prefix of a `text/html` resource.
///
/// The intended use is that when the `meta` prescan fails, the HTML
/// parser will have buffered the head section or the first 1024
/// bytes (whichever is larger) at which point the should be passed to
/// this function.
pub fn parse(bytes: &[u8]) -> Option<&'static encoding_rs::Encoding> {
    if let Some(after_xml) = bytes.strip_prefix(b"<?xml") {
        if let Some(gt) = position(b'>', after_xml) {
            let until_gt = &after_xml[..gt];
            if let Some(tail) = skip_encoding(until_gt) {
                let mut pos = 0;
                loop {
                    if pos >= tail.len() {
                        return None;
                    }
                    let c = tail[pos];
                    pos += 1;
                    if c == b'=' {
                        break;
                    }
                    if c <= b' ' {
                        continue;
                    }
                    return None;
                }
                let is_single_quoted;
                let label_start;
                loop {
                    if pos >= tail.len() {
                        return None;
                    }
                    let c = tail[pos];
                    pos += 1;
                    if c == b'"' {
                        is_single_quoted = false;
                        label_start = pos;
                        break;
                    }
                    if c == b'\'' {
                        is_single_quoted = true;
                        label_start = pos;
                        break;
                    }
                    if c <= b' ' {
                        continue;
                    }
                    return None;
                }
                loop {
                    if pos >= tail.len() {
                        return None;
                    }
                    let c = tail[pos];
                    if c <= b' ' {
                        return None;
                    }
                    if (c == b'"' && !is_single_quoted) || (c == b'\'' && is_single_quoted) {
                        let encoding = encoding_rs::Encoding::for_label(&tail[label_start..pos]);
                        if encoding == Some(encoding_rs::UTF_16LE)
                            || encoding == Some(encoding_rs::UTF_16BE)
                        {
                            return Some(encoding_rs::UTF_8);
                        }
                        return encoding;
                    }
                    pos += 1;
                }
            }
        }
    }
    None
}

// Any copyright to the test code below this comment is dedicated to the
