use smallvec::SmallVec;

use core::{fmt::Debug, iter, ops::Range};

/// Structure that keeps track of a I -> T mapping,
/// optimized for a case where keys of the same values
/// are often grouped together linearly.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct RangedStates<I, T> {
    /// List of ranges, each associated with a singe value.
    /// Ranges of keys have to be non-intersecting and ordered.
    ranges: SmallVec<[(Range<I>, T); 1]>,
}

impl<I: Copy + Ord, T: Copy + PartialEq> RangedStates<I, T> {
    pub fn from_range(range: Range<I>, value: T) -> Self {
        Self {
            ranges: iter::once((range, value)).collect(),
        }
    }

    /// Construct a new instance from a slice of ranges.

    pub fn iter(&self) -> impl Iterator<Item = &(Range<I>, T)> + Clone {
        self.ranges.iter()
    }

    pub fn iter_mut(&mut self) -> impl Iterator<Item = &mut (Range<I>, T)> {
        self.ranges.iter_mut()
    }

    /// Check that all the ranges are non-intersecting and ordered.
    /// Panics otherwise.

    /// Merge the neighboring ranges together, where possible.
    pub fn coalesce(&mut self) {
        let mut num_removed = 0;
        let mut iter = self.ranges.iter_mut();
        let mut cur = match iter.next() {
            Some(elem) => elem,
            None => return,
        };
        for next in iter {
            if cur.0.end == next.0.start && cur.1 == next.1 {
                num_removed += 1;
                cur.0.end = next.0.end;
                next.0.end = next.0.start;
            } else {
                cur = next;
            }
        }
        if num_removed != 0 {
            self.ranges.retain(|pair| pair.0.start != pair.0.end);
        }
    }

    pub fn iter_filter<'a>(
        &'a self,
        range: &'a Range<I>,
    ) -> impl Iterator<Item = (Range<I>, &'a T)> + 'a {
        self.ranges
            .iter()
            .filter(move |&(inner, ..)| inner.end > range.start && inner.start < range.end)
            .map(move |(inner, v)| {
                let new_range = inner.start.max(range.start)..inner.end.min(range.end);

                (new_range, v)
            })
    }

    /// Split the storage ranges in such a way that there is a linear subset of
    /// them occupying exactly `index` range, which is returned mutably.
    ///
    /// Gaps in the ranges are filled with `default` value.
    pub fn isolate(&mut self, index: &Range<I>, default: T) -> &mut [(Range<I>, T)] {
        let mut start_pos = match self.ranges.iter().position(|pair| pair.0.end > index.start) {
            Some(pos) => pos,
            None => {
                let pos = self.ranges.len();
                self.ranges.push((index.clone(), default));
                return &mut self.ranges[pos..];
            }
        };

        {
            let (range, value) = self.ranges[start_pos].clone();
            if range.start < index.start {
                self.ranges[start_pos].0.start = index.start;
                self.ranges
                    .insert(start_pos, (range.start..index.start, value));
                start_pos += 1;
            }
        }
        let mut pos = start_pos;
        let mut range_pos = index.start;
        loop {
            let (range, value) = self.ranges[pos].clone();
            if range.start >= index.end {
                self.ranges.insert(pos, (range_pos..index.end, default));
                pos += 1;
                break;
            }
            if range.start > range_pos {
                self.ranges.insert(pos, (range_pos..range.start, default));
                pos += 1;
                range_pos = range.start;
            }
            if range.end >= index.end {
                if range.end != index.end {
                    self.ranges[pos].0.start = index.end;
                    self.ranges.insert(pos, (range_pos..index.end, value));
                }
                pos += 1;
                break;
            }
            pos += 1;
            range_pos = range.end;
            if pos == self.ranges.len() {
                self.ranges.push((range_pos..index.end, default));
                pos += 1;
                break;
            }
        }

        &mut self.ranges[start_pos..pos]
    }

}
