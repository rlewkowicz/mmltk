// Copyright 2015 The Servo Project Developers. See the
// COPYRIGHT file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Accessor for `Bidi_Class` property from Unicode Character Database (UCD)

mod tables;

pub use self::tables::{BidiClass, UNICODE_VERSION};
#[cfg(feature = "hardcoded-data")]
use core::char;
#[cfg(feature = "hardcoded-data")]
use core::cmp::Ordering::{Equal, Greater, Less};

#[cfg(feature = "hardcoded-data")]
use self::tables::bidi_class_table;
use crate::data_source::BidiMatchedOpeningBracket;
use crate::BidiClass::*;
#[cfg(feature = "hardcoded-data")]
use crate::BidiDataSource;
/// Hardcoded Bidi data that ships with the unicode-bidi crate.
///
/// This can be enabled with the default `hardcoded-data` Cargo feature.
#[cfg(feature = "hardcoded-data")]
pub struct HardcodedBidiData;

#[cfg(feature = "hardcoded-data")]
impl BidiDataSource for HardcodedBidiData {
    fn bidi_class(&self, c: char) -> BidiClass {
        bsearch_range_value_table(c, bidi_class_table)
    }
}

/// Find the `BidiClass` of a single char.
#[cfg(feature = "hardcoded-data")]
pub fn bidi_class(c: char) -> BidiClass {
    bsearch_range_value_table(c, bidi_class_table)
}

/// If this character is a bracket according to BidiBrackets.txt,
/// return the corresponding *normalized* *opening bracket* of the pair,
/// and whether or not it itself is an opening bracket.
pub(crate) fn bidi_matched_opening_bracket(c: char) -> Option<BidiMatchedOpeningBracket> {
    for pair in self::tables::bidi_pairs_table {
        if pair.0 == c || pair.1 == c {
            let skeleton = pair.2.unwrap_or(pair.0);
            return Some(BidiMatchedOpeningBracket {
                opening: skeleton,
                is_open: pair.0 == c,
            });
        }
    }
    None
}

pub fn is_rtl(bidi_class: BidiClass) -> bool {
    matches!(bidi_class, RLE | RLO | RLI)
}

#[cfg(feature = "hardcoded-data")]
fn bsearch_range_value_table(c: char, r: &'static [(char, char, BidiClass)]) -> BidiClass {
    match r.binary_search_by(|&(lo, hi, _)| {
        if lo <= c && c <= hi {
            Equal
        } else if hi < c {
            Less
        } else {
            Greater
        }
    }) {
        Ok(idx) => {
            let (_, _, cat) = r[idx];
            cat
        }
        Err(_) => L,
    }
}
