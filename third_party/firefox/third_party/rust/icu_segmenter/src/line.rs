// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::complex::*;
use crate::indices::*;
use crate::provider::*;
use crate::rule_segmenter::*;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;
use core::char;
use icu_locale_core::subtags::{language, Language};
use icu_locale_core::LanguageIdentifier;
use icu_provider::prelude::*;
use utf8_iter::Utf8CharIndices;

#[allow(dead_code)]
const UNKNOWN: u8 = 0;
#[allow(dead_code)]
const AI: u8 = 1;
#[allow(dead_code)]
const AK: u8 = 2;
#[allow(dead_code)]
const AL: u8 = 3;
#[allow(dead_code)]
const AL_DOTTED_CIRCLE: u8 = 4;
#[allow(dead_code)]
const AP: u8 = 5;
#[allow(dead_code)]
const AS: u8 = 6;
#[allow(dead_code)]
const B2: u8 = 7;
#[allow(dead_code)]
const BA: u8 = 8;
#[allow(dead_code)]
const BB: u8 = 9;
#[allow(dead_code)]
const BK: u8 = 10;
#[allow(dead_code)]
const CB: u8 = 11;
#[allow(dead_code)]
const CJ: u8 = 12;
#[allow(dead_code)]
const CL: u8 = 13;
#[allow(dead_code)]
const CM: u8 = 14;
#[allow(dead_code)]
const CP: u8 = 15;
#[allow(dead_code)]
const CR: u8 = 16;
#[allow(dead_code)]
const EB: u8 = 17;
#[allow(dead_code)]
const EM: u8 = 18;
#[allow(dead_code)]
const EX: u8 = 19;
#[allow(dead_code)]
const GL: u8 = 20;
#[allow(dead_code)]
const H2: u8 = 21;
#[allow(dead_code)]
const H3: u8 = 22;
#[allow(dead_code)]
const HL: u8 = 23;
#[allow(dead_code)]
const HY: u8 = 24;
#[allow(dead_code)]
const ID: u8 = 25;
#[allow(dead_code)]
const ID_CN: u8 = 26;
#[allow(dead_code)]
const IN: u8 = 27;
#[allow(dead_code)]
const IS: u8 = 28;
#[allow(dead_code)]
const JL: u8 = 29;
#[allow(dead_code)]
const JT: u8 = 30;
#[allow(dead_code)]
const JV: u8 = 31;
#[allow(dead_code)]
const LF: u8 = 32;
#[allow(dead_code)]
const NL: u8 = 33;
#[allow(dead_code)]
const NS: u8 = 34;
#[allow(dead_code)]
const NU: u8 = 35;
#[allow(dead_code)]
const OP_EA: u8 = 36;
#[allow(dead_code)]
const OP_OP30: u8 = 37;
#[allow(dead_code)]
const PO: u8 = 38;
#[allow(dead_code)]
const PO_EAW: u8 = 39;
#[allow(dead_code)]
const PR: u8 = 40;
#[allow(dead_code)]
const PR_EAW: u8 = 41;
#[allow(dead_code)]
const QU: u8 = 42;
#[allow(dead_code)]
const QU_PF: u8 = 43;
#[allow(dead_code)]
const QU_PI: u8 = 44;
#[allow(dead_code)]
const RI: u8 = 45;
#[allow(dead_code)]
const SA: u8 = 46;
#[allow(dead_code)]
const SP: u8 = 47;
#[allow(dead_code)]
const SY: u8 = 48;
#[allow(dead_code)]
const VF: u8 = 49;
#[allow(dead_code)]
const VI: u8 = 50;
#[allow(dead_code)]
const WJ: u8 = 51;
#[allow(dead_code)]
const XX: u8 = 52;
#[allow(dead_code)]
const ZW: u8 = 53;
#[allow(dead_code)]
const ZWJ: u8 = 54;

/// An enum specifies the strictness of line-breaking rules. It can be passed as
/// an argument when creating a line segmenter.
///
/// Each enum value has the same meaning with respect to the `line-break`
/// property values in the CSS Text spec. See the details in
/// <https://drafts.csswg.org/css-text-3/#line-break-property>.
#[non_exhaustive]
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub enum LineBreakStrictness {
    /// Breaks text using the least restrictive set of line-breaking rules.
    /// Typically used for short lines, such as in newspapers.
    /// <https://drafts.csswg.org/css-text-3/#valdef-line-break-loose>
    Loose,

    /// Breaks text using the most common set of line-breaking rules.
    /// <https://drafts.csswg.org/css-text-3/#valdef-line-break-normal>
    Normal,

    /// Breaks text using the most stringent set of line-breaking rules.
    /// <https://drafts.csswg.org/css-text-3/#valdef-line-break-strict>
    ///
    /// This is the default behaviour of the Unicode Line Breaking Algorithm,
    /// resolving class [CJ](https://www.unicode.org/reports/tr14/#CJ) to
    /// [NS](https://www.unicode.org/reports/tr14/#NS);
    /// see rule [LB1](https://www.unicode.org/reports/tr14/#LB1).
    #[default]
    Strict,

    /// Breaks text assuming there is a soft wrap opportunity around every
    /// typographic character unit, disregarding any prohibition against line
    /// breaks. See more details in
    /// <https://drafts.csswg.org/css-text-3/#valdef-line-break-anywhere>.
    Anywhere,
}

