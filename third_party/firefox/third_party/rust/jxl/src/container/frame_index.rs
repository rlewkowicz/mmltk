// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

//! Parser for the JPEG XL Frame Index box (`jxli`), as specified in
//! the JPEG XL container specification.
//!
//! The frame index box provides a seek table for animated JXL files,
//! listing keyframe byte offsets in the codestream, timestamps, and
//! frame counts.

use std::num::NonZero;

use byteorder::{BigEndian, ReadBytesExt};

use crate::error::{Error, Result};
use crate::icc::read_varint_from_reader;
use crate::util::NewWithCapacity;

/// A single entry in the frame index.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameIndexEntry {
    /// Absolute byte offset of this keyframe in the codestream.
    /// (Accumulated from the delta-coded OFFi values.)
    pub codestream_offset: u64,
    /// Duration in ticks from this indexed frame to the next indexed frame
    /// (or end of stream for the last entry). A tick lasts TNUM/TDEN seconds.
    pub duration_ticks: u64,
    /// Number of displayed frames from this indexed frame to the next indexed
    /// frame (or end of stream for the last entry).
    pub frame_count: u64,
}

/// Parsed contents of a Frame Index box (`jxli`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameIndexBox {
    /// Tick numerator. A tick lasts `tnum / tden` seconds.
    pub tnum: u32,
    /// Tick denominator (non-zero per spec).
    pub tden: NonZero<u32>,
    /// Indexed frame entries.
    pub entries: Vec<FrameIndexEntry>,
}

impl FrameIndexBox {
    /// Returns the number of indexed frames.
    pub fn num_frames(&self) -> usize {
        self.entries.len()
    }

    /// Returns the duration of one tick in seconds.
    pub fn tick_duration_secs(&self) -> f64 {
        self.tnum as f64 / self.tden.get() as f64
    }

    /// Finds the index entry for the keyframe at or before the given
    /// codestream byte offset.
    pub fn entry_for_offset(&self, offset: u64) -> Option<&FrameIndexEntry> {
        match self
            .entries
            .binary_search_by_key(&offset, |e| e.codestream_offset)
        {
            Ok(i) => Some(&self.entries[i]),
            Err(0) => None,
            Err(i) => Some(&self.entries[i - 1]),
        }
    }

    /// Parse a frame index box from its raw content bytes (after the box header).
    pub fn parse(data: &[u8]) -> Result<Self> {
        let mut reader = data;

        let nf = read_varint_from_reader(&mut reader)?;
        if nf > u32::MAX as u64 {
            return Err(Error::InvalidBox);
        }
        let nf = nf as usize;

        let tnum = reader
            .read_u32::<BigEndian>()
            .map_err(|_| Error::InvalidBox)?;
        let tden = NonZero::new(
            reader
                .read_u32::<BigEndian>()
                .map_err(|_| Error::InvalidBox)?,
        )
        .ok_or(Error::InvalidBox)?;

        let mut entries = Vec::new_with_capacity(nf.min(reader.len() / 3))?;
        let mut absolute_offset: u64 = 0;

        for _ in 0..nf {
            let off_delta = read_varint_from_reader(&mut reader)?;
            let duration_ticks = read_varint_from_reader(&mut reader)?;
            let frame_count = read_varint_from_reader(&mut reader)?;

            absolute_offset = absolute_offset
                .checked_add(off_delta)
                .ok_or(Error::InvalidBox)?;

            entries.push(FrameIndexEntry {
                codestream_offset: absolute_offset,
                duration_ticks,
                frame_count,
            });
        }

        Ok(FrameIndexBox {
            tnum,
            tden,
            entries,
        })
    }
}
