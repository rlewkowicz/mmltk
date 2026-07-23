// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::grapheme::*;
use crate::indices::Utf16Indices;
use crate::provider::*;
use crate::scaffold::{Utf16, Utf8};
use core::str::CharIndices;
use icu_collections::char16trie::{Char16Trie, TrieResult};

/// A trait for dictionary based iterator
trait DictionaryType {
    /// The iterator over characters.
    type IterAttr<'s>: Iterator<Item = (usize, Self::CharType)> + Clone;

    /// The character type.
    type CharType: Copy + Into<u32>;

    fn to_char(c: Self::CharType) -> char;
    fn char_len(c: Self::CharType) -> usize;
}

struct DictionaryBreakIterator<
    'l,
    's,
    Y: DictionaryType + ?Sized,
    X: Iterator<Item = usize> + ?Sized,
> {
    trie: Char16Trie<'l>,
    iter: Y::IterAttr<'s>,
    len: usize,
    grapheme_iter: X,
}

/// Implement the [`Iterator`] trait over the segmenter break opportunities of the given string.
/// Please see the [module-level documentation](crate) for its usages.
///
/// Lifetimes:
/// - `'l` = lifetime of the segmenter object from which this iterator was created
/// - `'s` = lifetime of the string being segmented
///
/// [`Iterator`]: core::iter::Iterator
impl<Y: DictionaryType + ?Sized, X: Iterator<Item = usize> + ?Sized> Iterator
    for DictionaryBreakIterator<'_, '_, Y, X>
{
    type Item = usize;

    fn next(&mut self) -> Option<Self::Item> {
        let mut trie_iter = self.trie.iter();
        let mut intermediate_length = 0;
        let mut not_match = false;
        let mut previous_match = None;
        let mut last_grapheme_offset = 0;

        while let Some(next) = self.iter.next() {
            let ch = Y::to_char(next.1);
            match trie_iter.next(ch) {
                TrieResult::FinalValue(_) => {
                    return Some(next.0 + Y::char_len(next.1));
                }
                TrieResult::Intermediate(_) => {
                    while last_grapheme_offset < next.0 + Y::char_len(next.1) {
                        if let Some(offset) = self.grapheme_iter.next() {
                            last_grapheme_offset = offset;
                            continue;
                        }
                        last_grapheme_offset = self.len;
                        break;
                    }
                    if last_grapheme_offset != next.0 + Y::char_len(next.1) {
                        continue;
                    }

                    intermediate_length = next.0 + Y::char_len(next.1);
                    previous_match = Some(self.iter.clone());
                }
                TrieResult::NoMatch => {
                    if intermediate_length > 0 {
                        if let Some(previous_match) = previous_match {
                            self.iter = previous_match;
                        }
                        return Some(intermediate_length);
                    }
                    return Some(next.0 + Y::char_len(next.1));
                }
                TrieResult::NoValue => {
                    not_match = true;
                }
            }
        }

        if intermediate_length > 0 {
            Some(intermediate_length)
        } else if not_match {
            Some(self.len)
        } else {
            None
        }
    }
}

impl DictionaryType for u32 {
    type IterAttr<'s> = Utf16Indices<'s>;
    type CharType = u32;

    fn to_char(c: u32) -> char {
        char::from_u32(c).unwrap_or(char::REPLACEMENT_CHARACTER)
    }

    fn char_len(c: u32) -> usize {
        if c >= 0x10000 {
            2
        } else {
            1
        }
    }
}

impl DictionaryType for char {
    type IterAttr<'s> = CharIndices<'s>;
    type CharType = char;

    fn to_char(c: char) -> char {
        c
    }

    fn char_len(c: char) -> usize {
        c.len_utf8()
    }
}

pub(super) struct DictionarySegmenter<'l> {
    dict: &'l UCharDictionaryBreakData<'l>,
    grapheme: GraphemeClusterSegmenterBorrowed<'l>,
}

impl<'l> DictionarySegmenter<'l> {
    pub(super) fn new(
        dict: &'l UCharDictionaryBreakData<'l>,
        grapheme: GraphemeClusterSegmenterBorrowed<'l>,
    ) -> Self {
        Self { dict, grapheme }
    }

    /// Create a dictionary based break iterator for an `str` (a UTF-8 string).
    pub(super) fn segment_str(&'l self, input: &'l str) -> impl Iterator<Item = usize> + 'l {
        let grapheme_iter = self.grapheme.segment_str(input);
        DictionaryBreakIterator::<char, GraphemeClusterBreakIterator<Utf8>> {
            trie: Char16Trie::new(self.dict.trie_data.clone()),
            iter: input.char_indices(),
            len: input.len(),
            grapheme_iter,
        }
    }

    /// Create a dictionary based break iterator for a UTF-16 string.
    pub(super) fn segment_utf16(&'l self, input: &'l [u16]) -> impl Iterator<Item = usize> + 'l {
        let grapheme_iter = self.grapheme.segment_utf16(input);
        DictionaryBreakIterator::<u32, GraphemeClusterBreakIterator<Utf16>> {
            trie: Char16Trie::new(self.dict.trie_data.clone()),
            iter: Utf16Indices::new(input),
            len: input.len(),
            grapheme_iter,
        }
    }
}
