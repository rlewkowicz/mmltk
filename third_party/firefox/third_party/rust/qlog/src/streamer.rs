// Copyright (C) 2021, Cloudflare, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright notice,
//     * Redistributions in binary form must reproduce the above copyright
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR

use crate::events::EventData;
use crate::events::EventImportance;
use crate::events::EventType;
use crate::events::Eventable;
use crate::events::ExData;

/// A helper object specialized for streaming JSON-serialized qlog to a
/// [`Write`] trait.
///
/// The object is responsible for the `Qlog` object that contains the
/// provided `Trace`.
///
/// Serialization is progressively driven by method calls; once log streaming
/// is started, `event::Events` can be written using `add_event()`.
///
/// [`Write`]: https://doc.rust-lang.org/std/io/trait.Write.html
use super::*;

#[derive(PartialEq, Eq, Debug)]
pub enum StreamerState {
    Initial,
    Ready,
    Finished,
}

pub struct QlogStreamer {
    start_time: std::time::Instant,
    writer: Box<dyn std::io::Write + Send + Sync>,
    qlog: QlogSeq,
    state: StreamerState,
    log_level: EventImportance,
}

impl QlogStreamer {
    /// Creates a [QlogStreamer] object.
    ///
    /// It owns a [QlogSeq] object that contains the provided [TraceSeq]
    /// containing [Event]s.
    ///
    /// All serialization will be written to the provided [`Write`] using the
    /// JSON-SEQ format.
    ///
    /// [`Write`]: https://doc.rust-lang.org/std/io/trait.Write.html
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        qlog_version: String, title: Option<String>, description: Option<String>,
        summary: Option<String>, start_time: std::time::Instant, trace: TraceSeq,
        log_level: EventImportance,
        writer: Box<dyn std::io::Write + Send + Sync>,
    ) -> Self {
        let qlog = QlogSeq {
            qlog_version,
            qlog_format: "JSON-SEQ".to_string(),
            title,
            description,
            summary,
            trace,
        };

        QlogStreamer {
            start_time,
            writer,
            qlog,
            state: StreamerState::Initial,
            log_level,
        }
    }

    /// Starts qlog streaming serialization.
    ///
    /// This writes out the JSON-SEQ-serialized form of all initial qlog
    /// information. [Event]s are separately appended using [add_event()],
    /// [add_event_with_instant()], [add_event_now()],
    /// [add_event_data_with_instant()], or [add_event_data_now()].
    ///
    /// [add_event()]: #method.add_event
    /// [add_event_with_instant()]: #method.add_event_with_instant
    /// [add_event_now()]: #method.add_event_now
    /// [add_event_data_with_instant()]: #method.add_event_data_with_instant
    /// [add_event_data_now()]: #method.add_event_data_now
    pub fn start_log(&mut self) -> Result<()> {
        if self.state != StreamerState::Initial {
            return Err(Error::Done);
        }

        self.writer.as_mut().write_all(b"")?;
        serde_json::to_writer(self.writer.as_mut(), &self.qlog)
            .map_err(|_| Error::Done)?;
        self.writer.as_mut().write_all(b"\n")?;

        self.state = StreamerState::Ready;

        Ok(())
    }

    /// Finishes qlog streaming serialization.
    ///
    /// After this is called, no more serialization will occur.
    pub fn finish_log(&mut self) -> Result<()> {
        if self.state == StreamerState::Initial ||
            self.state == StreamerState::Finished
        {
            return Err(Error::InvalidState);
        }

        self.state = StreamerState::Finished;

        self.writer.as_mut().flush()?;

        Ok(())
    }

    /// Writes a serializable to a JSON-SEQ record using
    /// [std::time::Instant::now()].
    pub fn add_event_now<E: Serialize + Eventable>(
        &mut self, event: E,
    ) -> Result<()> {
        let now = std::time::Instant::now();

        self.add_event_with_instant(event, now)
    }

    /// Writes a serializable to a pretty-printed JSON-SEQ record using
    /// [std::time::Instant::now()].
    pub fn add_event_now_pretty<E: Serialize + Eventable>(
        &mut self, event: E,
    ) -> Result<()> {
        let now = std::time::Instant::now();

        self.add_event_with_instant_pretty(event, now)
    }

    /// Writes a serializable to a JSON-SEQ record using the provided
    /// [std::time::Instant].
    pub fn add_event_with_instant<E: Serialize + Eventable>(
        &mut self, event: E, now: std::time::Instant,
    ) -> Result<()> {
        self.event_with_instant(event, now, false)
    }

    /// Writes a serializable to a pretty-printed JSON-SEQ record using the
    /// provided [std::time::Instant].
    pub fn add_event_with_instant_pretty<E: Serialize + Eventable>(
        &mut self, event: E, now: std::time::Instant,
    ) -> Result<()> {
        self.event_with_instant(event, now, true)
    }

    fn event_with_instant<E: Serialize + Eventable>(
        &mut self, mut event: E, now: std::time::Instant, pretty: bool,
    ) -> Result<()> {
        if self.state != StreamerState::Ready {
            return Err(Error::InvalidState);
        }

        if !event.importance().is_contained_in(&self.log_level) {
            return Err(Error::Done);
        }

        let dur = if false {
            std::time::Duration::from_secs(0)
        } else {
            now.duration_since(self.start_time)
        };

        let rel_time = dur.as_secs_f32() * 1000.0;
        event.set_time(rel_time);

        if pretty {
            self.add_event_pretty(event)
        } else {
            self.add_event(event)
        }
    }

    /// Writes an [Event] based on the provided [EventData] to a JSON-SEQ record
    /// at time [std::time::Instant::now()].
    pub fn add_event_data_now(&mut self, event_data: EventData) -> Result<()> {
        self.add_event_data_ex_now(event_data, Default::default())
    }

    /// Writes an [Event] based on the provided [EventData] to a pretty-printed
    /// JSON-SEQ record at time [std::time::Instant::now()].
    pub fn add_event_data_now_pretty(
        &mut self, event_data: EventData,
    ) -> Result<()> {
        self.add_event_data_ex_now_pretty(event_data, Default::default())
    }

    /// Writes an [Event] based on the provided [EventData] and [ExData] to a
    /// JSON-SEQ record at time [std::time::Instant::now()].
    pub fn add_event_data_ex_now(
        &mut self, event_data: EventData, ex_data: ExData,
    ) -> Result<()> {
        let now = std::time::Instant::now();

        self.add_event_data_ex_with_instant(event_data, ex_data, now)
    }

    /// Writes an [Event] based on the provided [EventData] and [ExData] to a
    /// pretty-printed JSON-SEQ record at time [std::time::Instant::now()].
    pub fn add_event_data_ex_now_pretty(
        &mut self, event_data: EventData, ex_data: ExData,
    ) -> Result<()> {
        let now = std::time::Instant::now();

        self.add_event_data_ex_with_instant_pretty(event_data, ex_data, now)
    }

    /// Writes an [Event] based on the provided [EventData] and
    /// [std::time::Instant] to a JSON-SEQ record.
    pub fn add_event_data_with_instant(
        &mut self, event_data: EventData, now: std::time::Instant,
    ) -> Result<()> {
        self.add_event_data_ex_with_instant(event_data, Default::default(), now)
    }

    /// Writes an [Event] based on the provided [EventData] and
    /// [std::time::Instant] to a pretty-printed JSON-SEQ record.
    pub fn add_event_data_with_instant_pretty(
        &mut self, event_data: EventData, now: std::time::Instant,
    ) -> Result<()> {
        self.add_event_data_ex_with_instant_pretty(
            event_data,
            Default::default(),
            now,
        )
    }

    /// Writes an [Event] based on the provided [EventData], [ExData], and
    /// [std::time::Instant] to a JSON-SEQ record.
    pub fn add_event_data_ex_with_instant(
        &mut self, event_data: EventData, ex_data: ExData,
        now: std::time::Instant,
    ) -> Result<()> {
        self.event_data_ex_with_instant(event_data, ex_data, now, false)
    }

    /// [std::time::Instant] to a pretty-printed JSON-SEQ record.
    pub fn add_event_data_ex_with_instant_pretty(
        &mut self, event_data: EventData, ex_data: ExData,
        now: std::time::Instant,
    ) -> Result<()> {
        self.event_data_ex_with_instant(event_data, ex_data, now, true)
    }

    fn event_data_ex_with_instant(
        &mut self, event_data: EventData, ex_data: ExData,
        now: std::time::Instant, pretty: bool,
    ) -> Result<()> {
        if self.state != StreamerState::Ready {
            return Err(Error::InvalidState);
        }

        let ty = EventType::from(&event_data);
        if !EventImportance::from(ty).is_contained_in(&self.log_level) {
            return Err(Error::Done);
        }

        let dur = if false {
            std::time::Duration::from_secs(0)
        } else {
            now.duration_since(self.start_time)
        };

        let rel_time = dur.as_secs_f32() * 1000.0;
        let event = Event::with_time_ex(rel_time, event_data, ex_data);

        if pretty {
            self.add_event_pretty(event)
        } else {
            self.add_event(event)
        }
    }

    /// Writes a JSON-SEQ-serialized [Event] using the provided [Event].
    pub fn add_event<E: Serialize + Eventable>(
        &mut self, event: E,
    ) -> Result<()> {
        self.write_event(event, false)
    }

    /// Writes a pretty-printed JSON-SEQ-serialized [Event] using the provided
    /// [Event].
    pub fn add_event_pretty<E: Serialize + Eventable>(
        &mut self, event: E,
    ) -> Result<()> {
        self.write_event(event, true)
    }

    /// Writes a JSON-SEQ-serialized [Event] using the provided [Event].
    fn write_event<E: Serialize + Eventable>(
        &mut self, event: E, pretty: bool,
    ) -> Result<()> {
        if self.state != StreamerState::Ready {
            return Err(Error::InvalidState);
        }

        if !event.importance().is_contained_in(&self.log_level) {
            return Err(Error::Done);
        }

        self.writer.as_mut().write_all(b"")?;
        if pretty {
            serde_json::to_writer_pretty(self.writer.as_mut(), &event)
                .map_err(|_| Error::Done)?;
        } else {
            serde_json::to_writer(self.writer.as_mut(), &event)
                .map_err(|_| Error::Done)?;
        }
        self.writer.as_mut().write_all(b"\n")?;

        Ok(())
    }

    /// Returns the writer.
    #[allow(clippy::borrowed_box)]
    pub fn writer(&self) -> &Box<dyn std::io::Write + Send + Sync> {
        &self.writer
    }

    pub fn start_time(&self) -> std::time::Instant {
        self.start_time
    }
}

impl Drop for QlogStreamer {
    fn drop(&mut self) {
        let _ = self.finish_log();
    }
}
