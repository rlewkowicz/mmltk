// Copyright 2015 The Servo Project Developers. See the
// COPYRIGHT file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! 3.3.3 Preparations for Implicit Processing
//!
//! <http://www.unicode.org/reports/tr9/#Preparations_for_Implicit_Processing>

use alloc::vec::Vec;
use core::cmp::max;
use core::ops::Range;
#[cfg(feature = "smallvec")]
use smallvec::{smallvec, SmallVec};

use super::level::Level;
use super::BidiClass::{self, *};

/// A maximal substring of characters with the same embedding level.
///
/// Represented as a range of byte indices.
pub type LevelRun = Range<usize>;

#[cfg(feature = "smallvec")]
pub type LevelRunVec = SmallVec<[LevelRun; 8]>;
#[cfg(not(feature = "smallvec"))]
pub type LevelRunVec = Vec<LevelRun>;

/// Output of `isolating_run_sequences` (steps X9-X10)
#[derive(Debug, PartialEq)]
pub struct IsolatingRunSequence {
    pub runs: Vec<LevelRun>,
    pub sos: BidiClass, 
    pub eos: BidiClass, 
}

#[cfg(feature = "smallvec")]
pub type IsolatingRunSequenceVec = SmallVec<[IsolatingRunSequence; 8]>;
#[cfg(not(feature = "smallvec"))]
pub type IsolatingRunSequenceVec = Vec<IsolatingRunSequence>;

/// Compute the set of isolating run sequences.
///
/// An isolating run sequence is a maximal sequence of level runs such that for all level runs
/// except the last one in the sequence, the last character of the run is an isolate initiator
/// whose matching PDI is the first character of the next level run in the sequence.
///
/// Note: This function does *not* return the sequences in order by their first characters.
#[cfg_attr(feature = "flame_it", flamer::flame)]
pub fn isolating_run_sequences(
    para_level: Level,
    original_classes: &[BidiClass],
    levels: &[Level],
    runs: LevelRunVec,
    has_isolate_controls: bool,
    isolating_run_sequences: &mut IsolatingRunSequenceVec,
) {
    if !has_isolate_controls {
        isolating_run_sequences.reserve_exact(runs.len());
        for run in runs {

            let run_levels = &levels[run.clone()];
            let run_classes = &original_classes[run.clone()];
            let seq_level = run_levels[run_classes
                .iter()
                .position(|c| not_removed_by_x9(c))
                .unwrap_or(0)];

            let end_level = run_levels[run_classes
                .iter()
                .rposition(|c| not_removed_by_x9(c))
                .unwrap_or(run.end - run.start - 1)];

            let pred_level = match original_classes[..run.start]
                .iter()
                .rposition(not_removed_by_x9)
            {
                Some(idx) => levels[idx],
                None => para_level,
            };

            let succ_level = match original_classes[run.end..]
                .iter()
                .position(not_removed_by_x9)
            {
                Some(idx) => levels[run.end + idx],
                None => para_level,
            };

            isolating_run_sequences.push(IsolatingRunSequence {
                runs: vec![run],
                sos: max(seq_level, pred_level).bidi_class(),
                eos: max(end_level, succ_level).bidi_class(),
            });
        }
        return;
    }

    let mut sequences = Vec::with_capacity(runs.len());

    #[cfg(feature = "smallvec")]
    let mut stack: SmallVec<[Vec<Range<usize>>; 8]> = smallvec![vec![]];
    #[cfg(not(feature = "smallvec"))]
    let mut stack = vec![vec![]];

    for run in runs {
        assert!(!run.is_empty());
        assert!(!stack.is_empty());

        let start_class = original_classes[run.start];
        let end_class = original_classes[run.start..run.end]
            .iter()
            .copied()
            .rev()
            .find(not_removed_by_x9)
            .unwrap_or(start_class);

        let mut sequence = if start_class == PDI && stack.len() > 1 {
            stack.pop().unwrap()
        } else {
            Vec::new()
        };

        sequence.push(run);

        if matches!(end_class, RLI | LRI | FSI) {
            stack.push(sequence);
        } else {
            sequences.push(sequence);
        }
    }
    sequences.extend(stack.into_iter().rev().filter(|seq| !seq.is_empty()));

    for sequence in sequences {
        assert!(!sequence.is_empty());

        let start_of_seq = sequence[0].start;
        let runs_len = sequence.len();
        let end_of_seq = sequence[runs_len - 1].end;

        let mut result = IsolatingRunSequence {
            runs: sequence,
            sos: L,
            eos: L,
        };

        let seq_level = levels[result
            .iter_forwards_from(start_of_seq, 0)
            .find(|i| not_removed_by_x9(&original_classes[*i]))
            .unwrap_or(start_of_seq)];

        let end_level = levels[result
            .iter_backwards_from(end_of_seq, runs_len - 1)
            .find(|i| not_removed_by_x9(&original_classes[*i]))
            .unwrap_or(end_of_seq - 1)];

#[cfg(any())]










        for idx in result.runs.clone().into_iter().flatten() {
            if not_removed_by_x9(&original_classes[idx]) {
                assert_eq!(seq_level, levels[idx]);
            }
        }

        let pred_level = match original_classes[..start_of_seq]
            .iter()
            .rposition(not_removed_by_x9)
        {
            Some(idx) => levels[idx],
            None => para_level,
        };

        let last_non_removed = original_classes[..end_of_seq]
            .iter()
            .copied()
            .rev()
            .find(not_removed_by_x9)
            .unwrap_or(BN);

        let succ_level = if matches!(last_non_removed, RLI | LRI | FSI) {
            para_level
        } else {
            match original_classes[end_of_seq..]
                .iter()
                .position(not_removed_by_x9)
            {
                Some(idx) => levels[end_of_seq + idx],
                None => para_level,
            }
        };

        result.sos = max(seq_level, pred_level).bidi_class();
        result.eos = max(end_level, succ_level).bidi_class();

        isolating_run_sequences.push(result);
    }
}

