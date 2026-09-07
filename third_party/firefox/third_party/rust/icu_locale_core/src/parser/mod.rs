// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

pub mod errors;
mod langid;
mod locale;

pub use errors::ParseError;
pub use langid::*;

pub use locale::*;

const fn skip_before_separator(slice: &[u8]) -> &[u8] {
    let mut end = 0;

    #[expect(clippy::indexing_slicing)] 
    while end < slice.len() && !matches!(slice[end], b'-') {
        end += 1;
    }

    unsafe { slice.split_at_unchecked(end).0 }
}

#[derive(Copy, Clone, Debug)]
pub struct SubtagIterator<'a> {
    remaining: &'a [u8],
    current: Option<&'a [u8]>,
}

impl<'a> SubtagIterator<'a> {
    pub const fn new(rest: &'a [u8]) -> Self {
        Self {
            remaining: rest,
            current: Some(skip_before_separator(rest)),
        }
    }

    pub const fn next_const(mut self) -> (Self, Option<&'a [u8]>) {
        let Some(result) = self.current else {
            return (self, None);
        };

        self.current = if result.len() < self.remaining.len() {
            self.remaining = unsafe { self.remaining.split_at_unchecked(result.len() + 1).1 };
            Some(skip_before_separator(self.remaining))
        } else {
            None
        };
        (self, Some(result))
    }

    pub const fn peek(&self) -> Option<&'a [u8]> {
        self.current
    }
}

impl<'a> Iterator for SubtagIterator<'a> {
    type Item = &'a [u8];

    fn next(&mut self) -> Option<Self::Item> {
        let (s, res) = self.next_const();
        *self = s;
        res
    }
}
