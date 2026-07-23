// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
use num_traits::{CheckedAdd, CheckedSub, PrimInt, Zero};
use std::ops::{Add, Neg, Sub};

use super::*;

/// A zero-overhead wrapper around integer types for the sake of always
/// requiring checked arithmetic
#[repr(transparent)]
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct CheckedInteger<T>(pub T);

impl<T> From<T> for CheckedInteger<T> {
    fn from(i: T) -> Self {
        Self(i)
    }
}

impl From<CheckedInteger<i64>> for i64 {
    fn from(checked: CheckedInteger<i64>) -> i64 {
        checked.0
    }
}

impl<T, U: Into<T>> Add<U> for CheckedInteger<T>
where
    T: CheckedAdd,
{
    type Output = Option<Self>;

    fn add(self, other: U) -> Self::Output {
        self.0.checked_add(&other.into()).map(Into::into)
    }
}

impl<T, U: Into<T>> Sub<U> for CheckedInteger<T>
where
    T: CheckedSub,
{
    type Output = Option<Self>;

    fn sub(self, other: U) -> Self::Output {
        self.0.checked_sub(&other.into()).map(Into::into)
    }
}

/// Implement subtraction of checked `u64`s returning i64
impl Sub for CheckedInteger<u64> {
    type Output = Option<CheckedInteger<i64>>;

    fn sub(self, other: Self) -> Self::Output {
        if self >= other {
            self.0
                .checked_sub(other.0)
                .and_then(|u| i64::try_from(u).ok())
                .map(CheckedInteger)
        } else {
            other
                .0
                .checked_sub(self.0)
                .and_then(|u| i64::try_from(u).ok())
                .map(i64::neg)
                .map(CheckedInteger)
        }
    }
}


impl<T: std::cmp::PartialEq> PartialEq<T> for CheckedInteger<T> {
    fn eq(&self, other: &T) -> bool {
        self.0 == *other
    }
}

/// Provides the following information about a sample in the source file:
/// sample data offset (start and end), composition time in microseconds
/// (start and end) and whether it is a sync sample
#[repr(C)]
#[derive(Default, Debug, PartialEq, Eq)]
pub struct Indice {
    /// The byte offset in the file where the indexed sample begins.
    pub start_offset: CheckedInteger<u64>,
    /// The byte offset in the file where the indexed sample ends. This is
    /// equivalent to `start_offset` + the length in bytes of the indexed
    /// sample. Typically this will be the `start_offset` of the next sample
    /// in the file.
    pub end_offset: CheckedInteger<u64>,
    /// The time in ticks when the indexed sample should be displayed.
    /// Analogous to the concept of presentation time stamp (pts).
    pub start_composition: CheckedInteger<i64>,
    /// The time in ticks when the indexed sample should stop being
    /// displayed. Typically this would be the `start_composition` time of the
    /// next sample if samples were ordered by composition time.
    pub end_composition: CheckedInteger<i64>,
    /// The time in ticks that the indexed sample should be decoded at.
    /// Analogous to the concept of decode time stamp (dts).
    pub start_decode: CheckedInteger<i64>,
    /// Set if the indexed sample is a sync sample. The meaning of sync is
    /// somewhat codec specific, but essentially amounts to if the sample is a
    /// key frame.
    pub sync: bool,
}

