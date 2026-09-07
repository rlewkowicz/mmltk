// Copyright (C) 2023, Cloudflare, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright notice,
//     * Redistributions in binary form must reproduce the above copyright
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR

use crate::QlogSeq;

/// Represents the format of the read event.
#[allow(clippy::large_enum_variant)]
#[derive(Clone, Debug)]
pub enum Event {
    /// A native qlog event type.
    Qlog(crate::events::Event),

    Json(crate::events::JsonEvent),
}

/// A helper object specialized for reading JSON-SEQ qlog from a [`BufRead`]
/// trait.
///
/// [`BufRead`]: https://doc.rust-lang.org/std/io/trait.BufRead.html
pub struct QlogSeqReader<'a> {
    pub qlog: QlogSeq,
    reader: Box<dyn std::io::BufRead + Send + Sync + 'a>,
}

impl<'a> QlogSeqReader<'a> {
    pub fn new(
        mut reader: Box<dyn std::io::BufRead + Send + Sync + 'a>,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        Self::read_record(reader.as_mut());

        let header = Self::read_record(reader.as_mut()).ok_or_else(|| {
            std::io::Error::other("error reading file header bytes")
        })?;

        let res: Result<QlogSeq, serde_json::Error> =
            serde_json::from_slice(&header);
        match res {
            Ok(qlog) => Ok(Self { qlog, reader }),

            Err(e) => Err(e.into()),
        }
    }

    fn read_record(
        reader: &mut (dyn std::io::BufRead + Send + Sync),
    ) -> Option<Vec<u8>> {
        let mut buf = Vec::<u8>::new();
        let size = reader.read_until(b'', &mut buf).unwrap();
        if size <= 1 {
            return None;
        }

        buf.truncate(buf.len() - 1);

        Some(buf)
    }
}

impl Iterator for QlogSeqReader<'_> {
    type Item = Event;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        while let Some(bytes) = Self::read_record(&mut self.reader) {
            let r: serde_json::Result<crate::events::Event> =
                serde_json::from_slice(&bytes);

            if let Ok(event) = r {
                return Some(Event::Qlog(event));
            }

            let r: serde_json::Result<crate::events::JsonEvent> =
                serde_json::from_slice(&bytes);

            if let Ok(event) = r {
                return Some(Event::Json(event));
            }
        }

        None
    }
}
