/*! Lazy initialization of texture and buffer memory.

The WebGPU specification requires all texture & buffer memory to be
zero initialized on first read. To avoid unnecessary inits, we track
the initialization status of every resource and perform inits lazily.

The granularity is different for buffers and textures:

- Buffer: Byte granularity to support usecases with large, partially
  bound buffers well.

- Texture: Mip-level per layer. That is, a 2D surface is either
  completely initialized or not, subrects are not tracked.

Every use of a buffer/texture generates a InitTrackerAction which are
recorded and later resolved at queue submit by merging them with the
current state and each other in execution order.

It is important to note that from the point of view of the memory init
system there are two kind of writes:

- **Full writes**: Any kind of memcpy operation. These cause a
  `MemoryInitKind.ImplicitlyInitialized` action.

- **(Potentially) partial writes**: For example, write use in a
  Shader. The system is not able to determine if a resource is fully
  initialized afterwards but is no longer allowed to perform any
  clears, therefore this leads to a
  `MemoryInitKind.NeedsInitializedMemory` action, exactly like a read
  would.

 */

use core::{fmt, iter, ops::Range};

use smallvec::SmallVec;

mod buffer;
mod texture;

pub(crate) use buffer::{BufferInitTracker, BufferInitTrackerAction};
pub(crate) use texture::{
    has_copy_partial_init_tracker_coverage, TextureInitRange, TextureInitTracker,
    TextureInitTrackerAction,
};

#[derive(Debug, Clone, Copy)]
pub(crate) enum MemoryInitKind {
    ImplicitlyInitialized,
    NeedsInitializedMemory,
}

type UninitializedRangeVec<Idx> = SmallVec<[Range<Idx>; 1]>;

/// Tracks initialization status of a linear range from 0..size
#[derive(Debug, Clone)]
pub(crate) struct InitTracker<Idx: Ord + Copy + Default> {
    /// Non-overlapping list of all uninitialized ranges, sorted by
    /// range end.
    uninitialized_ranges: UninitializedRangeVec<Idx>,
}

pub(crate) struct UninitializedIter<'a, Idx: fmt::Debug + Ord + Copy> {
    uninitialized_ranges: &'a UninitializedRangeVec<Idx>,
    drain_range: Range<Idx>,
    next_index: usize,
}

impl<'a, Idx> Iterator for UninitializedIter<'a, Idx>
where
    Idx: fmt::Debug + Ord + Copy,
{
    type Item = Range<Idx>;

    fn next(&mut self) -> Option<Self::Item> {
        self.uninitialized_ranges
            .get(self.next_index)
            .and_then(|range| {
                if range.start < self.drain_range.end {
                    self.next_index += 1;
                    Some(
                        range.start.max(self.drain_range.start)
                            ..range.end.min(self.drain_range.end),
                    )
                } else {
                    None
                }
            })
    }
}

pub(crate) struct InitTrackerDrain<'a, Idx: fmt::Debug + Ord + Copy> {
    uninitialized_ranges: &'a mut UninitializedRangeVec<Idx>,
    drain_range: Range<Idx>,
    first_index: usize,
    next_index: usize,
}

impl<'a, Idx> Iterator for InitTrackerDrain<'a, Idx>
where
    Idx: fmt::Debug + Ord + Copy,
{
    type Item = Range<Idx>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(r) = self
            .uninitialized_ranges
            .get(self.next_index)
            .and_then(|range| {
                if range.start < self.drain_range.end {
                    Some(range.clone())
                } else {
                    None
                }
            })
        {
            self.next_index += 1;
            Some(r.start.max(self.drain_range.start)..r.end.min(self.drain_range.end))
        } else {
            let num_affected = self.next_index - self.first_index;
            if num_affected == 0 {
                return None;
            }
            let first_range = &mut self.uninitialized_ranges[self.first_index];

            if num_affected == 1
                && first_range.start < self.drain_range.start
                && first_range.end > self.drain_range.end
            {
                let old_start = first_range.start;
                first_range.start = self.drain_range.end;
                self.uninitialized_ranges
                    .insert(self.first_index, old_start..self.drain_range.start);
            }
            else {
                let remove_start = if first_range.start >= self.drain_range.start {
                    self.first_index
                } else {
                    first_range.end = self.drain_range.start;
                    self.first_index + 1
                };

                let last_range = &mut self.uninitialized_ranges[self.next_index - 1];
                let remove_end = if last_range.end <= self.drain_range.end {
                    self.next_index
                } else {
                    last_range.start = self.drain_range.end;
                    self.next_index - 1
                };

                self.uninitialized_ranges.drain(remove_start..remove_end);
            }

            None
        }
    }
}

