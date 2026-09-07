// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    cell::RefCell,
    cmp::max,
    collections::BTreeMap,
    fmt::Debug,
    mem,
    rc::{Rc, Weak},
    time::{Duration, Instant},
};

use neqo_common::{Buffer, Role, qtrace};
use smallvec::SmallVec;
use strum::Display;

use crate::{
    AppError, Error, Res,
    events::ConnectionEvents,
    fc::ReceiverFlowControl,
    frame::FrameType,
    packet,
    recovery::{self, StreamRecoveryToken},
    send_stream::SendStreams,
    stats::FrameStats,
    stream_id::StreamId,
};

#[derive(Debug, Default)]
pub struct RecvStreams {
    streams: BTreeMap<StreamId, RecvStream>,
    keep_alive: Weak<()>,
    /// Set when any stream has ended; cleared by `remove_ended`.
    has_ended: bool,
}

impl RecvStreams {
    pub fn write_frames<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
        now: Instant,
        rtt: Duration,
    ) {
        for stream in self.streams.values_mut() {
            stream.write_frame(builder, tokens, stats, now, rtt);
            if builder.is_full() {
                return;
            }
        }
    }

    pub fn insert(&mut self, id: StreamId, stream: RecvStream) {
        self.streams.insert(id, stream);
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_errors_doc,
        reason = "OK here."
    )]
    pub fn get_mut(&mut self, id: StreamId) -> Res<&mut RecvStream> {
        self.streams.get_mut(&id).ok_or(Error::InvalidStreamId)
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_errors_doc,
        reason = "OK here."
    )]
    pub fn keep_alive(&mut self, id: StreamId, k: bool) -> Res<()> {
        let self_ka = &mut self.keep_alive;
        let s = self.streams.get_mut(&id).ok_or(Error::InvalidStreamId)?;
        s.keep_alive = k.then(|| {
            self_ka.upgrade().unwrap_or_else(|| {
                let r = Rc::new(());
                *self_ka = Rc::downgrade(&r);
                r
            })
        });
        Ok(())
    }

    #[must_use]
    pub fn need_keep_alive(&self) -> bool {
        self.keep_alive.strong_count() > 0
    }

    pub fn clear(&mut self) {
        self.streams.clear();
        self.has_ended = false;
    }

    pub(crate) const fn set_ended(&mut self, ended: bool) {
        self.has_ended |= ended;
    }

    /// Read from a stream, noting when it ends.
    ///
    /// # Errors
    /// When the stream does not exist or has no more data.
    pub fn read(&mut self, stream_id: StreamId, data: &mut [u8]) -> Res<(usize, bool)> {
        let (n, fin) = self.get_mut(stream_id)?.read(data)?;
        self.set_ended(fin);
        Ok((n, fin))
    }

    /// Stop sending on a stream, noting when it ends.
    ///
    /// # Errors
    /// When the stream does not exist.
    pub fn stop_sending(&mut self, stream_id: StreamId, err: AppError) -> Res<()> {
        let ended = self.get_mut(stream_id)?.stop_sending(err);
        self.set_ended(ended);
        Ok(())
    }

    /// Reset a stream, noting if it ended.
    ///
    /// # Errors
    /// When flow control is violated.
    pub fn reset(
        &mut self,
        stream_id: StreamId,
        application_error_code: AppError,
        final_size: u64,
    ) -> Res<()> {
        if let Ok(rs) = self.get_mut(stream_id) {
            let ended = rs.reset(application_error_code, final_size)?;
            self.set_ended(ended);
        }
        Ok(())
    }

    /// Note whether a stop-sending ack ended the stream.
    pub fn stop_sending_acked(&mut self, stream_id: StreamId) {
        if let Ok(rs) = self.get_mut(stream_id) {
            let ended = rs.stop_sending_acked();
            self.set_ended(ended);
        }
    }

    pub fn remove_ended(&mut self, send_streams: &SendStreams, role: Role) -> (u64, u64) {
        if !self.has_ended {
            return (0, 0);
        }
        self.has_ended = false;
        let mut removed_bidi = 0;
        let mut removed_uni = 0;
        self.streams.retain(|id, s| {
            let dead = s.is_ended() && (id.is_uni() || !send_streams.exists(*id));
            if dead && id.is_remote_initiated(role) {
                if id.is_bidi() {
                    removed_bidi += 1;
                } else {
                    removed_uni += 1;
                }
            }
            !dead
        });

        (removed_bidi, removed_uni)
    }
}