/// An enum specifies the line break opportunities between letters. It can be
/// passed as an argument when creating a line segmenter.
///
/// Each enum value has the same meaning with respect to the `word-break`
/// property values in the CSS Text spec. See the details in
/// <https://drafts.csswg.org/css-text-3/#word-break-property>
#[non_exhaustive]
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub enum LineBreakWordOption {
    /// Words break according to their customary rules. See the details in
    /// <https://drafts.csswg.org/css-text-3/#valdef-word-break-normal>.
    #[default]
    Normal,

    /// Breaking is allowed within "words".
    /// <https://drafts.csswg.org/css-text-3/#valdef-word-break-break-all>
    BreakAll,

    /// Breaking is forbidden within "word".
    /// <https://drafts.csswg.org/css-text-3/#valdef-word-break-keep-all>
    KeepAll,
}

/// Options to tailor line-breaking behavior.
#[non_exhaustive]
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub struct LineBreakOptions<'a> {
    /// Strictness of line-breaking rules. See [`LineBreakStrictness`].
    ///
    /// Default is [`LineBreakStrictness::Strict`]
    pub strictness: Option<LineBreakStrictness>,

    /// Line break opportunities between letters. See [`LineBreakWordOption`].
    ///
    /// Default is [`LineBreakStrictness::Normal`]
    pub word_option: Option<LineBreakWordOption>,

    /// Content locale for line segmenter
    ///
    /// This allows more break opportunities when `LineBreakStrictness` is
    /// `Normal` or `Loose`. See
    /// <https://drafts.csswg.org/css-text-3/#line-break-property> for details.
    /// This option has no effect in Latin-1 mode.
    pub content_locale: Option<&'a LanguageIdentifier>,
}

impl LineBreakOptions<'_> {
    /// `const` version of [`Default::default`]
    pub const fn default() -> Self {
        Self {
            strictness: None,
            word_option: None,
            content_locale: None,
        }
    }
}

#[derive(Debug, Clone, Copy)]
struct ResolvedLineBreakOptions {
    strictness: LineBreakStrictness,
    word_option: LineBreakWordOption,
    ja_zh: bool,
}

impl LineBreakOptions<'_> {
    const fn resolve(self) -> ResolvedLineBreakOptions {
        ResolvedLineBreakOptions {
            strictness: match self.strictness {
                Some(s) => s,
                None => LineBreakStrictness::Strict,
            },
            word_option: match self.word_option {
                Some(s) => s,
                None => LineBreakWordOption::Normal,
            },
            ja_zh: if let Some(content_locale) = self.content_locale.as_ref() {
                const JA: Language = language!("ja");
                const ZH: Language = language!("zh");
                matches!(content_locale.language, JA | ZH)
            } else {
                false
            },
        }
    }
}