/// Create a vector of `Indice`s with the information about track samples.
/// It uses `stsc`, `stco`, `stsz` and `stts` boxes to construct a list of
/// every sample in the file and provides offsets which can be used to read
/// raw sample data from the file.
#[allow(clippy::reversed_empty_ranges)]
pub fn create_sample_table(
    track: &Track,
    track_offset_time: CheckedInteger<i64>,
) -> Option<TryVec<Indice>> {
    let (stsc, stco, stsz, stts) = match (&track.stsc, &track.stco, &track.stsz, &track.stts) {
        (Some(a), Some(b), Some(c), Some(d)) => (a, b, c, d),
        _ => return None,
    };

    let has_sync_table = track.stss.is_some();

    let mut sample_size_iter = stsz.sample_sizes.iter();


    let total_sample_count = sample_to_chunk_iter(&stsc.samples, &stco.offsets)
        .map(|(_, sample_counts)| sample_counts.to_usize())
        .try_fold(0usize, usize::checked_add)?;
    let mut sample_table = TryVec::with_capacity(total_sample_count).ok()?;

    for i in sample_to_chunk_iter(&stsc.samples, &stco.offsets) {
        let chunk_id = i.0 as usize;
        let sample_counts = i.1;
        let mut cur_position = match stco.offsets.get(chunk_id) {
            Some(&i) => i.into(),
            _ => return None,
        };
        for _ in 0..sample_counts {
            let start_offset = cur_position;
            let end_offset = match (stsz.sample_size, sample_size_iter.next()) {
                (_, Some(t)) => (start_offset + *t)?,
                (t, _) if t > 0 => (start_offset + t)?,
                _ => 0.into(),
            };
            if end_offset == 0 {
                return None;
            }
            cur_position = end_offset;

            sample_table
                .push(Indice {
                    start_offset,
                    end_offset,
                    sync: !has_sync_table,
                    ..Default::default()
                })
                .ok()?;
        }
    }

    if let Some(ref v) = track.stss {
        for iter in &v.samples {
            match iter
                .checked_sub(&1)
                .and_then(|idx| sample_table.get_mut(idx as usize))
            {
                Some(elem) => elem.sync = true,
                _ => return None,
            }
        }
    }

    let ctts_iter = track.ctts.as_ref().map(|v| v.samples.as_slice().iter());

    let mut ctts_offset_iter = TimeOffsetIterator {
        cur_sample_range: (0..0),
        cur_offset: 0,
        ctts_iter,
        track_id: track.id,
    };

    let mut stts_iter = TimeToSampleIterator {
        cur_sample_count: (0..0),
        cur_sample_delta: 0,
        stts_iter: stts.samples.as_slice().iter(),
        track_id: track.id,
    };

    let mut sum_delta = TrackScaledTime::<i64>(0, track.id);
    for sample in sample_table.as_mut_slice() {
        let decode_time = sum_delta;
        sum_delta = (sum_delta + stts_iter.next_delta())?;

        let ctts_offset = ctts_offset_iter.next_offset_time();

        let start_composition = decode_time + ctts_offset;

        let end_composition = sum_delta + ctts_offset;

        let start_decode = decode_time;

        let start_composition_val: i64 = start_composition?.0;
        let end_composition_val: i64 = end_composition?.0;

        let track_offset: i64 = track_offset_time.0;

        sample.start_composition = CheckedInteger(track_offset.checked_add(start_composition_val)?);
        sample.end_composition = CheckedInteger(track_offset.checked_add(end_composition_val)?);
        sample.start_decode = CheckedInteger(start_decode.0);
    }

    if !sample_table.is_empty() {
        let mut sort_table = TryVec::with_capacity(sample_table.len()).ok()?;

        for i in 0..sample_table.len() {
            sort_table.push(i).ok()?;
        }

        sort_table.sort_by_key(|i| match sample_table.get(*i) {
            Some(v) => v.start_composition,
            _ => 0.into(),
        });

        for indices in sort_table.windows(2) {
            if let [current_index, peek_index] = *indices {
                let next_start_composition_time = sample_table[peek_index].start_composition;
                let sample = &mut sample_table[current_index];
                sample.end_composition = next_start_composition_time;
            }
        }
    }

    Some(sample_table)
}

struct TimeOffsetIterator<'a> {
    cur_sample_range: std::ops::Range<u32>,
    cur_offset: i64,
    ctts_iter: Option<std::slice::Iter<'a, TimeOffset>>,
    track_id: usize,
}

impl Iterator for TimeOffsetIterator<'_> {
    type Item = i64;

    #[allow(clippy::reversed_empty_ranges)]
    fn next(&mut self) -> Option<i64> {
        let has_sample = self.cur_sample_range.next().or_else(|| {
            let iter = match self.ctts_iter {
                Some(ref mut v) => v,
                _ => return None,
            };
            let offset_version;
            self.cur_sample_range = match iter.next() {
                Some(v) => {
                    offset_version = v.time_offset;
                    0..v.sample_count
                }
                _ => {
                    offset_version = TimeOffsetVersion::Version0(0);
                    0..0
                }
            };

            self.cur_offset = match offset_version {
                TimeOffsetVersion::Version0(i) => i64::from(i),
                TimeOffsetVersion::Version1(i) => i64::from(i),
            };

            self.cur_sample_range.next()
        });

        has_sample.and(Some(self.cur_offset))
    }
}

impl TimeOffsetIterator<'_> {
    fn next_offset_time(&mut self) -> TrackScaledTime<i64> {
        match self.next() {
            Some(v) => TrackScaledTime::<i64>(v, self.track_id),
            _ => TrackScaledTime::<i64>(0, self.track_id),
        }
    }
}

struct TimeToSampleIterator<'a> {
    cur_sample_count: std::ops::Range<u32>,
    cur_sample_delta: u32,
    stts_iter: std::slice::Iter<'a, Sample>,
    track_id: usize,
}

impl Iterator for TimeToSampleIterator<'_> {
    type Item = u32;

    #[allow(clippy::reversed_empty_ranges)]
    fn next(&mut self) -> Option<u32> {
        let has_sample = self.cur_sample_count.next().or_else(|| {
            self.cur_sample_count = match self.stts_iter.next() {
                Some(v) => {
                    self.cur_sample_delta = v.sample_delta;
                    0..v.sample_count
                }
                _ => 0..0,
            };

            self.cur_sample_count.next()
        });

        has_sample.and(Some(self.cur_sample_delta))
    }
}

