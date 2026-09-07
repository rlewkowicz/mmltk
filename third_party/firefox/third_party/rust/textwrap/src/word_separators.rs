//! Functionality for finding words.
//!
//! In order to wrap text, we need to know where the legal break
//! points are, i.e., where the words of the text are. This means that
//! we need to define what a "word" is.
//!
//! A simple approach is to simply split the text on whitespace, but
//! this does not work for East-Asian languages such as Chinese or
//! Japanese where there are no spaces between words. Breaking a long
//! sequence of emojis is another example where line breaks might be
//! wanted even if there are no whitespace to be found.
//!
//! The [`WordSeparator`] enum is responsible for determining where
//! there words are in a line of text. Please refer to the enum and
//! its variants for more information.

#[cfg(feature = "unicode-linebreak")]
use crate::core::skip_ansi_escape_sequence;
use crate::core::Word;

/// Describes where words occur in a line of text.
///
/// The simplest approach is say that words are separated by one or
/// more ASCII spaces (`' '`). This works for Western languages
/// without emojis. A more complex approach is to use the Unicode line
/// breaking algorithm, which finds break points in non-ASCII text.
///
/// The line breaks occur between words, please see
/// [`WordSplitter`](crate::WordSplitter) for options of how to handle
/// hyphenation of individual words.
///
/// # Examples
///
/// ```
/// use textwrap::core::Word;
/// use textwrap::WordSeparator::AsciiSpace;
///
/// let words = AsciiSpace.find_words("Hello World!").collect::<Vec<_>>();
/// assert_eq!(words, vec![Word::from("Hello "), Word::from("World!")]);
/// ```
#[derive(Clone, Copy)]
pub enum WordSeparator {
    /// Find words by splitting on runs of `' '` characters.
    ///
    /// # Examples
    ///
    /// ```
    /// use textwrap::core::Word;
    /// use textwrap::WordSeparator::AsciiSpace;
    ///
    /// let words = AsciiSpace.find_words("Hello   World!").collect::<Vec<_>>();
    /// assert_eq!(words, vec![Word::from("Hello   "),
    ///                        Word::from("World!")]);
    /// ```
    AsciiSpace,

    /// Split `line` into words using Unicode break properties.
    ///
    /// This word separator uses the Unicode line breaking algorithm
    /// described in [Unicode Standard Annex
    /// #14](https://www.unicode.org/reports/tr14/) to find legal places
    /// to break lines. There is a small difference in that the U+002D
    /// (Hyphen-Minus) and U+00AD (Soft Hyphen) don’t create a line break:
    /// to allow a line break at a hyphen, use
    /// [`WordSplitter::HyphenSplitter`](crate::WordSplitter::HyphenSplitter).
    /// Soft hyphens are not currently supported.
    ///
    /// # Examples
    ///
    /// Unlike [`WordSeparator::AsciiSpace`], the Unicode line
    /// breaking algorithm will find line break opportunities between
    /// some characters with no intervening whitespace:
    ///
    /// ```
    /// #[cfg(feature = "unicode-linebreak")] {
    /// use textwrap::core::Word;
    /// use textwrap::WordSeparator::UnicodeBreakProperties;
    ///
    /// assert_eq!(UnicodeBreakProperties.find_words("Emojis: 😂😍").collect::<Vec<_>>(),
    ///            vec![Word::from("Emojis: "),
    ///                 Word::from("😂"),
    ///                 Word::from("😍")]);
    ///
    /// assert_eq!(UnicodeBreakProperties.find_words("CJK: 你好").collect::<Vec<_>>(),
    ///            vec![Word::from("CJK: "),
    ///                 Word::from("你"),
    ///                 Word::from("好")]);
    /// }
    /// ```
    ///
    /// A U+2060 (Word Joiner) character can be inserted if you want to
    /// manually override the defaults and keep the characters together:
    ///
    /// ```
    /// #[cfg(feature = "unicode-linebreak")] {
    /// use textwrap::core::Word;
    /// use textwrap::WordSeparator::UnicodeBreakProperties;
    ///
    /// assert_eq!(UnicodeBreakProperties.find_words("Emojis: 😂\u{2060}😍").collect::<Vec<_>>(),
    ///            vec![Word::from("Emojis: "),
    ///                 Word::from("😂\u{2060}😍")]);
    /// }
    /// ```
    ///
    /// The Unicode line breaking algorithm will also automatically
    /// suppress break breaks around certain punctuation characters::
    ///
    /// ```
    /// #[cfg(feature = "unicode-linebreak")] {
    /// use textwrap::core::Word;
    /// use textwrap::WordSeparator::UnicodeBreakProperties;
    ///
    /// assert_eq!(UnicodeBreakProperties.find_words("[ foo ] bar !").collect::<Vec<_>>(),
    ///            vec![Word::from("[ foo ] "),
    ///                 Word::from("bar !")]);
    /// }
    /// ```
    #[cfg(feature = "unicode-linebreak")]
    UnicodeBreakProperties,

    /// Find words using a custom word separator
    Custom(fn(line: &str) -> Box<dyn Iterator<Item = Word<'_>> + '_>),
}