/// Supports loading line break data, and creating line break iterators for different string
/// encodings.
///
/// Most segmentation methods live on [`LineSegmenterBorrowed`], which can be obtained via
/// [`LineSegmenter::new_auto()`] (etc) or [`LineSegmenter::as_borrowed()`].
///
/// The segmenter returns mandatory breaks (as defined by [definition LD7][LD7] of
/// Unicode Standard Annex #14, _Unicode Line Breaking Algorithm_) as well as
/// line break opportunities ([definition LD3][LD3]).
/// It does not distinguish them.  Callers requiring that distinction can check
/// the Line_Break property of the code point preceding the break against those
/// listed in rules [LB4][LB4] and [LB5][LB5], special-casing the end of text
/// according to [LB3][LB3].
///
/// For consistency with the grapheme, word, and sentence segmenters, there is
/// always a breakpoint returned at index 0, but this breakpoint is not a
/// meaningful line break opportunity.
///
/// [LD3]: https://www.unicode.org/reports/tr14/#LD3
/// [LD7]: https://www.unicode.org/reports/tr14/#LD7
/// [LB3]: https://www.unicode.org/reports/tr14/#LB3
/// [LB4]: https://www.unicode.org/reports/tr14/#LB4
/// [LB5]: https://www.unicode.org/reports/tr14/#LB5
///
/// ```rust
/// # use icu::segmenter::LineSegmenter;
/// #
/// # let segmenter = LineSegmenter::new_auto(Default::default());
/// #
/// let text = "Summary\r\nThis annex…";
/// let breakpoints: Vec<usize> = segmenter.segment_str(text).collect();
/// // 9 and 22 are mandatory breaks, 14 is a line break opportunity.
/// assert_eq!(&breakpoints, &[0, 9, 14, 22]);
///
/// // There is a break opportunity between emoji, but not within the ZWJ sequence 🏳️‍🌈.
/// let flag_equation = "🏳️➕🌈🟰🏳️\u{200D}🌈";
/// let possible_first_lines: Vec<&str> =
///     segmenter.segment_str(flag_equation).skip(1).map(|i| &flag_equation[..i]).collect();
/// assert_eq!(
///     &possible_first_lines,
///     &[
///         "🏳️",
///         "🏳️➕",
///         "🏳️➕🌈",
///         "🏳️➕🌈🟰",
///         "🏳️➕🌈🟰🏳️‍🌈"
///     ]
/// );
/// ```
///
/// # Examples
///
/// Segment a string with default options:
///
/// ```rust
/// use icu::segmenter::LineSegmenter;
///
/// let segmenter = LineSegmenter::new_auto(Default::default());
///
/// let breakpoints: Vec<usize> =
///     segmenter.segment_str("Hello World").collect();
/// assert_eq!(&breakpoints, &[0, 6, 11]);
/// ```
///
/// Segment a string with CSS option overrides:
///
/// ```rust
/// use icu::segmenter::options::{
///     LineBreakOptions, LineBreakStrictness, LineBreakWordOption,
/// };
/// use icu::segmenter::LineSegmenter;
///
/// let mut options = LineBreakOptions::default();
/// options.strictness = Some(LineBreakStrictness::Strict);
/// options.word_option = Some(LineBreakWordOption::BreakAll);
/// options.content_locale = None;
/// let segmenter = LineSegmenter::new_auto(options);
///
/// let breakpoints: Vec<usize> =
///     segmenter.segment_str("Hello World").collect();
/// assert_eq!(&breakpoints, &[0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11]);
/// ```
///
/// Segment a Latin1 byte string:
///
/// ```rust
/// use icu::segmenter::LineSegmenter;
///
/// let segmenter = LineSegmenter::new_auto(Default::default());
///
/// let breakpoints: Vec<usize> =
///     segmenter.segment_latin1(b"Hello World").collect();
/// assert_eq!(&breakpoints, &[0, 6, 11]);
/// ```
///
/// Separate mandatory breaks from the break opportunities:
///
/// ```rust
/// use icu::properties::{props::LineBreak, CodePointMapData};
/// use icu::segmenter::LineSegmenter;
///
/// # let segmenter = LineSegmenter::new_auto(Default::default());
/// #
/// let text = "Summary\r\nThis annex…";
///
/// let mandatory_breaks: Vec<usize> = segmenter
///     .segment_str(text)
///     .filter(|&i| {
///         text[..i].chars().next_back().is_some_and(|c| {
///             matches!(
///                 CodePointMapData::<LineBreak>::new().get(c),
///                 LineBreak::MandatoryBreak
///                     | LineBreak::CarriageReturn
///                     | LineBreak::LineFeed
///                     | LineBreak::NextLine
///             ) || i == text.len()
///         })
///     })
///     .collect();
/// assert_eq!(&mandatory_breaks, &[9, 22]);
/// ```
#[derive(Debug)]
pub struct LineSegmenter {
    options: ResolvedLineBreakOptions,
    payload: DataPayload<SegmenterBreakLineV1>,
    complex: ComplexPayloads,
}

/// Segments a string into lines (borrowed version).
///
/// See [`LineSegmenter`] for examples.
#[derive(Clone, Debug, Copy)]
pub struct LineSegmenterBorrowed<'data> {
    options: ResolvedLineBreakOptions,
    data: &'data RuleBreakData<'data>,
    complex: ComplexPayloadsBorrowed<'data>,
}