/// Holds data not yet read by application. Orders and dedupes data ranges
/// from incoming STREAM frames.
#[derive(Debug, Default)]
pub struct RxStreamOrderer {
    data_ranges: BTreeMap<u64, Vec<u8>>, 
    retired: u64,                        
    received: u64,                       
}

impl RxStreamOrderer {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Process an incoming stream frame off the wire. This may result in data
    /// being available to upper layers if frame is not out of order (ooo) or
    /// if the frame fills a gap.
    /// # Panics
    /// Only when `u64` values cannot be converted to `usize`, which only
    /// happens on 32-bit machines that hold far too much data at the same time.
    pub fn inbound_frame(&mut self, mut new_start: u64, mut new_data: &[u8]) {
        qtrace!("Inbound data offset={new_start} len={}", new_data.len());

        let new_end = new_start + u64::try_from(new_data.len()).expect("usize fits in u64");

        if new_end <= self.retired {
            return;
        }

        if new_start < self.retired {
            new_data =
                &new_data[usize::try_from(self.retired - new_start).expect("u64 fits in usize")..];
            new_start = self.retired;
        }

        if new_data.is_empty() {
            return;
        }

        let extend = if let Some((&prev_start, prev_vec)) =
            self.data_ranges.range_mut(..=new_start).next_back()
        {
            let prev_end = prev_start + u64::try_from(prev_vec.len()).expect("usize fits in u64");
            if new_end > prev_end {
                let overlap = prev_end.saturating_sub(new_start);
                qtrace!("New frame {new_start}-{new_end} received, overlap: {overlap}");
                new_start += overlap;
                new_data = &new_data[usize::try_from(overlap).expect("u64 fits in usize")..];
                prev_vec.len() < 4096 && prev_end == new_start
            } else {
                qtrace!("Dropping frame with already-received range {new_start}-{new_end}");
                return;
            }
        } else {
            qtrace!("New frame {new_start}-{new_end} received");
            false
        };

        let mut to_add = new_data;
        if self
            .data_ranges
            .last_entry()
            .is_some_and(|e| *e.key() >= new_start)
        {

            let mut to_remove = SmallVec::<[_; 8]>::new();

            for (&next_start, next_data) in self.data_ranges.range_mut(new_start..) {
                let next_end =
                    next_start + u64::try_from(next_data.len()).expect("usize fits in u64");
                let overlap = new_end.saturating_sub(next_start);
                if overlap == 0 {
                    break;
                } else if next_end >= new_end {
                    qtrace!(
                        "New frame {new_start}-{new_end} overlaps with next frame by {overlap}, truncating"
                    );
                    let truncate_to =
                        new_data.len() - usize::try_from(overlap).expect("u64 fits in usize");
                    to_add = &new_data[..truncate_to];
                    break;
                }
                qtrace!(
                    "New frame {new_start}-{new_end} spans entire next frame {next_start}-{next_end}, replacing"
                );
                to_remove.push(next_start);
            }

            for start in to_remove {
                self.data_ranges.remove(&start);
            }
        }

        if !to_add.is_empty() {
            self.received += u64::try_from(to_add.len()).expect("usize fits in u64");
            if extend {
                if let Some((_, buf)) = self.data_ranges.range_mut(..=new_start).next_back() {
                    buf.extend_from_slice(to_add);
                }
            } else {
                self.data_ranges.insert(new_start, to_add.to_vec());
            }
        }
    }

    /// Are any bytes readable?
    #[must_use]
    pub fn data_ready(&self) -> bool {
        self.data_ranges
            .keys()
            .next()
            .is_some_and(|&start| start <= self.retired)
    }

