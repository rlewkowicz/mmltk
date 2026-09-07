// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::preferences::extensions::unicode::errors::PreferencesParseError;
use crate::preferences::extensions::unicode::struct_keyword;
use crate::{
    extensions::unicode::{SubdivisionId, Value},
    subtags::Subtag,
};

struct_keyword!(
    /// A Unicode Subdivision Identifier defines a regional subdivision used for locales.
    ///
    /// The valid values are listed in [LDML](https://unicode.org/reports/tr35/#UnicodeSubdivisionIdentifier).
    [Copy]
    RegionalSubdivision,
    "sd",
    SubdivisionId,
    |input: Value| {
        input
            .into_single_subtag()
            .and_then(|subtag| subtag.as_str().parse().ok().map(Self))
            .ok_or(PreferencesParseError::InvalidKeywordValue)
    },
    |input: RegionalSubdivision| {
        let mut raw = [0; 8];
        raw[0] = input.0.region.into_raw()[0];
        raw[1] = input.0.region.into_raw()[1];
        raw[2] = input.0.region.into_raw()[2];
        let len = input.0.region.as_str().len();
        debug_assert!((2..=3).contains(&len));
        #[allow(clippy::indexing_slicing)] 
        {
            raw[len] = input.0.suffix.into_raw()[0];
            raw[len + 1] = input.0.suffix.into_raw()[1];
            raw[len + 2] = input.0.suffix.into_raw()[2];
            raw[len + 3] = input.0.suffix.into_raw()[2];
        }
        #[expect(clippy::unwrap_used)] 
        Value::from_subtag(Some(Subtag::try_from_raw(raw).unwrap()))
    }
);