impl LineSegmenter {
    /// Constructs a [`LineSegmenter`] with an invariant locale, custom [`LineBreakOptions`], and
    /// the best available compiled data for complex scripts (Khmer, Lao, Myanmar, and Thai).
    ///
    /// The current behavior, which is subject to change, is to use the LSTM model when available.
    ///
    /// See also [`Self::new_auto`].
    ///
    /// ✨ *Enabled with the `compiled_data` and `auto` Cargo features.*
    ///
    /// [📚 Help choosing a constructor](icu_provider::constructors)
    #[cfg(feature = "auto")]
    #[cfg(feature = "compiled_data")]
    pub fn new_auto(options: LineBreakOptions) -> LineSegmenterBorrowed<'static> {
        Self::new_lstm(options)
    }

    #[cfg(feature = "auto")]
    icu_provider::gen_buffer_data_constructors!(
        (options: LineBreakOptions) -> error: DataError,
        functions: [
            new_auto: skip,
            try_new_auto_with_buffer_provider,
            try_new_auto_unstable,
            Self,
        ]
    );

    #[cfg(feature = "auto")]
    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new_auto)]
    pub fn try_new_auto_unstable<D>(
        provider: &D,
        options: LineBreakOptions,
    ) -> Result<Self, DataError>
    where
        D: DataProvider<SegmenterBreakLineV1>
            + DataProvider<SegmenterLstmAutoV1>
            + DataProvider<SegmenterBreakGraphemeClusterV1>
            + ?Sized,
    {
        Self::try_new_lstm_unstable(provider, options)
    }

    /// Constructs a [`LineSegmenter`] with an invariant locale, custom [`LineBreakOptions`], and
    /// compiled LSTM data for complex scripts (Khmer, Lao, Myanmar, and Thai).
    ///
    /// The LSTM, or Long Term Short Memory, is a machine learning model. It is smaller than
    /// the full dictionary but more expensive during segmentation (inference).
    ///
    /// See also [`Self::new_lstm`].
    ///
    /// ✨ *Enabled with the `compiled_data` and `lstm` Cargo features.*
    ///
    /// [📚 Help choosing a constructor](icu_provider::constructors)
    #[cfg(feature = "lstm")]
    #[cfg(feature = "compiled_data")]
    pub fn new_lstm(options: LineBreakOptions) -> LineSegmenterBorrowed<'static> {
        LineSegmenterBorrowed {
            options: options.resolve(),
            data: crate::provider::Baked::SINGLETON_SEGMENTER_BREAK_LINE_V1,
            complex: ComplexPayloadsBorrowed::new_lstm(),
        }
    }

    #[cfg(feature = "lstm")]
    icu_provider::gen_buffer_data_constructors!(
        (options: LineBreakOptions) -> error: DataError,
        functions: [
            try_new_lstm: skip,
            try_new_lstm_with_buffer_provider,
            try_new_lstm_unstable,
            Self,
        ]
    );

    #[cfg(feature = "lstm")]
    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new_lstm)]
    pub fn try_new_lstm_unstable<D>(
        provider: &D,
        options: LineBreakOptions,
    ) -> Result<Self, DataError>
    where
        D: DataProvider<SegmenterBreakLineV1>
            + DataProvider<SegmenterLstmAutoV1>
            + DataProvider<SegmenterBreakGraphemeClusterV1>
            + ?Sized,
    {
        Ok(Self {
            options: options.resolve(),
            payload: provider.load(Default::default())?.payload,
            complex: ComplexPayloads::try_new_lstm(provider)?,
        })
    }

    /// Constructs a [`LineSegmenter`] with an invariant locale, custom [`LineBreakOptions`], and
    /// compiled dictionary data for complex scripts (Khmer, Lao, Myanmar, and Thai).
    ///
    /// The dictionary model uses a list of words to determine appropriate breakpoints. It is
    /// faster than the LSTM model but requires more data.
    ///
    /// ✨ *Enabled with the `compiled_data` Cargo feature.*
    ///
    /// [📚 Help choosing a constructor](icu_provider::constructors)
    #[cfg(feature = "compiled_data")]
    pub fn new_dictionary(options: LineBreakOptions) -> LineSegmenterBorrowed<'static> {
        LineSegmenterBorrowed {
            options: options.resolve(),
            data: crate::provider::Baked::SINGLETON_SEGMENTER_BREAK_LINE_V1,
            complex: ComplexPayloadsBorrowed::new_southeast_asian(),
        }
    }

    icu_provider::gen_buffer_data_constructors!(
        (options: LineBreakOptions) -> error: DataError,
        functions: [
            new_dictionary: skip,
            try_new_dictionary_with_buffer_provider,
            try_new_dictionary_unstable,
            Self,
        ]
    );

    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new_dictionary)]
    pub fn try_new_dictionary_unstable<D>(
        provider: &D,
        options: LineBreakOptions,
    ) -> Result<Self, DataError>
    where
        D: DataProvider<SegmenterBreakLineV1>
            + DataProvider<SegmenterDictionaryExtendedV1>
            + DataProvider<SegmenterBreakGraphemeClusterV1>
            + ?Sized,
    {
        Ok(Self {
            options: options.resolve(),
            payload: provider.load(Default::default())?.payload,
            complex: ComplexPayloads::try_new_southeast_asian(provider)?,
        })
    }

    /// Constructs a [`LineSegmenter`] with an invariant locale, custom [`LineBreakOptions`], and
    /// no support for scripts requiring complex context dependent line breaks (Khmer, Lao, Myanmar, Thai).
    ///
    /// ✨ *Enabled with the `compiled_data` Cargo feature.*
    ///
    /// [📚 Help choosing a constructor](icu_provider::constructors)
    #[cfg(feature = "compiled_data")]
    pub const fn new_for_non_complex_scripts(
        options: LineBreakOptions,
    ) -> LineSegmenterBorrowed<'static> {
        LineSegmenterBorrowed {
            options: options.resolve(),
            data: crate::provider::Baked::SINGLETON_SEGMENTER_BREAK_LINE_V1,
            complex: ComplexPayloadsBorrowed::empty(),
        }
    }

    icu_provider::gen_buffer_data_constructors!(
        (options: LineBreakOptions) -> error: DataError,
        functions: [
            new_for_non_complex_scripts: skip,
            try_new_for_non_complex_scripts_with_buffer_provider,
            try_new_for_non_complex_scripts_unstable,
            Self,
        ]
    );

    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new_for_non_complex_scripts)]
    pub fn try_new_for_non_complex_scripts_unstable<D>(
        provider: &D,
        options: LineBreakOptions,
    ) -> Result<Self, DataError>
    where
        D: DataProvider<SegmenterBreakLineV1>
            + DataProvider<SegmenterBreakGraphemeClusterV1>
            + ?Sized,
    {
        Ok(Self {
            options: options.resolve(),
            payload: provider.load(Default::default())?.payload,
            complex: ComplexPayloads::try_new_empty(provider)?,
        })
    }

    /// Constructs a borrowed version of this type for more efficient querying.
    ///
    /// Most useful methods for segmentation are on this type.
    pub fn as_borrowed(&self) -> LineSegmenterBorrowed<'_> {
        LineSegmenterBorrowed {
            options: self.options,
            data: self.payload.get(),
            complex: self.complex.as_borrowed(),
        }
    }
}