impl TimeToSampleIterator<'_> {
    fn next_delta(&mut self) -> TrackScaledTime<i64> {
        match self.next() {
            Some(v) => TrackScaledTime::<i64>(i64::from(v), self.track_id),
            _ => TrackScaledTime::<i64>(0, self.track_id),
        }
    }
}

fn sample_to_chunk_iter<'a>(
    stsc_samples: &'a TryVec<SampleToChunk>,
    stco_offsets: &'a TryVec<u64>,
) -> SampleToChunkIterator<'a> {
    SampleToChunkIterator {
        chunks: (0..0),
        sample_count: 0,
        stsc_peek_iter: stsc_samples.as_slice().iter().peekable(),
        remain_chunk_count: stco_offsets
            .len()
            .try_into()
            .expect("stco.entry_count is u32"),
    }
}

struct SampleToChunkIterator<'a> {
    chunks: std::ops::Range<u32>,
    sample_count: u32,
    stsc_peek_iter: std::iter::Peekable<std::slice::Iter<'a, SampleToChunk>>,
    remain_chunk_count: u32, 
}

impl Iterator for SampleToChunkIterator<'_> {
    type Item = (u32, u32);

    fn next(&mut self) -> Option<(u32, u32)> {
        let has_chunk = self.chunks.next().or_else(|| {
            self.chunks = self.locate();
            self.remain_chunk_count
                .checked_sub(
                    self.chunks
                        .len()
                        .try_into()
                        .expect("len() of a Range<u32> must fit in u32"),
                )
                .and_then(|res| {
                    self.remain_chunk_count = res;
                    self.chunks.next()
                })
        });

        has_chunk.map(|id| (id, self.sample_count))
    }
}

impl SampleToChunkIterator<'_> {
    #[allow(clippy::reversed_empty_ranges)]
    fn locate(&mut self) -> std::ops::Range<u32> {
        loop {
            return match (self.stsc_peek_iter.next(), self.stsc_peek_iter.peek()) {
                (Some(next), Some(peek)) if next.first_chunk == peek.first_chunk => {
                    continue;
                }
                (Some(next), Some(peek)) if next.first_chunk > 0 && peek.first_chunk > 0 => {
                    self.sample_count = next.samples_per_chunk;
                    (next.first_chunk - 1)..(peek.first_chunk - 1)
                }
                (Some(next), None) if next.first_chunk > 0 => {
                    self.sample_count = next.samples_per_chunk;
                    match next.first_chunk.checked_add(self.remain_chunk_count) {
                        Some(r) => (next.first_chunk - 1)..r - 1,
                        _ => 0..0,
                    }
                }
                _ => 0..0,
            };
        }
    }
}

/// Calculate numerator * scale / denominator, if possible.
///
/// Applying the associativity of integer arithmetic, we divide first
/// and add the remainder after multiplying each term separately
/// to preserve precision while leaving more headroom. That is,
/// (n * s) / d is split into floor(n / d) * s + (n % d) * s / d.
///
/// Return None on overflow or if the denominator is zero.
pub fn rational_scale<T, S>(numerator: T, denominator: T, scale2: S) -> Option<T>
where
    T: PrimInt + Zero,
    S: PrimInt,
{
    if denominator.is_zero() {
        return None;
    }

    let integer = numerator / denominator;
    let remainder = numerator % denominator;
    num_traits::cast(scale2).and_then(|s| match integer.checked_mul(&s) {
        Some(integer) => remainder
            .checked_mul(&s)
            .and_then(|remainder| (remainder / denominator).checked_add(&integer)),
        None => None,
    })
}

#[derive(Debug, PartialEq, Eq)]
pub struct Microseconds<T>(pub T);

/// Convert `time` in media's global (mvhd) timescale to microseconds,
/// using provided `MediaTimeScale`
pub fn media_time_to_us(time: MediaScaledTime, scale: MediaTimeScale) -> Option<Microseconds<u64>> {
    let microseconds_per_second = 1_000_000;
    rational_scale(time.0, scale.0, microseconds_per_second).map(Microseconds)
}

/// Convert `time` in track's local (mdhd) timescale to microseconds,
/// using provided `TrackTimeScale<T>`
pub fn track_time_to_us<T>(
    time: TrackScaledTime<T>,
    scale: TrackTimeScale<T>,
) -> Option<Microseconds<T>>
where
    T: PrimInt + Zero,
{
    assert_eq!(time.1, scale.1);
    let microseconds_per_second = 1_000_000;
    rational_scale(time.0, scale.0, microseconds_per_second).map(Microseconds)
}