impl<'a, Idx> Drop for InitTrackerDrain<'a, Idx>
where
    Idx: fmt::Debug + Ord + Copy,
{
    fn drop(&mut self) {
        if self.next_index <= self.first_index {
            for _ in self {}
        }
    }
}

impl<Idx> InitTracker<Idx>
where
    Idx: fmt::Debug + Ord + Copy + Default,
{
    pub(crate) fn new(size: Idx) -> Self {
        Self {
            uninitialized_ranges: iter::once(Idx::default()..size).collect(),
        }
    }

    /// Checks for uninitialized ranges within a given query range.
    ///
    /// If `query_range` includes any uninitialized portions of this init
    /// tracker's resource, return the smallest subrange of `query_range` that
    /// covers all uninitialized regions.
    ///
    /// The returned range may be larger than necessary, to keep this function
    /// O(log n).
    pub(crate) fn check(&self, query_range: Range<Idx>) -> Option<Range<Idx>> {
        let index = self
            .uninitialized_ranges
            .partition_point(|r| r.end <= query_range.start);
        self.uninitialized_ranges
            .get(index)
            .and_then(|start_range| {
                if start_range.start < query_range.end {
                    let start = start_range.start.max(query_range.start);
                    match self.uninitialized_ranges.get(index + 1) {
                        Some(next_range) => {
                            if next_range.start < query_range.end {
                                Some(start..query_range.end)
                            } else {
                                Some(start..start_range.end.min(query_range.end))
                            }
                        }
                        None => Some(start..start_range.end.min(query_range.end)),
                    }
                } else {
                    None
                }
            })
    }

    pub(crate) fn uninitialized(&mut self, drain_range: Range<Idx>) -> UninitializedIter<'_, Idx> {
        let index = self
            .uninitialized_ranges
            .partition_point(|r| r.end <= drain_range.start);
        UninitializedIter {
            drain_range,
            uninitialized_ranges: &self.uninitialized_ranges,
            next_index: index,
        }
    }

    pub(crate) fn drain(&mut self, drain_range: Range<Idx>) -> InitTrackerDrain<'_, Idx> {
        let index = self
            .uninitialized_ranges
            .partition_point(|r| r.end <= drain_range.start);
        InitTrackerDrain {
            drain_range,
            uninitialized_ranges: &mut self.uninitialized_ranges,
            first_index: index,
            next_index: index,
        }
    }
}

impl InitTracker<u32> {
    pub(crate) fn discard(&mut self, pos: u32) {
        let r_idx = self.uninitialized_ranges.partition_point(|r| r.end < pos);
        if let Some(r) = self.uninitialized_ranges.get(r_idx) {
            if r.end == pos {
                if let Some(right) = self.uninitialized_ranges.get(r_idx + 1) {
                    if right.start == pos + 1 {
                        self.uninitialized_ranges[r_idx] = r.start..right.end;
                        self.uninitialized_ranges.remove(r_idx + 1);
                        return;
                    }
                }
                self.uninitialized_ranges[r_idx] = r.start..(pos + 1);
            } else if r.start > pos {
                if r.start == pos + 1 {
                    self.uninitialized_ranges[r_idx] = pos..r.end;
                } else {
                    self.uninitialized_ranges.push(pos..(pos + 1));
                }
            }
        } else {
            self.uninitialized_ranges.push(pos..(pos + 1));
        }
    }
}