impl<'data> LineSegmenterBorrowed<'data> {
    /// Creates a line break iterator for an `str` (a UTF-8 string).
    ///
    /// There are always breakpoints at 0 and the string length, or only at 0 for the empty string.
    pub fn segment_str<'s>(self, input: &'s str) -> LineBreakIterator<'data, 's, Utf8> {
        LineBreakIterator {
            iter: input.char_indices(),
            len: input.len(),
            current_pos_data: None,
            result_cache: Vec::new(),
            data: self.data,
            options: self.options,
            complex: self.complex,
        }
    }
    /// Creates a line break iterator for a potentially ill-formed UTF8 string
    ///
    /// Invalid characters are treated as REPLACEMENT CHARACTER
    ///
    /// There are always breakpoints at 0 and the string length, or only at 0 for the empty string.
    pub fn segment_utf8<'s>(
        self,
        input: &'s [u8],
    ) -> LineBreakIterator<'data, 's, PotentiallyIllFormedUtf8> {
        LineBreakIterator {
            iter: Utf8CharIndices::new(input),
            len: input.len(),
            current_pos_data: None,
            result_cache: Vec::new(),
            data: self.data,
            options: self.options,
            complex: self.complex,
        }
    }
    /// Creates a line break iterator for a Latin-1 (8-bit) string.
    ///
    /// There are always breakpoints at 0 and the string length, or only at 0 for the empty string.
    pub fn segment_latin1<'s>(self, input: &'s [u8]) -> LineBreakIterator<'data, 's, Latin1> {
        LineBreakIterator {
            iter: Latin1Indices::new(input),
            len: input.len(),
            current_pos_data: None,
            result_cache: Vec::new(),
            data: self.data,
            options: self.options,
            complex: self.complex,
        }
    }

    /// Creates a line break iterator for a UTF-16 string.
    ///
    /// There are always breakpoints at 0 and the string length, or only at 0 for the empty string.
    pub fn segment_utf16<'s>(self, input: &'s [u16]) -> LineBreakIterator<'data, 's, Utf16> {
        LineBreakIterator {
            iter: Utf16Indices::new(input),
            len: input.len(),
            current_pos_data: None,
            result_cache: Vec::new(),
            data: self.data,
            options: self.options,
            complex: self.complex,
        }
    }
}

impl LineSegmenterBorrowed<'static> {
    /// Cheaply converts a [`LineSegmenterBorrowed<'static>`] into a [`LineSegmenter`].
    ///
    /// Note: Due to branching and indirection, using [`LineSegmenter`] might inhibit some
    /// compile-time optimizations that are possible with [`LineSegmenterBorrowed`].
    pub fn static_to_owned(self) -> LineSegmenter {
        LineSegmenter {
            payload: DataPayload::from_static_ref(self.data),
            complex: self.complex.static_to_owned(),
            options: self.options,
        }
    }
}

impl RuleBreakData<'_> {
    fn get_linebreak_property_utf32_with_rule(
        &self,
        codepoint: u32,
        strictness: LineBreakStrictness,
        word_option: LineBreakWordOption,
    ) -> u8 {
        let prop = self.property_table.get32(codepoint);

        if word_option == LineBreakWordOption::BreakAll
            || strictness == LineBreakStrictness::Loose
            || strictness == LineBreakStrictness::Normal
        {
            return match prop {
                CJ => ID, 
                _ => prop,
            };
        }

        prop
    }

    #[inline]
    fn get_break_state_from_table(&self, left: u8, right: u8) -> BreakState {
        let idx = (left as usize) * (self.property_count as usize) + (right as usize);
        self.break_state_table.get(idx).unwrap_or(BreakState::Keep)
    }

    #[inline]
    fn use_complex_breaking_utf32(&self, codepoint: u32) -> bool {
        let line_break_property = self.get_linebreak_property_utf32_with_rule(
            codepoint,
            LineBreakStrictness::Strict,
            LineBreakWordOption::Normal,
        );

        line_break_property == SA
    }
}

#[inline]
fn is_break_utf32_by_loose(
    right_codepoint: u32,
    left_prop: u8,
    right_prop: u8,
    ja_zh: bool,
) -> Option<bool> {
    if right_prop == BA {
        if left_prop == ID && (right_codepoint == 0x2010 || right_codepoint == 0x2013) {
            return Some(true);
        }
    } else if right_prop == NS {
        if right_codepoint == 0x301C || right_codepoint == 0x30A0 {
            return Some(ja_zh);
        }

        if right_codepoint == 0x3005
            || right_codepoint == 0x303B
            || right_codepoint == 0x309D
            || right_codepoint == 0x309E
            || right_codepoint == 0x30FD
            || right_codepoint == 0x30FE
        {
            return Some(true);
        }

        if right_codepoint == 0x30FB
            || right_codepoint == 0xFF1A
            || right_codepoint == 0xFF1B
            || right_codepoint == 0xFF65
            || right_codepoint == 0x203C
            || (0x2047..=0x2049).contains(&right_codepoint)
        {
            return Some(ja_zh);
        }
    } else if right_prop == IN {
        return Some(true);
    } else if right_prop == EX {
        if right_codepoint == 0xFF01 || right_codepoint == 0xFF1F {
            return Some(ja_zh);
        }
    }

    if right_prop == PO_EAW {
        return Some(ja_zh);
    }
    if left_prop == PR_EAW {
        return Some(ja_zh);
    }
    None
}