impl PartialEq for WordSeparator {
    /// Compare two word separators.
    ///
    /// ```
    /// use textwrap::WordSeparator;
    ///
    /// assert_eq!(WordSeparator::AsciiSpace, WordSeparator::AsciiSpace);
    /// #[cfg(feature = "unicode-linebreak")] {
    ///     assert_eq!(WordSeparator::UnicodeBreakProperties,
    ///                WordSeparator::UnicodeBreakProperties);
    /// }
    /// ```
    ///
    /// Note that `WordSeparator::Custom` values never compare equal:
    ///
    /// ```
    /// use textwrap::WordSeparator;
    /// use textwrap::core::Word;
    /// fn word_separator(line: &str) -> Box<dyn Iterator<Item = Word<'_>> + '_> {
    ///     Box::new(line.split_inclusive(' ').map(Word::from))
    /// }
    /// assert_ne!(WordSeparator::Custom(word_separator),
    ///            WordSeparator::Custom(word_separator));
    /// ```
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (WordSeparator::AsciiSpace, WordSeparator::AsciiSpace) => true,
            #[cfg(feature = "unicode-linebreak")]
            (WordSeparator::UnicodeBreakProperties, WordSeparator::UnicodeBreakProperties) => true,
            (_, _) => false,
        }
    }
}

impl std::fmt::Debug for WordSeparator {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WordSeparator::AsciiSpace => f.write_str("AsciiSpace"),
            #[cfg(feature = "unicode-linebreak")]
            WordSeparator::UnicodeBreakProperties => f.write_str("UnicodeBreakProperties"),
            WordSeparator::Custom(_) => f.write_str("Custom(...)"),
        }
    }
}

impl WordSeparator {
    /// Create a new word separator.
    ///
    /// The best available algorithm is used by default, i.e.,
    /// [`WordSeparator::UnicodeBreakProperties`] if available,
    /// otherwise [`WordSeparator::AsciiSpace`].
    pub const fn new() -> Self {
        #[cfg(feature = "unicode-linebreak")]
        {
            WordSeparator::UnicodeBreakProperties
        }

        #[cfg(not(feature = "unicode-linebreak"))]
        {
            WordSeparator::AsciiSpace
        }
    }

    /// Find all words in `line`.
    pub fn find_words<'a>(&self, line: &'a str) -> Box<dyn Iterator<Item = Word<'a>> + 'a> {
        match self {
            WordSeparator::AsciiSpace => find_words_ascii_space(line),
            #[cfg(feature = "unicode-linebreak")]
            WordSeparator::UnicodeBreakProperties => find_words_unicode_break_properties(line),
            WordSeparator::Custom(func) => func(line),
        }
    }
}

fn find_words_ascii_space<'a>(line: &'a str) -> Box<dyn Iterator<Item = Word<'a>> + 'a> {
    let mut start = 0;
    let mut in_whitespace = false;
    let mut char_indices = line.char_indices();

    Box::new(std::iter::from_fn(move || {
        for (idx, ch) in char_indices.by_ref() {
            if in_whitespace && ch != ' ' {
                let word = Word::from(&line[start..idx]);
                start = idx;
                in_whitespace = ch == ' ';
                return Some(word);
            }

            in_whitespace = ch == ' ';
        }

        if start < line.len() {
            let word = Word::from(&line[start..]);
            start = line.len();
            return Some(word);
        }

        None
    }))
}

#[cfg(feature = "unicode-linebreak")]
fn strip_ansi_escape_sequences(text: &str) -> String {
    let mut result = String::with_capacity(text.len());

    let mut chars = text.chars();
    while let Some(ch) = chars.next() {
        if skip_ansi_escape_sequence(ch, &mut chars) {
            continue;
        }
        result.push(ch);
    }

    result
}

/// Soft hyphen, also knows as a “shy hyphen”. Should show up as ‘-’
/// if a line is broken at this point, and otherwise be invisible.
/// Textwrap does not currently support breaking words at soft
/// hyphens.
#[cfg(feature = "unicode-linebreak")]
const SHY: char = '\u{00ad}';

/// Find words in line. ANSI escape sequences are ignored in `line`.
#[cfg(feature = "unicode-linebreak")]
fn find_words_unicode_break_properties<'a>(
    line: &'a str,
) -> Box<dyn Iterator<Item = Word<'a>> + 'a> {
    let mut last_stripped_idx = 0;
    let mut char_indices = line.char_indices();
    let mut idx_map = std::iter::from_fn(move || match char_indices.next() {
        Some((orig_idx, ch)) => {
            let stripped_idx = last_stripped_idx;
            if !skip_ansi_escape_sequence(ch, &mut char_indices.by_ref().map(|(_, ch)| ch)) {
                last_stripped_idx += ch.len_utf8();
            }
            Some((orig_idx, stripped_idx))
        }
        None => None,
    });

    let stripped = strip_ansi_escape_sequences(line);
    let mut opportunities = unicode_linebreak::linebreaks(&stripped)
        .filter(|(idx, _)| {
            #[allow(clippy::match_like_matches_macro)]
            match &stripped[..*idx].chars().next_back() {
                Some('-') => false,
                Some(SHY) => false,
                _ => true,
            }
        })
        .collect::<Vec<_>>()
        .into_iter();

    opportunities.next_back();

    let mut start = 0;
    Box::new(std::iter::from_fn(move || {
        for (idx, _) in opportunities.by_ref() {
            if let Some((orig_idx, _)) = idx_map.find(|&(_, stripped_idx)| stripped_idx == idx) {
                let word = Word::from(&line[start..orig_idx]);
                start = orig_idx;
                return Some(word);
            }
        }

        if start < line.len() {
            let word = Word::from(&line[start..]);
            start = line.len();
            return Some(word);
        }

        None
    }))
}