    /// How many bytes are readable?
    fn bytes_ready(&self) -> usize {
        let mut prev_end = self.retired;
        self.data_ranges
            .iter()
            .map(|(start_offset, data)| {
                let data_len = data.len() as u64 - self.retired.saturating_sub(*start_offset);
                (start_offset, data_len)
            })
            .take_while(|(start_offset, data_len)| {
                if **start_offset <= prev_end {
                    prev_end += data_len;
                    true
                } else {
                    false
                }
            })
            .fold(0, |acc: usize, (_, data_len)| {
                acc.saturating_add(usize::try_from(data_len).unwrap_or(usize::MAX))
            })
    }

    /// Bytes read by the application.
    #[must_use]
    pub const fn retired(&self) -> u64 {
        self.retired
    }

    #[must_use]
    pub const fn received(&self) -> u64 {
        self.received
    }

    /// Data bytes buffered. Could be more than `bytes_readable` if there are
    /// ranges missing.
    fn buffered(&self) -> u64 {
        self.data_ranges
            .iter()
            .map(|(&start, data)| data.len() as u64 - (self.retired.saturating_sub(start)))
            .sum()
    }

    /// Copy received data (if any) into the buffer. Returns bytes copied.
    fn read(&mut self, buf: &mut [u8]) -> usize {
        qtrace!("Reading {} bytes, {} available", buf.len(), self.buffered());
        let mut copied = 0;

        for (&range_start, range_data) in &mut self.data_ranges {
            let mut keep = false;
            if self.retired >= range_start {
                let copy_offset = usize::try_from(max(range_start, self.retired) - range_start)
                    .expect("u64 fits in usize");
                assert!(range_data.len() >= copy_offset);
                let available = range_data.len() - copy_offset;
                let space = buf.len() - copied;
                let copy_bytes = if available > space {
                    keep = true;
                    space
                } else {
                    available
                };

                if copy_bytes > 0 {
                    let copy_slc = &range_data[copy_offset..copy_offset + copy_bytes];
                    buf[copied..copied + copy_bytes].copy_from_slice(copy_slc);
                    copied += copy_bytes;
                    self.retired += u64::try_from(copy_bytes).expect("usize fits in u64");
                }
            } else {
                keep = true;
            }
            if keep {
                let mut keep = self.data_ranges.split_off(&range_start);
                mem::swap(&mut self.data_ranges, &mut keep);
                return copied;
            }
        }

        self.data_ranges.clear();
        copied
    }

    /// Extend the given Vector with any available data.
    pub fn read_to_end(&mut self, buf: &mut Vec<u8>) -> usize {
        let orig_len = buf.len();
        buf.resize(orig_len + self.bytes_ready(), 0);
        self.read(&mut buf[orig_len..])
    }
}

/// QUIC receiving states, based on -transport 3.2.
#[derive(Debug, Display)]
// Because a dead_code warning is easier than clippy::unused_self, see https://github.com/rust-lang/rust/issues/68408
enum RecvStreamState {
    Recv {
        fc: ReceiverFlowControl<StreamId>,
        session_fc: Rc<RefCell<ReceiverFlowControl<()>>>,
        recv_buf: RxStreamOrderer,
    },
    SizeKnown {
        fc: ReceiverFlowControl<StreamId>,
        session_fc: Rc<RefCell<ReceiverFlowControl<()>>>,
        recv_buf: RxStreamOrderer,
    },
    DataRecvd {
        fc: ReceiverFlowControl<StreamId>,
        session_fc: Rc<RefCell<ReceiverFlowControl<()>>>,
        recv_buf: RxStreamOrderer,
    },
    DataRead {
        final_received: u64,
        final_read: u64,
    },
    AbortReading {
        fc: ReceiverFlowControl<StreamId>,
        session_fc: Rc<RefCell<ReceiverFlowControl<()>>>,
        final_size_reached: bool,
        frame_needed: bool,
        err: AppError,
        final_received: u64,
        final_read: u64,
    },
    WaitForReset {
        fc: ReceiverFlowControl<StreamId>,
        session_fc: Rc<RefCell<ReceiverFlowControl<()>>>,
        final_received: u64,
        final_read: u64,
    },
    ResetRecvd {
        final_received: u64,
        final_read: u64,
    },
}