/// A trait allowing for LineBreakIterator to be generalized to multiple string iteration methods.
///
/// This is implemented by ICU4X for several common string types.
///
/// <div class="stab unstable">
/// 🚫 This trait is sealed; it cannot be implemented by user code. If an API requests an item that implements this
/// trait, please consider using a type from the implementors listed below.
/// </div>
pub trait LineBreakType: crate::private::Sealed + Sized + RuleBreakType {
    #[doc(hidden)]
    fn use_complex_breaking(iterator: &LineBreakIterator<'_, '_, Self>, c: Self::CharType) -> bool;

    #[doc(hidden)]
    fn get_linebreak_property_with_rule(
        iterator: &LineBreakIterator<'_, '_, Self>,
        c: Self::CharType,
    ) -> u8;

    #[doc(hidden)]
    fn line_handle_complex_language(
        iterator: &mut LineBreakIterator<'_, '_, Self>,
        left_codepoint: Self::CharType,
    ) -> Option<usize>;
}

/// Implements the [`Iterator`] trait over the line break opportunities of the given string.
///
/// Lifetimes:
///
/// - `'l` = lifetime of the [`LineSegmenter`] object from which this iterator was created
/// - `'s` = lifetime of the string being segmented
///
/// The [`Iterator::Item`] is an [`usize`] representing index of a code unit
/// _after_ the break (for a break at the end of text, this index is the length
/// of the [`str`] or array of code units).
///
/// For examples of use, see [`LineSegmenter`].
#[derive(Debug)]
pub struct LineBreakIterator<'data, 's, Y: LineBreakType> {
    iter: Y::IterAttr<'s>,
    len: usize,
    current_pos_data: Option<(usize, Y::CharType)>,
    result_cache: Vec<usize>,
    data: &'data RuleBreakData<'data>,
    options: ResolvedLineBreakOptions,
    complex: ComplexPayloadsBorrowed<'data>,
}

impl<Y: LineBreakType> Iterator for LineBreakIterator<'_, '_, Y> {
    type Item = usize;

