use std::{
    fmt::Debug,
    iter::Sum,
    ops::{Add, AddAssign, Range, Sub},
};

#[derive(Debug)]
pub struct RangeAllocator<T> {
    /// The range this allocator covers.
    initial_range: Range<T>,
    /// A Vec of ranges in this heap which are unused.
    /// Must be ordered with ascending range start to permit short circuiting allocation.
    /// No two ranges in this vec may overlap.
    free_ranges: Vec<Range<T>>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct RangeAllocationError<T> {
    pub fragmented_free_length: T,
}

impl<T> RangeAllocator<T>
where
    T: Clone + Copy + Add<Output = T> + AddAssign + Sub<Output = T> + Eq + PartialOrd + Debug,
{
    pub fn new(range: Range<T>) -> Self {
        RangeAllocator {
            initial_range: range.clone(),
            free_ranges: vec![range],
        }
    }

    pub fn initial_range(&self) -> &Range<T> {
        &self.initial_range
    }

    pub fn grow_to(&mut self, new_end: T) {
        if let Some(last_range) = self.free_ranges.last_mut() {
            last_range.end = new_end;
        } else {
            self.free_ranges.push(self.initial_range.end..new_end);
        }

        self.initial_range.end = new_end;
    }

    pub fn allocate_range(&mut self, length: T) -> Result<Range<T>, RangeAllocationError<T>> {
        assert_ne!(length + length, length);
        let mut best_fit: Option<(usize, Range<T>)> = None;

        #[allow(clippy::eq_op)]
        let mut fragmented_free_length = length - length;
        for (index, range) in self.free_ranges.iter().cloned().enumerate() {
            let range_length = range.end - range.start;
            fragmented_free_length += range_length;
            if range_length < length {
                continue;
            } else if range_length == length {
                best_fit = Some((index, range));
                break;
            }
            best_fit = Some(match best_fit {
                Some((best_index, best_range)) => {
                    if range_length < best_range.end - best_range.start {
                        (index, range)
                    } else {
                        (best_index, best_range.clone())
                    }
                }
                None => (index, range),
            });
        }
        match best_fit {
            Some((index, range)) => {
                if range.end - range.start == length {
                    self.free_ranges.remove(index);
                } else {
                    self.free_ranges[index].start += length;
                }
                Ok(range.start..(range.start + length))
            }
            None => Err(RangeAllocationError {
                fragmented_free_length,
            }),
        }
    }

    pub fn free_range(&mut self, range: Range<T>) {
        assert!(self.initial_range.start <= range.start && range.end <= self.initial_range.end);
        assert!(range.start < range.end);

        let i = self
            .free_ranges
            .iter()
            .position(|r| r.start > range.start)
            .unwrap_or(self.free_ranges.len());

        if i > 0 && range.start == self.free_ranges[i - 1].end {
            self.free_ranges[i - 1].end =
                if i < self.free_ranges.len() && range.end == self.free_ranges[i].start {
                    let right = self.free_ranges.remove(i);
                    right.end
                } else {
                    range.end
                };

            return;
        } else if i < self.free_ranges.len() && range.end == self.free_ranges[i].start {
            self.free_ranges[i].start = if i > 0 && range.start == self.free_ranges[i - 1].end {
                let left = self.free_ranges.remove(i - 1);
                left.start
            } else {
                range.start
            };

            return;
        }

        assert!(
            (i == 0 || self.free_ranges[i - 1].end < range.start)
                && (i >= self.free_ranges.len() || range.end < self.free_ranges[i].start)
        );

        self.free_ranges.insert(i, range);
    }

    /// Returns an iterator over allocated non-empty ranges
    pub fn allocated_ranges(&self) -> impl Iterator<Item = Range<T>> + '_ {
        let first = match self.free_ranges.first() {
            Some(Range { ref start, .. }) if *start > self.initial_range.start => {
                Some(self.initial_range.start..*start)
            }
            None => Some(self.initial_range.clone()),
            _ => None,
        };

        let last = match self.free_ranges.last() {
            Some(Range { end, .. }) if *end < self.initial_range.end => {
                Some(*end..self.initial_range.end)
            }
            _ => None,
        };

        let mid = self
            .free_ranges
            .iter()
            .zip(self.free_ranges.iter().skip(1))
            .map(|(ra, rb)| ra.end..rb.start);

        first.into_iter().chain(mid).chain(last)
    }

    pub fn reset(&mut self) {
        self.free_ranges.clear();
        self.free_ranges.push(self.initial_range.clone());
    }

    pub fn is_empty(&self) -> bool {
        self.free_ranges.len() == 1 && self.free_ranges[0] == self.initial_range
    }
}

impl<T: Copy + Sub<Output = T> + Sum> RangeAllocator<T> {
    pub fn total_available(&self) -> T {
        self.free_ranges
            .iter()
            .map(|range| range.end - range.start)
            .sum()
    }
}
