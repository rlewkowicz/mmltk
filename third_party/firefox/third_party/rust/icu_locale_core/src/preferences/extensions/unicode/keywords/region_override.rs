// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::extensions::unicode::{SubdivisionId, Value};
use crate::preferences::extensions::unicode::errors::PreferencesParseError;
use crate::preferences::extensions::unicode::struct_keyword;

struct_keyword!(
    /// A Region Override specifies an alternate region to use for obtaining certain region-specific default values.
    ///
    /// The valid values are listed in [LDML](https://unicode.org/reports/tr35/#RegionOverride).
    [Copy]
    RegionOverride,
    "rg",
    SubdivisionId,
    |input: Value| {
        input
            .into_single_subtag()
            .and_then(|subtag| subtag.as_str().parse().ok().map(Self))
            .ok_or(PreferencesParseError::InvalidKeywordValue)
    },
    |input: RegionOverride| {
        Value::from_subtag(Some(input.0.into_subtag()))
    }
);