impl RecvStreamState {
    fn new(
        max_bytes: u64,
        stream_id: StreamId,
        session_fc: Rc<RefCell<ReceiverFlowControl<()>>>,
    ) -> Self {
        Self::Recv {
            fc: ReceiverFlowControl::new(stream_id, max_bytes),
            recv_buf: RxStreamOrderer::new(),
            session_fc,
        }
    }

    const fn recv_buf(&self) -> Option<&RxStreamOrderer> {
        match self {
            Self::Recv { recv_buf, .. }
            | Self::SizeKnown { recv_buf, .. }
            | Self::DataRecvd { recv_buf, .. } => Some(recv_buf),
            Self::DataRead { .. }
            | Self::AbortReading { .. }
            | Self::WaitForReset { .. }
            | Self::ResetRecvd { .. } => None,
        }
    }

    fn flow_control_consume_data(&mut self, consumed: u64, fin: bool) -> Res<()> {
        let (fc, session_fc, final_size_reached, retire_data) = match self {
            Self::Recv { fc, session_fc, .. } => (fc, session_fc, false, false),
            Self::WaitForReset { fc, session_fc, .. } => (fc, session_fc, false, true),
            Self::SizeKnown { fc, session_fc, .. } | Self::DataRecvd { fc, session_fc, .. } => {
                (fc, session_fc, true, false)
            }
            Self::AbortReading {
                fc,
                session_fc,
                final_size_reached,
                ..
            } => {
                let old_final_size_reached = *final_size_reached;
                *final_size_reached |= fin;
                (fc, session_fc, old_final_size_reached, true)
            }
            Self::DataRead { .. } | Self::ResetRecvd { .. } => {
                return Ok(());
            }
        };

        let final_size_ok = match (fin, final_size_reached) {
            (true, true) => consumed == fc.consumed(),
            (false, true) => consumed <= fc.consumed(),
            (true, false) => consumed >= fc.consumed(),
            (false, false) => true,
        };

        if !final_size_ok {
            return Err(Error::FinalSize);
        }

        let new_bytes_consumed = fc.set_consumed(consumed)?;
        session_fc.borrow_mut().consume(new_bytes_consumed)?;
        if retire_data {
            RecvStream::flow_control_retire_data(fc.consumed() - fc.retired(), fc, session_fc);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Stats {
    pub bytes_received: u64,
    pub bytes_read: u64,
}

impl Stats {
    #[must_use]
    pub const fn new(bytes_received: u64, bytes_read: u64) -> Self {
        Self {
            bytes_received,
            bytes_read,
        }
    }

    #[must_use]
    pub const fn bytes_received(&self) -> u64 {
        self.bytes_received
    }

    #[must_use]
    pub const fn bytes_read(&self) -> u64 {
        self.bytes_read
    }
}

/// Implement a QUIC receive stream.
#[derive(Debug)]
pub struct RecvStream {
    stream_id: StreamId,
    state: RecvStreamState,
    conn_events: ConnectionEvents,
    keep_alive: Option<Rc<()>>,
}

impl RecvStream {
    pub fn new(
        stream_id: StreamId,
        max_stream_data: u64,
        session_fc: Rc<RefCell<ReceiverFlowControl<()>>>,
        conn_events: ConnectionEvents,
    ) -> Self {
        Self {
            stream_id,
            state: RecvStreamState::new(max_stream_data, stream_id, session_fc),
            conn_events,
            keep_alive: None,
        }
    }

    fn set_state(&mut self, new_state: RecvStreamState) {
        debug_assert_ne!(
            mem::discriminant(&self.state),
            mem::discriminant(&new_state)
        );
        qtrace!(
            "RecvStream {} state {} -> {new_state}",
            self.stream_id.as_u64(),
            self.state
        );

        match new_state {
            RecvStreamState::DataRecvd { .. }
            | RecvStreamState::AbortReading { .. }
            | RecvStreamState::ResetRecvd { .. } => {
                self.keep_alive = None;
            }
            RecvStreamState::DataRead { .. } => {
                self.conn_events.recv_stream_complete(self.stream_id);
            }
            _ => {}
        }

        self.state = new_state;
    }

    #[must_use]
    pub const fn stats(&self) -> Stats {
        match &self.state {
            RecvStreamState::Recv { recv_buf, .. }
            | RecvStreamState::SizeKnown { recv_buf, .. }
            | RecvStreamState::DataRecvd { recv_buf, .. } => {
                let received = recv_buf.received();
                let read = recv_buf.retired();
                Stats::new(received, read)
            }
            RecvStreamState::AbortReading {
                final_received,
                final_read,
                ..
            }
            | RecvStreamState::WaitForReset {
                final_received,
                final_read,
                ..
            }
            | RecvStreamState::DataRead {
                final_received,
                final_read,
            }
            | RecvStreamState::ResetRecvd {
                final_received,
                final_read,
            } => {
                let received = *final_received;
                let read = *final_read;
                Stats::new(received, read)
            }
        }
    }

    /// # Errors
    /// When the incoming data violates flow control limits.
    /// # Panics
    /// Only when `u64` values are so big that they can't fit in a `usize`, which
    /// only happens on a 32-bit machine that has far too much unread data.
    pub fn inbound_stream_frame(&mut self, fin: bool, offset: u64, data: &[u8]) -> Res<()> {
        let already_data_ready = self.data_ready();
        let new_end = offset + u64::try_from(data.len())?;

        self.state.flow_control_consume_data(new_end, fin)?;

        match &mut self.state {
            RecvStreamState::Recv {
                recv_buf,
                fc,
                session_fc,
            } => {
                recv_buf.inbound_frame(offset, data);
                if fin {
                    let all_recv =
                        fc.consumed() == recv_buf.retired() + recv_buf.bytes_ready() as u64;
                    let buf = mem::replace(recv_buf, RxStreamOrderer::new());
                    let fc_copy = mem::take(fc);
                    let session_fc_copy = mem::take(session_fc);
                    if all_recv {
                        self.set_state(RecvStreamState::DataRecvd {
                            fc: fc_copy,
                            session_fc: session_fc_copy,
                            recv_buf: buf,
                        });
                    } else {
                        self.set_state(RecvStreamState::SizeKnown {
                            fc: fc_copy,
                            session_fc: session_fc_copy,
                            recv_buf: buf,
                        });
                    }
                }
            }
            RecvStreamState::SizeKnown {
                recv_buf,
                fc,
                session_fc,
            } => {
                recv_buf.inbound_frame(offset, data);
                if fc.consumed() == recv_buf.retired() + recv_buf.bytes_ready() as u64 {
                    let buf = mem::replace(recv_buf, RxStreamOrderer::new());
                    let fc_copy = mem::take(fc);
                    let session_fc_copy = mem::take(session_fc);
                    self.set_state(RecvStreamState::DataRecvd {
                        fc: fc_copy,
                        session_fc: session_fc_copy,
                        recv_buf: buf,
                    });
                }
            }
            RecvStreamState::DataRecvd { .. }
            | RecvStreamState::DataRead { .. }
            | RecvStreamState::AbortReading { .. }
            | RecvStreamState::WaitForReset { .. }
            | RecvStreamState::ResetRecvd { .. } => {
                qtrace!("data received when we are in state {}", self.state);
            }
        }

        if !already_data_ready && (self.data_ready() || self.needs_to_inform_app_about_fin()) {
            self.conn_events.recv_stream_readable(self.stream_id);
        }

        Ok(())
    }

    /// # Errors
    /// When the reset occurs at an invalid point.
    ///
    /// # Returns
    /// `true` when the stream transitions to `ResetRecvd` (ended).
    /// `false` if the stream is already in a terminal state and the reset is a no-op.
    pub fn reset(&mut self, application_error_code: AppError, final_size: u64) -> Res<bool> {
        self.state.flow_control_consume_data(final_size, true)?;
        match &mut self.state {
            RecvStreamState::Recv {
                fc,
                session_fc,
                recv_buf,
            }
            | RecvStreamState::SizeKnown {
                fc,
                session_fc,
                recv_buf,
            } => {
                Self::flow_control_retire_data(final_size - fc.retired(), fc, session_fc);
                self.conn_events
                    .recv_stream_reset(self.stream_id, application_error_code);
                let received = recv_buf.received();
                let read = recv_buf.retired();
                self.set_state(RecvStreamState::ResetRecvd {
                    final_received: received,
                    final_read: read,
                });
                Ok(true)
            }
            RecvStreamState::AbortReading {
                fc,
                session_fc,
                final_received,
                final_read,
                ..
            }
            | RecvStreamState::WaitForReset {
                fc,
                session_fc,
                final_received,
                final_read,
            } => {
                Self::flow_control_retire_data(final_size - fc.retired(), fc, session_fc);
                self.conn_events
                    .recv_stream_reset(self.stream_id, application_error_code);
                let received = *final_received;
                let read = *final_read;
                self.set_state(RecvStreamState::ResetRecvd {
                    final_received: received,
                    final_read: read,
                });
                Ok(true)
            }
            _ => Ok(false), 
        }
    }

    fn flow_control_retire_data(
        new_read: u64,
        fc: &mut ReceiverFlowControl<StreamId>,
        session_fc: &Rc<RefCell<ReceiverFlowControl<()>>>,
    ) {
        if new_read > 0 {
            fc.add_retired(new_read);
            session_fc.borrow_mut().add_retired(new_read);
        }
    }

    /// Send a flow control update.
    /// This is used when a peer declares that they are blocked.
    /// This sends `MAX_STREAM_DATA` if there is any increase possible.
    pub const fn send_flowc_update(&mut self) {
        if let RecvStreamState::Recv { fc, .. } = &mut self.state {
            fc.send_flowc_update();
        }
    }

    pub const fn set_stream_max_data(&mut self, max_data: u64) {
        if let RecvStreamState::Recv { fc, .. } = &mut self.state {
            fc.set_max_active(max_data);
        }
    }

    #[must_use]
    pub const fn is_ended(&self) -> bool {
        matches!(
            self.state,
            RecvStreamState::ResetRecvd { .. } | RecvStreamState::DataRead { .. }
        )
    }

    const fn needs_to_inform_app_about_fin(&self) -> bool {
        matches!(self.state, RecvStreamState::DataRecvd { .. })
    }

    fn data_ready(&self) -> bool {
        self.state
            .recv_buf()
            .is_some_and(RxStreamOrderer::data_ready)
    }

    /// # Errors
    /// `NoMoreData` if data and fin bit were previously read by the application.
    pub fn read(&mut self, buf: &mut [u8]) -> Res<(usize, bool)> {
        let data_recvd_state = matches!(self.state, RecvStreamState::DataRecvd { .. });
        match &mut self.state {
            RecvStreamState::Recv {
                recv_buf,
                fc,
                session_fc,
            }
            | RecvStreamState::SizeKnown {
                recv_buf,
                fc,
                session_fc,
                ..
            }
            | RecvStreamState::DataRecvd {
                recv_buf,
                fc,
                session_fc,
            } => {
                let bytes_read = recv_buf.read(buf);
                Self::flow_control_retire_data(u64::try_from(bytes_read)?, fc, session_fc);
                let fin_read = if data_recvd_state {
                    if recv_buf.buffered() == 0 {
                        let received = recv_buf.received();
                        let read = recv_buf.retired();
                        self.set_state(RecvStreamState::DataRead {
                            final_received: received,
                            final_read: read,
                        });
                        true
                    } else {
                        false
                    }
                } else {
                    false
                };
                Ok((bytes_read, fin_read))
            }
            RecvStreamState::DataRead { .. }
            | RecvStreamState::AbortReading { .. }
            | RecvStreamState::WaitForReset { .. }
            | RecvStreamState::ResetRecvd { .. } => Err(Error::NoMoreData),
        }
    }

    /// # Returns
    /// `true` if the stream transitions to `DataRead` (ended).
    /// `false` if the stream transitions to `AbortReading` or was already
    /// in a terminal or aborting state.
    #[must_use]
    pub fn stop_sending(&mut self, err: AppError) -> bool {
        qtrace!("stop_sending called when in state {}", self.state);
        match &mut self.state {
            RecvStreamState::Recv {
                fc,
                session_fc,
                recv_buf,
            }
            | RecvStreamState::SizeKnown {
                fc,
                session_fc,
                recv_buf,
            } => {
                Self::flow_control_retire_data(fc.consumed() - fc.retired(), fc, session_fc);
                let fc_copy = mem::take(fc);
                let session_fc_copy = mem::take(session_fc);
                let received = recv_buf.received();
                let read = recv_buf.retired();
                self.set_state(RecvStreamState::AbortReading {
                    fc: fc_copy,
                    session_fc: session_fc_copy,
                    final_size_reached: matches!(self.state, RecvStreamState::SizeKnown { .. }),
                    frame_needed: true,
                    err,
                    final_received: received,
                    final_read: read,
                });
                false
            }
            RecvStreamState::DataRecvd {
                fc,
                session_fc,
                recv_buf,
            } => {
                Self::flow_control_retire_data(fc.consumed() - fc.retired(), fc, session_fc);
                let received = recv_buf.received();
                let read = recv_buf.retired();
                self.set_state(RecvStreamState::DataRead {
                    final_received: received,
                    final_read: read,
                });
                true
            }
            RecvStreamState::DataRead { .. }
            | RecvStreamState::AbortReading { .. }
            | RecvStreamState::WaitForReset { .. }
            | RecvStreamState::ResetRecvd { .. } => false,
        }
    }

    /// Maybe write a `MAX_STREAM_DATA` frame.
    pub fn write_frame<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
        now: Instant,
        rtt: Duration,
    ) {
        match &mut self.state {
            RecvStreamState::Recv { fc, .. } => fc.write_frames(builder, tokens, stats, now, rtt),
            RecvStreamState::AbortReading {
                frame_needed, err, ..
            } if *frame_needed
                && builder.write_varint_frame(&[
                    FrameType::StopSending.into(),
                    self.stream_id.as_u64(),
                    *err,
                ]) =>
            {
                tokens.push(recovery::Token::Stream(StreamRecoveryToken::StopSending {
                    stream_id: self.stream_id,
                }));
                stats.stop_sending += 1;
                *frame_needed = false;
            }
            _ => {}
        }
    }

    pub const fn max_stream_data_lost(&mut self, maximum_data: u64) {
        if let RecvStreamState::Recv { fc, .. } = &mut self.state {
            fc.frame_lost(maximum_data);
        }
    }

    pub const fn stop_sending_lost(&mut self) {
        if let RecvStreamState::AbortReading { frame_needed, .. } = &mut self.state {
            *frame_needed = true;
        }
    }

    /// # Returns
    /// `true` if the stream transitions to `ResetRecvd` (ended) because
    /// the final size was already known.
    #[must_use]
    pub fn stop_sending_acked(&mut self) -> bool {
        if let RecvStreamState::AbortReading {
            fc,
            session_fc,
            final_size_reached,
            final_received,
            final_read,
            ..
        } = &mut self.state
        {
            let received = *final_received;
            let read = *final_read;
            if *final_size_reached {
                self.set_state(RecvStreamState::ResetRecvd {
                    final_received: received,
                    final_read: read,
                });
                return true;
            }
            let fc_copy = mem::take(fc);
            let session_fc_copy = mem::take(session_fc);
            self.set_state(RecvStreamState::WaitForReset {
                fc: fc_copy,
                session_fc: session_fc_copy,
                final_received: received,
                final_read: read,
            });
        }
        false
    }

#[cfg(any())]









    #[must_use]
    pub const fn has_frames_to_write(&self) -> bool {
        if let RecvStreamState::Recv { fc, .. } = &self.state {
            fc.frame_needed()
        } else {
            false
        }
    }

#[cfg(any())]









    #[must_use]
    pub const fn fc(&self) -> Option<&ReceiverFlowControl<StreamId>> {
        match &self.state {
            RecvStreamState::Recv { fc, .. }
            | RecvStreamState::SizeKnown { fc, .. }
            | RecvStreamState::DataRecvd { fc, .. }
            | RecvStreamState::AbortReading { fc, .. }
            | RecvStreamState::WaitForReset { fc, .. } => Some(fc),
            _ => None,
        }
    }
}
