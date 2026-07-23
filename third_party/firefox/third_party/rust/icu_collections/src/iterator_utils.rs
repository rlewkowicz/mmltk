// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::codepointtrie::CodePointMapRange;

/// This is an iterator that coalesces adjacent ranges in an iterator over code
/// point ranges
pub(crate) struct RangeListIteratorCoalescer<I, T> {
    iter: I,
    peek: Option<CodePointMapRange<T>>,
}

impl<I, T: Eq> RangeListIteratorCoalescer<I, T>
where
    I: Iterator<Item = CodePointMapRange<T>>,
{
    pub fn new(iter: I) -> Self {
        Self { iter, peek: None }
    }
}

impl<I, T: Eq> Iterator for RangeListIteratorCoalescer<I, T>
where
    I: Iterator<Item = CodePointMapRange<T>>,
{
    type Item = CodePointMapRange<T>;

    fn next(&mut self) -> Option<Self::Item> {
        let mut ret = if let Some(peek) = self.peek.take() {
            peek
        } else if let Some(next) = self.iter.next() {
            next
        } else {
            return None;
        };

        #[expect(clippy::while_let_on_iterator)]
        while let Some(next) = self.iter.next() {
            if *next.range.start() == ret.range.end() + 1 && next.value == ret.value {
                ret.range = *ret.range.start()..=*next.range.end();
            } else {
                self.peek = Some(next);
                return Some(ret);
            }
        }

        Some(ret)
    }
}