    fn next(&mut self) -> Option<Self::Item> {
        match self.check_eof() {
            StringBoundaryPosType::Start => return Some(0),
            StringBoundaryPosType::End => return None,
            _ => (),
        }

        if let Some(&first_pos) = self.result_cache.first() {
            let mut i = 0;
            loop {
                if i == first_pos {
                    self.result_cache = self.result_cache.iter().skip(1).map(|r| r - i).collect();
                    return self.get_current_position();
                }
                i += self.get_current_codepoint().map_or(0, Y::char_len);
                self.advance_iter();
                if self.is_eof() {
                    self.result_cache.clear();
                    return Some(self.len);
                }
            }
        }

        let mut lb9_left: Option<u8> = None;
        let mut lb8a_after_lb9 = false;

        'a: loop {
            debug_assert!(!self.is_eof());
            let left_codepoint = self.get_current_codepoint()?;
            let mut left_prop =
                lb9_left.unwrap_or_else(|| self.get_linebreak_property(left_codepoint));
            let after_zwj = lb8a_after_lb9 || (lb9_left.is_none() && left_prop == ZWJ);
            self.advance_iter();

            let Some(right_codepoint) = self.get_current_codepoint() else {
                return Some(self.len);
            };
            let right_prop = self.get_linebreak_property(right_codepoint);
            if (right_prop == CM
                || (right_prop == ZWJ && self.options.strictness != LineBreakStrictness::Anywhere))
                && left_prop != BK
                && left_prop != CR
                && left_prop != LF
                && left_prop != NL
                && left_prop != SP
                && left_prop != ZW
            {
                lb9_left = Some(left_prop);
                lb8a_after_lb9 = right_prop == ZWJ;
                continue;
            } else {
                lb9_left = None;
                lb8a_after_lb9 = false;
            }

            match (self.options.word_option, left_prop, right_prop) {
                (LineBreakWordOption::BreakAll, AL | NU | SA, _) => {
                    left_prop = ID;
                }
                (
                    LineBreakWordOption::KeepAll,
                    AI | AL | ID | NU | HY | H2 | H3 | JL | JV | JT | CJ,
                    AI | AL | ID | NU | HY | H2 | H3 | JL | JV | JT | CJ,
                ) => {
                    continue;
                }
                _ => (),
            }

            match self.options.strictness {
                LineBreakStrictness::Normal => {
                    if self.is_break_by_normal(right_codepoint) && !after_zwj {
                        return self.get_current_position();
                    }
                }
                LineBreakStrictness::Loose => {
                    if let Some(breakable) = is_break_utf32_by_loose(
                        right_codepoint.into(),
                        left_prop,
                        right_prop,
                        self.options.ja_zh,
                    ) {
                        if breakable && !after_zwj {
                            return self.get_current_position();
                        }
                        continue;
                    }
                }
                LineBreakStrictness::Anywhere => {
                    return self.get_current_position();
                }
                _ => (),
            };

            if self.options.word_option != LineBreakWordOption::BreakAll
                && Y::use_complex_breaking(self, left_codepoint)
                && Y::use_complex_breaking(self, right_codepoint)
            {
                let result = Y::line_handle_complex_language(self, left_codepoint);
                if result.is_some() {
                    return result;
                }
            }

            match self.data.get_break_state_from_table(left_prop, right_prop) {
                BreakState::Break | BreakState::NoMatch => {
                    if after_zwj {
                        continue;
                    } else {
                        return self.get_current_position();
                    }
                }
                BreakState::Keep => continue,
                BreakState::Index(mut index) | BreakState::Intermediate(mut index) => {
                    let mut previous_iter = self.iter.clone();
                    let mut previous_pos_data = self.current_pos_data;
                    let mut previous_is_after_zwj = after_zwj;

                    let mut left_prop_pre_lb9 = right_prop;

                    let is_intermediate_rule_no_match = if lb8a_after_lb9 {
                        true
                    } else {
                        index > self.data.last_codepoint_property
                    };

                    loop {
                        self.advance_iter();
                        let after_zwj = left_prop_pre_lb9 == ZWJ;

                        let previous_break_state_is_cp_prop =
                            index <= self.data.last_codepoint_property;

                        let Some(prop) = self.get_current_linebreak_property() else {
                            let break_state = self
                                .data
                                .get_break_state_from_table(index, self.data.eot_property);
                            if break_state == BreakState::NoMatch {
                                self.iter = previous_iter;
                                self.current_pos_data = previous_pos_data;
                                if previous_is_after_zwj {
                                    continue 'a;
                                } else {
                                    return self.get_current_position();
                                }
                            }
                            return Some(self.len);
                        };

                        if (prop == CM || prop == ZWJ)
                            && left_prop_pre_lb9 != BK
                            && left_prop_pre_lb9 != CR
                            && left_prop_pre_lb9 != LF
                            && left_prop_pre_lb9 != NL
                            && left_prop_pre_lb9 != SP
                            && left_prop_pre_lb9 != ZW
                        {
                            left_prop_pre_lb9 = prop;
                            continue;
                        }

                        match self.data.get_break_state_from_table(index, prop) {
                            BreakState::Keep => continue 'a,
                            BreakState::NoMatch => {
                                self.iter = previous_iter;
                                self.current_pos_data = previous_pos_data;
                                if after_zwj {
                                    if is_intermediate_rule_no_match && !previous_is_after_zwj {
                                        return self.get_current_position();
                                    }
                                    continue 'a;
                                } else if previous_is_after_zwj {
                                    continue 'a;
                                } else {
                                    return self.get_current_position();
                                }
                            }
                            BreakState::Break => {
                                if after_zwj {
                                    continue 'a;
                                } else {
                                    return self.get_current_position();
                                }
                            }
                            BreakState::Intermediate(i) => {
                                index = i;
                                previous_iter = self.iter.clone();
                                previous_pos_data = self.current_pos_data;
                                previous_is_after_zwj = after_zwj;
                            }
                            BreakState::Index(i) => {
                                index = i;
                                if previous_break_state_is_cp_prop {
                                    previous_iter = self.iter.clone();
                                    previous_pos_data = self.current_pos_data;
                                    previous_is_after_zwj = after_zwj;
                                }
                            }
                        }
                        left_prop_pre_lb9 = prop;
                    }
                }
            }
        }
    }
}

enum StringBoundaryPosType {
    Start,
    Middle,
    End,
}

impl<Y: LineBreakType> LineBreakIterator<'_, '_, Y> {
    fn advance_iter(&mut self) {
        self.current_pos_data = self.iter.next();
    }

    fn is_eof(&self) -> bool {
        self.current_pos_data.is_none()
    }

    #[inline]
    fn check_eof(&mut self) -> StringBoundaryPosType {
        if self.is_eof() {
            self.advance_iter();
            if self.is_eof() {
                if self.len == 0 {
                    self.len = 1;
                    StringBoundaryPosType::Start
                } else {
                    StringBoundaryPosType::End
                }
            } else {
                StringBoundaryPosType::Start
            }
        } else {
            StringBoundaryPosType::Middle
        }
    }

    fn get_current_position(&self) -> Option<usize> {
        self.current_pos_data.map(|(pos, _)| pos)
    }

    fn get_current_codepoint(&self) -> Option<Y::CharType> {
        self.current_pos_data.map(|(_, codepoint)| codepoint)
    }

    fn get_linebreak_property(&self, codepoint: Y::CharType) -> u8 {
        Y::get_linebreak_property_with_rule(self, codepoint)
    }

    fn get_current_linebreak_property(&self) -> Option<u8> {
        self.get_current_codepoint()
            .map(|c| self.get_linebreak_property(c))
    }

    fn is_break_by_normal(&self, codepoint: Y::CharType) -> bool {
        match codepoint.into() {
            0x301C | 0x30A0 => self.options.ja_zh,
            _ => false,
        }
    }
}

impl LineBreakType for Utf8 {
    fn get_linebreak_property_with_rule(iterator: &LineBreakIterator<Self>, c: char) -> u8 {
        iterator.data.get_linebreak_property_utf32_with_rule(
            c as u32,
            iterator.options.strictness,
            iterator.options.word_option,
        )
    }