impl IsolatingRunSequence {
    /// Given a text-relative position `pos` and an index of the level run it is in,
    /// produce an iterator of all characters after and pos (`pos..`) that are in this
    /// run sequence
    pub(crate) fn iter_forwards_from(
        &self,
        pos: usize,
        level_run_index: usize,
    ) -> impl Iterator<Item = usize> + '_ {
        let runs = &self.runs[level_run_index..];

        #[cfg(feature = "std")]
        debug_assert!(runs[0].start <= pos && pos <= runs[0].end);

        (pos..runs[0].end).chain(runs[1..].iter().flat_map(Clone::clone))
    }

    /// Given a text-relative position `pos` and an index of the level run it is in,
    /// produce an iterator of all characters before and excludingpos (`..pos`) that are in this
    /// run sequence
    pub(crate) fn iter_backwards_from(
        &self,
        pos: usize,
        level_run_index: usize,
    ) -> impl Iterator<Item = usize> + '_ {
        let prev_runs = &self.runs[..level_run_index];
        let current = &self.runs[level_run_index];

        #[cfg(feature = "std")]
        debug_assert!(current.start <= pos && pos <= current.end);

        (current.start..pos)
            .rev()
            .chain(prev_runs.iter().rev().flat_map(Clone::clone))
    }
}

/// Finds the level runs in a paragraph.
///
/// <http://www.unicode.org/reports/tr9/#BD7>
///
/// This is only used by tests; normally level runs are identified during explicit::compute.

/// Should this character be ignored in steps after X9?
///
/// <http://www.unicode.org/reports/tr9/#X9>
pub fn removed_by_x9(class: BidiClass) -> bool {
    matches!(class, RLE | LRE | RLO | LRO | PDF | BN)
}

pub fn not_removed_by_x9(class: &BidiClass) -> bool {
    !removed_by_x9(*class)
}
