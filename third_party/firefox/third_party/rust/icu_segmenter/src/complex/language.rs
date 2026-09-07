// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

#[derive(PartialEq, Debug, Copy, Clone)]
pub(super) enum Language {
    Burmese,
    ChineseOrJapanese,
    Khmer,
    Lao,
    Thai,
    Unknown,
}

fn get_language(codepoint: u32) -> Language {
    match codepoint {
        0xe01..=0xe7f => Language::Thai,
        0x0E80..=0x0EFF => Language::Lao,
        0x1000..=0x109f => Language::Burmese,
        0x1780..=0x17FF => Language::Khmer,
        0x19E0..=0x19FF => Language::Khmer,
        0x2E80..=0x2EFF => Language::ChineseOrJapanese,
        0x2F00..=0x2FDF => Language::ChineseOrJapanese,
        0x3040..=0x30FF => Language::ChineseOrJapanese,
        0x31F0..=0x31FF => Language::ChineseOrJapanese,
        0x32D0..=0x32FE => Language::ChineseOrJapanese,
        0x3400..=0x4DBF => Language::ChineseOrJapanese,
        0x4E00..=0x9FFF => Language::ChineseOrJapanese,
        0xa9e0..=0xa9ff => Language::Burmese,
        0xaa60..=0xaa7f => Language::Burmese,
        0xF900..=0xFAFF => Language::ChineseOrJapanese,
        0xFF66..=0xFF9D => Language::ChineseOrJapanese,
        0x16FE2..=0x16FE3 => Language::ChineseOrJapanese,
        0x16FF0..=0x16FF1 => Language::ChineseOrJapanese,
        0x1AFF0..=0x1B16F => Language::ChineseOrJapanese,
        0x1F200 => Language::ChineseOrJapanese,
        0x20000..=0x2FA1F => Language::ChineseOrJapanese,
        0x30000..=0x3134F => Language::ChineseOrJapanese,
        _ => Language::Unknown,
    }
}

/// This struct is an iterator that returns the string per language from the
/// given string.
pub(super) struct LanguageIterator<'s> {
    rest: &'s str,
}

impl<'s> LanguageIterator<'s> {
    pub(super) fn new(input: &'s str) -> Self {
        Self { rest: input }
    }
}

impl<'s> Iterator for LanguageIterator<'s> {
    type Item = (&'s str, Language);

    fn next(&mut self) -> Option<Self::Item> {
        let mut indices = self.rest.char_indices();
        let lang = get_language(indices.next()?.1 as u32);
        match indices.find(|&(_, ch)| get_language(ch as u32) != lang) {
            Some((i, _)) => {
                let (result, rest) = self.rest.split_at(i);
                self.rest = rest;
                Some((result, lang))
            }
            None => Some((core::mem::take(&mut self.rest), lang)),
        }
    }
}

pub(super) struct LanguageIteratorUtf16<'s> {
    rest: &'s [u16],
}

impl<'s> LanguageIteratorUtf16<'s> {
    pub(super) fn new(input: &'s [u16]) -> Self {
        Self { rest: input }
    }
}

impl<'s> Iterator for LanguageIteratorUtf16<'s> {
    type Item = (&'s [u16], Language);

    fn next(&mut self) -> Option<Self::Item> {
        let lang = get_language(*self.rest.first()? as u32);
        match self
            .rest
            .iter()
            .position(|&ch| get_language(ch as u32) != lang)
        {
            Some(i) => {
                let (result, rest) = self.rest.split_at(i);
                self.rest = rest;
                Some((result, lang))
            }
            None => Some((core::mem::take(&mut self.rest), lang)),
        }
    }
}