    #[inline]
    fn use_complex_breaking(iterator: &LineBreakIterator<Self>, c: char) -> bool {
        iterator.data.use_complex_breaking_utf32(c as u32)
    }

    fn line_handle_complex_language(
        iter: &mut LineBreakIterator<'_, '_, Self>,
        left_codepoint: char,
    ) -> Option<usize> {
        line_handle_complex_language_utf8(iter, left_codepoint)
    }
}

impl LineBreakType for PotentiallyIllFormedUtf8 {
    fn get_linebreak_property_with_rule(iterator: &LineBreakIterator<Self>, c: char) -> u8 {
        iterator.data.get_linebreak_property_utf32_with_rule(
            c as u32,
            iterator.options.strictness,
            iterator.options.word_option,
        )
    }

    #[inline]
    fn use_complex_breaking(iterator: &LineBreakIterator<Self>, c: char) -> bool {
        iterator.data.use_complex_breaking_utf32(c as u32)
    }

    fn line_handle_complex_language(
        iter: &mut LineBreakIterator<'_, '_, Self>,
        left_codepoint: char,
    ) -> Option<usize> {
        line_handle_complex_language_utf8(iter, left_codepoint)
    }
}
/// line_handle_complex_language impl for UTF8 iterators
fn line_handle_complex_language_utf8<T>(
    iter: &mut LineBreakIterator<'_, '_, T>,
    left_codepoint: char,
) -> Option<usize>
where
    T: LineBreakType<CharType = char>,
{
    let start_iter = iter.iter.clone();
    let start_point = iter.current_pos_data;
    let mut s = String::new();
    s.push(left_codepoint);
    loop {
        debug_assert!(!iter.is_eof());
        s.push(iter.get_current_codepoint()?);
        iter.advance_iter();
        if let Some(current_codepoint) = iter.get_current_codepoint() {
            if !T::use_complex_breaking(iter, current_codepoint) {
                break;
            }
        } else {
            break;
        }
    }

    iter.iter = start_iter;
    iter.current_pos_data = start_point;
    let breaks = iter.complex.complex_language_segment_str(&s);
    iter.result_cache = breaks;
    let first_pos = *iter.result_cache.first()?;
    let mut i = left_codepoint.len_utf8();
    loop {
        if i == first_pos {
            iter.result_cache = iter.result_cache.iter().skip(1).map(|r| r - i).collect();
            return iter.get_current_position();
        }
        debug_assert!(
            i < first_pos,
            "we should always arrive at first_pos: near index {:?}",
            iter.get_current_position()
        );
        i += iter.get_current_codepoint().map_or(0, T::char_len);
        iter.advance_iter();
        if iter.is_eof() {
            iter.result_cache.clear();
            return Some(iter.len);
        }
    }
}

impl LineBreakType for Latin1 {
    fn get_linebreak_property_with_rule(iterator: &LineBreakIterator<Self>, c: u8) -> u8 {
        iterator.data.property_table.get32(c as u32)
    }

    #[inline]
    fn use_complex_breaking(_iterator: &LineBreakIterator<Self>, _c: u8) -> bool {
        false
    }

    fn line_handle_complex_language(
        _: &mut LineBreakIterator<Self>,
        _: Self::CharType,
    ) -> Option<usize> {
        unreachable!()
    }
}

impl LineBreakType for Utf16 {
    fn get_linebreak_property_with_rule(iterator: &LineBreakIterator<Self>, c: u32) -> u8 {
        iterator.data.get_linebreak_property_utf32_with_rule(
            c,
            iterator.options.strictness,
            iterator.options.word_option,
        )
    }

    #[inline]
    fn use_complex_breaking(iterator: &LineBreakIterator<Self>, c: u32) -> bool {
        iterator.data.use_complex_breaking_utf32(c)
    }

    fn line_handle_complex_language(
        iterator: &mut LineBreakIterator<Self>,
        left_codepoint: Self::CharType,
    ) -> Option<usize> {
        let start_iter = iterator.iter.clone();
        let start_point = iterator.current_pos_data;
        let mut s = vec![left_codepoint as u16];
        loop {
            debug_assert!(!iterator.is_eof());
            s.push(iterator.get_current_codepoint()? as u16);
            iterator.advance_iter();
            if let Some(current_codepoint) = iterator.get_current_codepoint() {
                if !Self::use_complex_breaking(iterator, current_codepoint) {
                    break;
                }
            } else {
                break;
            }
        }

        iterator.iter = start_iter;
        iterator.current_pos_data = start_point;
        let breaks = iterator.complex.complex_language_segment_utf16(&s);
        iterator.result_cache = breaks;
        let first_pos = *iterator.result_cache.first()?;
        let mut i = 1;
        loop {
            if i == first_pos {
                iterator.result_cache = iterator
                    .result_cache
                    .iter()
                    .skip(1)
                    .map(|r| r - i)
                    .collect();
                return iterator.get_current_position();
            }
            debug_assert!(
                i < first_pos,
                "we should always arrive at first_pos: near index {:?}",
                iterator.get_current_position()
            );
            i += 1;
            iterator.advance_iter();
            if iterator.is_eof() {
                iterator.result_cache.clear();
                return Some(iterator.len);
            }
        }
    }
}
