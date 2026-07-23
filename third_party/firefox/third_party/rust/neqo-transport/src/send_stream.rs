// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    cell::RefCell,
    cmp::{Ordering, max, min},
    collections::{BTreeMap, VecDeque, btree_map::Entry},
    fmt::{self, Display, Formatter},
    mem,
    num::NonZeroUsize,
    ops::Add,
    rc::Rc,
};

use indexmap::IndexMap;
use neqo_common::{Buffer, Encoder, Role, qdebug, qerror, qtrace};
use smallvec::SmallVec;
use static_assertions::const_assert;

use crate::{
    AppError, Error, MAX_LOCAL_MAX_STREAM_DATA, Res,
    events::ConnectionEvents,
    fc::SenderFlowControl,
    frame::{Frame, FrameEncoder as _, FrameType},
    packet,
    recovery::{self, StreamRecoveryToken},
    stats::FrameStats,
    stream_id::StreamId,
    streams::SendOrder,
    tparams::{
        TransportParameterId::{InitialMaxStreamDataBidiRemote, InitialMaxStreamDataUni},
        TransportParameters,
    },
};

/// The priority that is assigned to sending data for the stream.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, PartialOrd, Ord)]
pub enum TransmissionPriority {
    /// This stream is more important than the functioning of the connection.
    /// Don't use this priority unless the stream really is that important.
    /// A stream at this priority can starve out other connection functions,
    /// including flow control, which could be very bad.
    Critical,
    /// The stream is very important.  Stream data will be written ahead of
    /// some of the less critical connection functions, like path validation,
    /// connection ID management, and session tickets.
    Important,
    /// High priority streams are important, but not enough to disrupt
    /// connection operation.  They go ahead of session tickets though.
    High,
    /// The default priority.
    #[default]
    Normal,
    /// Low priority streams get sent last.
    Low,
}

impl Add<RetransmissionPriority> for TransmissionPriority {
    type Output = Self;
    fn add(self, rhs: RetransmissionPriority) -> Self::Output {
        match rhs {
            RetransmissionPriority::Fixed(fixed) => fixed,
            RetransmissionPriority::Same => self,
            RetransmissionPriority::Higher => match self {
                Self::Critical => Self::Critical,
                Self::Important | Self::High => Self::Important,
                Self::Normal => Self::High,
                Self::Low => Self::Normal,
            },
            RetransmissionPriority::MuchHigher => match self {
                Self::Critical | Self::Important => Self::Critical,
                Self::High | Self::Normal => Self::Important,
                Self::Low => Self::High,
            },
        }
    }
}

/// If data is lost, this determines the priority that applies to retransmissions
/// of that data.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub enum RetransmissionPriority {
    /// Prioritize retransmission at a fixed priority.
    /// With this, it is possible to prioritize retransmissions lower than transmissions.
    /// Doing that can create a deadlock with flow control which might cause the connection
    /// to stall unless new data stops arriving fast enough that retransmissions can complete.
    Fixed(TransmissionPriority),
    /// Don't increase priority for retransmission.  This is probably not a good idea
    /// as it could mean starving flow control.
    Same,
    /// Increase the priority of retransmissions (the default).
    /// Retransmissions of `Critical` or `Important` aren't elevated at all.
    #[default]
    Higher,
    /// Increase the priority of retransmissions a lot.
    /// This is useful for streams that are particularly exposed to head-of-line blocking.
    MuchHigher,
}

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
enum RangeState {
    Sent,
    Acked,
}

/// Track ranges in the stream as sent or acked. Acked implies sent. Not in a
/// range implies needing-to-be-sent, either initially or as a retransmission.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct RangeTracker {
    /// The number of bytes that have been acknowledged starting from offset 0.
    acked: u64,
    /// A map that tracks the state of ranges.
    /// Keys are the offset of the start of the range.
    /// Values is a tuple of the range length and its state.
    used: BTreeMap<u64, (u64, RangeState)>,
    /// This is a cache for the output of `first_unmarked_range`, which we check a lot.
    first_unmarked: Option<(u64, Option<u64>)>,
}

impl RangeTracker {
    fn highest_offset(&self) -> u64 {
        self.used
            .last_key_value()
            .map_or(self.acked, |(&k, &(v, _))| k + v)
    }

    const fn acked_from_zero(&self) -> u64 {
        self.acked
    }

    /// Find the first unmarked range. If all are contiguous, this will return
    /// (`highest_offset()`, None).
    fn first_unmarked_range(&mut self) -> (u64, Option<u64>) {
        if let Some(first_unmarked) = self.first_unmarked {
            return first_unmarked;
        }

        let mut prev_end = self.acked;

        for (&cur_off, &(cur_len, _)) in &self.used {
            if prev_end == cur_off {
                prev_end = cur_off + cur_len;
            } else {
                let res = (prev_end, Some(cur_off - prev_end));
                self.first_unmarked = Some(res);
                return res;
            }
        }
        self.first_unmarked = Some((prev_end, None));
        (prev_end, None)
    }

    /// When the range of acknowledged bytes from zero increases, we need to drop any
    /// ranges within that span AND maybe extend it to include any adjacent acknowledged ranges.
    fn coalesce_acked(&mut self) {
        while let Some(e) = self.used.first_entry() {
            match self.acked.cmp(e.key()) {
                Ordering::Greater => {
                    let (off, (len, state)) = e.remove_entry();
                    let overflow = (off + len).saturating_sub(self.acked);
                    if overflow > 0 {
                        if state == RangeState::Acked {
                            self.acked += overflow;
                        } else {
                            self.used.insert(self.acked, (overflow, state));
                        }
                        break;
                    }
                }
                Ordering::Equal => {
                    if e.get().1 == RangeState::Acked {
                        let (len, _) = e.remove();
                        self.acked += len;
                    }
                    break;
                }
                Ordering::Less => break,
            }
        }
    }

    /// Mark a range as acknowledged.  This is simpler than marking a range as sent
    /// because an acknowledged range can never turn back into a sent range, so
    /// this function can just override the entire range.
    ///
    /// The only tricky parts are making sure that we maintain `self.acked`,
    /// which is the first acknowledged range.  And making sure that we don't create
    /// ranges of the same type that are adjacent; these need to be merged.
    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn mark_acked(&mut self, new_off: u64, new_len: usize) {
        let end = new_off + u64::try_from(new_len).expect("usize fits in u64");
        let new_off = max(self.acked, new_off);
        let mut new_len = end.saturating_sub(new_off);
        if new_len == 0 {
            return;
        }

        self.first_unmarked = None;
        if new_off == self.acked {
            self.acked += new_len;
            self.coalesce_acked();
            return;
        }
        let mut new_end = new_off + new_len;

        let mut covered = self
            .used
            .range(new_off..new_end)
            .map(|(&k, _)| k)
            .collect::<SmallVec<[_; 8]>>();

        if let Entry::Occupied(next_entry) = self.used.entry(new_end) {
            if next_entry.get().1 == RangeState::Acked {
                let (extra_len, _) = next_entry.remove();
                new_len += extra_len;
                new_end += extra_len;
            }
        } else if let Some(last) = covered.pop() {
            let (old_off, (old_len, old_state)) =
                self.used.remove_entry(&last).expect("entry exists"); 
            let remainder = (old_off + old_len).saturating_sub(new_end);
            if remainder > 0 {
                if old_state == RangeState::Acked {
                    new_len += remainder;
                    new_end += remainder;
                } else {
                    self.used.insert(new_end, (remainder, RangeState::Sent));
                }
            }
        }
        for k in covered {
            self.used.remove(&k);
        }

        let prev = self.used.range_mut(..new_off).next_back();
        if let Some((prev_off, (prev_len, prev_state))) = prev {
            let prev_end = *prev_off + *prev_len;
            if prev_end >= new_off {
                if *prev_state == RangeState::Sent {
                    *prev_len = new_off - *prev_off;
                    if prev_end > new_end {
                        self.used
                            .insert(new_end, (prev_end - new_end, RangeState::Sent));
                    }
                } else {
                    *prev_len = max(prev_end, new_end) - *prev_off;
                    return;
                }
            }
        }
        self.used.insert(new_off, (new_len, RangeState::Acked));
    }

    /// Turn a single sent range into a list of subranges that align with existing
    /// acknowledged ranges.
    ///
    /// This is more complicated than adding acked ranges because any acked ranges
    /// need to be kept in place, with sent ranges filling the gaps.
    ///
    /// This means:
    /// ```ignore
    ///   AAA S AAAS AAAAA
    /// +  SSSSSSSSSSSSS
    /// = AAASSSAAASSAAAAA
    /// ```
    ///
    /// But we also have to ensure that:
    /// ```ignore
    ///     SSSS
    /// + SS
    /// = SSSSSS
    /// ```
    /// and
    /// ```ignore
    ///   SSSSS
    /// +     SS
    /// = SSSSSS
    /// ```
    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn mark_sent(&mut self, mut new_off: u64, new_len: usize) {
        let new_end = new_off + u64::try_from(new_len).expect("usize fits in u64");
        new_off = max(self.acked, new_off);
        let mut new_len = new_end.saturating_sub(new_off);
        if new_len == 0 {
            return;
        }

        self.first_unmarked = None;

        let covered = self
            .used
            .range(new_off..(new_off + new_len))
            .map(|(&k, _)| k)
            .collect::<SmallVec<[u64; 8]>>();

        if let Entry::Occupied(next_entry) = self.used.entry(new_end)
            && next_entry.get().1 == RangeState::Sent
        {
            let (extra_len, _) = next_entry.remove();
            new_len += extra_len;
        }

        let prev = self.used.range(..new_off).next_back();
        if let Some((&prev_off, &(prev_len, prev_state))) = prev
            && prev_off + prev_len >= new_off
        {
            let overlap = prev_off + prev_len - new_off;
            new_len = new_len.saturating_sub(overlap);
            if new_len == 0 {
                return;
            }

            if prev_state == RangeState::Acked {
                new_off += overlap;
            } else {
                new_off = prev_off;
                new_len += prev_len;
            }
        }

        for old_off in covered {
            let Entry::Occupied(e) = self.used.entry(old_off) else {
                unreachable!();
            };
            let &(old_len, old_state) = e.get();
            if old_state == RangeState::Acked {
                let chunk_len = old_off - new_off;
                if chunk_len > 0 {
                    self.used.insert(new_off, (chunk_len, RangeState::Sent));
                }
                let included = chunk_len + old_len;
                new_len = new_len.saturating_sub(included);
                if new_len == 0 {
                    return;
                }
                new_off += included;
            } else {
                let overhang = (old_off + old_len).saturating_sub(new_off + new_len);
                new_len += overhang;
                if *e.key() != new_off {
                    e.remove();
                }
            }
        }

        self.used.insert(new_off, (new_len, RangeState::Sent));
    }

    fn unmark_range(&mut self, off: u64, len: usize) {
        if len == 0 {
            qdebug!("unmark 0-length range at {off}");
            return;
        }

        self.first_unmarked = None;
        let len = u64::try_from(len).expect("usize fits in u64");
        let end_off = off + len;

        let mut to_remove = SmallVec::<[_; 8]>::new();
        let mut to_add = None;

        for (cur_off, (cur_len, cur_state)) in self.used.range_mut(..off + len).rev() {
            if *cur_off < off {
                if *cur_off + *cur_len > off {
                    if *cur_state == RangeState::Acked {
                        qdebug!(
                            "Attempted to unmark Acked range {cur_off}-{cur_len} with unmark_range {off}-{}",
                            off + len
                        );
                    } else {
                        *cur_len = off - cur_off;
                    }
                }
                break;
            }

            if *cur_state == RangeState::Acked {
                qdebug!(
                    "Attempted to unmark Acked range {cur_off}-{cur_len} with unmark_range {off}-{}",
                    off + len
                );
                continue;
            }

            let cur_end_off = cur_off + *cur_len;
            if cur_end_off > end_off {
                let new_cur_off = off + len;
                let new_cur_len = cur_end_off - end_off;
                assert_eq!(to_add, None);
                to_add = Some((new_cur_off, new_cur_len, *cur_state));
            }

            to_remove.push(*cur_off);
        }

        for remove_off in to_remove {
            self.used.remove(&remove_off);
        }

        if let Some((new_cur_off, new_cur_len, cur_state)) = to_add {
            self.used.insert(new_cur_off, (new_cur_len, cur_state));
        }
    }

    /// Unmark all sent ranges.
    /// # Panics
    /// On 32-bit machines where far too much is sent before calling this.
    /// Note that this should not be called for handshakes, which should never exceed that limit.
    pub fn unmark_sent(&mut self) {
        self.unmark_range(
            0,
            usize::try_from(self.highest_offset()).expect("u64 fits in usize"),
        );
    }
}

/// Buffer to contain queued bytes and track their state.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct TxBuffer {
    send_buf: VecDeque<u8>, 
    ranges: RangeTracker,   
}

const_assert!(MAX_LOCAL_MAX_STREAM_DATA <= usize::MAX as u64);

impl TxBuffer {
    /// The maximum stream send buffer size.
    ///
    /// See [`MAX_LOCAL_MAX_STREAM_DATA`] for an explanation of this
    /// concrete value.
    #[expect(clippy::cast_possible_truncation, reason = "Checked by const_assert!")]
    pub const MAX_SIZE: usize = MAX_LOCAL_MAX_STREAM_DATA as usize;

    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Attempt to add some or all of the passed-in buffer to the `TxBuffer`.
    pub fn send(&mut self, buf: &[u8]) -> usize {
        let can_buffer = min(Self::MAX_SIZE - self.buffered(), buf.len());
        if can_buffer > 0 {
            self.send_buf.extend(&buf[..can_buffer]);
            debug_assert!(self.send_buf.len() <= Self::MAX_SIZE);
        }
        can_buffer
    }

    fn first_unmarked_range(&mut self) -> Option<(u64, Option<u64>)> {
        let (start, maybe_len) = self.ranges.first_unmarked_range();
        let buffered = u64::try_from(self.buffered()).ok()?;
        (start != self.retired() + buffered).then_some((start, maybe_len))
    }

    pub fn is_empty(&mut self) -> bool {
        self.first_unmarked_range().is_none()
    }

    pub fn next_bytes(&mut self) -> Option<(u64, &[u8])> {
        let (start, maybe_len) = self.first_unmarked_range()?;

        let buff_off = usize::try_from(start - self.retired()).ok()?;

        let slc = if buff_off < self.send_buf.as_slices().0.len() {
            &self.send_buf.as_slices().0[buff_off..]
        } else {
            &self.send_buf.as_slices().1[buff_off - self.send_buf.as_slices().0.len()..]
        };

        let len = maybe_len.map_or(slc.len(), |range_len| {
            min(usize::try_from(range_len).unwrap_or(usize::MAX), slc.len())
        });

        debug_assert!(len > 0);
        debug_assert!(len <= slc.len());

        Some((start, &slc[..len]))
    }

    pub fn mark_as_sent(&mut self, offset: u64, len: usize) {
        self.ranges.mark_sent(offset, len);
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn mark_as_acked(&mut self, offset: u64, len: usize) {
        let prev_retired = self.retired();
        self.ranges.mark_acked(offset, len);

        let new_retirable =
            usize::try_from(self.retired() - prev_retired).expect("u64 fits in usize");
        debug_assert!(new_retirable <= self.buffered());
        self.send_buf.drain(..new_retirable);
    }

    pub fn mark_as_lost(&mut self, offset: u64, len: usize) {
        self.ranges.unmark_range(offset, len);
    }

    /// Forget about anything that was marked as sent.
    pub fn unmark_sent(&mut self) {
        self.ranges.unmark_sent();
    }

    #[must_use]
    pub const fn retired(&self) -> u64 {
        self.ranges.acked_from_zero()
    }

    fn buffered(&self) -> usize {
        self.send_buf.len()
    }

    fn avail(&self) -> usize {
        Self::MAX_SIZE - self.buffered()
    }

    fn used(&self) -> u64 {
        self.retired() + u64::try_from(self.buffered()).expect("usize fits in u64")
    }
}

/// QUIC sending stream states, based on -transport 3.1.
#[derive(Debug)]
pub enum State {
    Ready {
        fc: SenderFlowControl<StreamId>,
        conn_fc: Rc<RefCell<SenderFlowControl<()>>>,
    },
    Send {
        fc: SenderFlowControl<StreamId>,
        conn_fc: Rc<RefCell<SenderFlowControl<()>>>,
        send_buf: TxBuffer,
    },
    DataSent {
        send_buf: TxBuffer,
        fin_sent: bool,
        fin_acked: bool,
    },
    DataRecvd {
        retired: u64,
        written: u64,
    },
    ResetSent {
        err: AppError,
        final_size: u64,
        priority: Option<TransmissionPriority>,
        final_retired: u64,
        final_written: u64,
    },
    ResetRecvd {
        final_retired: u64,
        final_written: u64,
    },
}

impl State {
    const fn tx_buf_mut(&mut self) -> Option<&mut TxBuffer> {
        match self {
            Self::Send { send_buf, .. } | Self::DataSent { send_buf, .. } => Some(send_buf),
            Self::Ready { .. }
            | Self::DataRecvd { .. }
            | Self::ResetSent { .. }
            | Self::ResetRecvd { .. } => None,
        }
    }

    fn tx_avail(&self) -> usize {
        match self {
            Self::Ready { .. } => TxBuffer::MAX_SIZE,
            Self::Send { send_buf, .. } | Self::DataSent { send_buf, .. } => send_buf.avail(),
            Self::DataRecvd { .. } | Self::ResetSent { .. } | Self::ResetRecvd { .. } => 0,
        }
    }

    fn transition(&mut self, new_state: Self) {
        qtrace!("SendStream state {self:?} -> {new_state:?}");
        *self = new_state;
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Stats {
    pub written: u64,
    pub sent: u64,
    pub acked: u64,
}

impl Stats {
    #[must_use]
    pub const fn new(written: u64, sent: u64, acked: u64) -> Self {
        Self {
            written,
            sent,
            acked,
        }
    }

    #[must_use]
    pub const fn bytes_written(&self) -> u64 {
        self.written
    }

    #[must_use]
    pub const fn bytes_sent(&self) -> u64 {
        self.sent
    }

    #[must_use]
    pub const fn bytes_acked(&self) -> u64 {
        self.acked
    }
}

/// Implement a QUIC send stream.
#[derive(Debug)]
pub struct SendStream {
    stream_id: StreamId,
    state: State,
    conn_events: ConnectionEvents,
    priority: TransmissionPriority,
    /// Cached result of `priority + retransmission`, recomputed in `set_priority`.
    effective_priority: TransmissionPriority,
    retransmission_offset: u64,
    sendorder: Option<SendOrder>,
    bytes_sent: u64,
    fair: bool,
    writable_event_low_watermark: NonZeroUsize,
}

impl SendStream {
    pub fn new(
        stream_id: StreamId,
        max_stream_data: u64,
        conn_fc: Rc<RefCell<SenderFlowControl<()>>>,
        conn_events: ConnectionEvents,
    ) -> Self {
        let ss = Self {
            stream_id,
            state: State::Ready {
                fc: SenderFlowControl::new(stream_id, max_stream_data),
                conn_fc,
            },
            conn_events,
            priority: TransmissionPriority::default(),
            effective_priority: TransmissionPriority::default() + RetransmissionPriority::default(),
            retransmission_offset: 0,
            sendorder: None,
            bytes_sent: 0,
            fair: false,
            writable_event_low_watermark: NonZeroUsize::MIN,
        };
        if ss.avail() > 0 {
            ss.conn_events.send_stream_writable(stream_id);
        }
        ss
    }

    pub fn write_frames<B: Buffer>(
        &mut self,
        priority: TransmissionPriority,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) -> bool {
        if !self.write_reset_frame(priority, builder, tokens, stats) {
            self.write_blocked_frame(priority, builder, tokens, stats);
            if builder.is_full() {
                return false;
            }
            self.write_stream_frame(priority, builder, tokens, stats);
            if builder.is_full() {
                return false;
            }
        }
        true
    }

    pub const fn set_fairness(&mut self, make_fair: bool) {
        self.fair = make_fair;
    }

    #[must_use]
    pub const fn is_fair(&self) -> bool {
        self.fair
    }

    pub fn set_priority(
        &mut self,
        transmission: TransmissionPriority,
        retransmission: RetransmissionPriority,
    ) {
        self.priority = transmission;
        self.effective_priority = transmission + retransmission;
    }

    #[must_use]
    pub const fn sendorder(&self) -> Option<SendOrder> {
        self.sendorder
    }

    pub const fn set_sendorder(&mut self, sendorder: Option<SendOrder>) {
        self.sendorder = sendorder;
    }

    /// If all data has been buffered or written, how much was sent.
    #[must_use]
    pub fn final_size(&self) -> Option<u64> {
        match &self.state {
            State::DataSent { send_buf, .. } => Some(send_buf.used()),
            State::ResetSent { final_size, .. } => Some(*final_size),
            _ => None,
        }
    }

    #[must_use]
    pub fn stats(&self) -> Stats {
        Stats::new(self.bytes_written(), self.bytes_sent, self.bytes_acked())
    }

    #[must_use]
    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn bytes_written(&self) -> u64 {
        match &self.state {
            State::Send { send_buf, .. } | State::DataSent { send_buf, .. } => {
                send_buf.retired() + u64::try_from(send_buf.buffered()).expect("usize fits in u64")
            }
            State::DataRecvd {
                retired, written, ..
            } => *retired + *written,
            State::ResetSent {
                final_retired,
                final_written,
                ..
            }
            | State::ResetRecvd {
                final_retired,
                final_written,
                ..
            } => *final_retired + *final_written,
            State::Ready { .. } => 0,
        }
    }

    #[must_use]
    pub const fn bytes_acked(&self) -> u64 {
        match &self.state {
            State::Send { send_buf, .. } | State::DataSent { send_buf, .. } => send_buf.retired(),
            State::DataRecvd { retired, .. } => *retired,
            State::ResetSent { final_retired, .. } | State::ResetRecvd { final_retired, .. } => {
                *final_retired
            }
            State::Ready { .. } => 0,
        }
    }

    /// Return the next range to be sent, if any.
    /// If this is a retransmission, cut off what is sent at the retransmission
    /// offset.
    fn next_bytes(&mut self, retransmission_only: bool) -> Option<(u64, &[u8])> {
        match self.state {
            State::Send {
                ref mut send_buf, ..
            } => {
                let (offset, slice) = send_buf.next_bytes()?;
                if retransmission_only {
                    qtrace!(
                        "next_bytes apply retransmission limit at {}",
                        self.retransmission_offset
                    );
                    (self.retransmission_offset > offset).then(|| {
                        let Ok(delta) = usize::try_from(self.retransmission_offset - offset) else {
                            return None;
                        };
                        let len = min(delta, slice.len());
                        Some((offset, &slice[..len]))
                    })?
                } else {
                    Some((offset, slice))
                }
            }
            State::DataSent {
                ref mut send_buf,
                fin_sent,
                ..
            } => {
                let used = send_buf.used(); 
                let bytes = send_buf.next_bytes();
                if bytes.is_some() {
                    bytes
                } else if fin_sent {
                    None
                } else {
                    Some((used, &[]))
                }
            }
            State::Ready { .. }
            | State::DataRecvd { .. }
            | State::ResetSent { .. }
            | State::ResetRecvd { .. } => None,
        }
    }

    /// Calculate how many bytes (length) can fit into available space and whether
    /// the remainder of the space can be filled (or if a length field is needed).
    fn length_and_fill(data_len: usize, space: usize) -> (usize, bool) {
        if data_len >= space {
            qtrace!("SendStream::length_and_fill fill {space}");
            return (space, true);
        }

        let length = min(space.saturating_sub(1), data_len);
        let length_len = Encoder::varint_len(u64::try_from(length).expect("usize fits in u64"));
        debug_assert!(length_len <= space); 

        let fill = data_len + length_len + packet::Builder::MINIMUM_FRAME_SIZE > space;
        qtrace!("SendStream::length_and_fill {data_len} fill {fill}");
        (data_len, fill)
    }

    /// Maybe write a `STREAM` frame.
    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn write_stream_frame<B: Buffer>(
        &mut self,
        priority: TransmissionPriority,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        let retransmission = if priority == self.priority {
            false
        } else if priority == self.effective_priority {
            true
        } else {
            return;
        };

        let id = self.stream_id;
        let final_size = self.final_size();
        if let Some((offset, data)) = self.next_bytes(retransmission) {
            let overhead = 1 
                + Encoder::varint_len(id.as_u64())
                + if offset > 0 {
                    Encoder::varint_len(offset)
                } else {
                    0
                };
            if overhead > builder.remaining() {
                qtrace!("[{self}] write_frame no space for header");
                return;
            }

            let (length, fill) = Self::length_and_fill(data.len(), builder.remaining() - overhead);
            let fin = final_size
                .is_some_and(|fs| fs == offset + u64::try_from(length).expect("usize fits in u64"));
            if length == 0 && !fin {
                qtrace!("[{self}] write_frame no data, no fin");
                return;
            }

            let frame_type = Frame::stream_type(fin, offset > 0, fill);
            builder.encode_frame(frame_type, |b| {
                b.encode_varint(id.as_u64());
                if offset > 0 {
                    b.encode_varint(offset);
                }
                if fill {
                    b.encode(&data[..length]);
                } else {
                    b.encode_vvec(&data[..length]);
                }
            });
            if fill {
                builder.mark_full();
            }
            debug_assert!(builder.len() <= builder.limit());

            self.mark_as_sent(offset, length, fin);
            tokens.push(recovery::Token::Stream(StreamRecoveryToken::Stream(
                RecoveryToken {
                    id,
                    offset,
                    length,
                    fin,
                },
            )));
            stats.stream += 1;
        }
    }

    pub fn reset_acked(&mut self) {
        match self.state {
            State::Ready { .. }
            | State::Send { .. }
            | State::DataSent { .. }
            | State::DataRecvd { .. } => {
                qtrace!("[{self}] Reset acked while in {:?} state?", self.state);
            }
            State::ResetSent {
                final_retired,
                final_written,
                ..
            } => self.state.transition(State::ResetRecvd {
                final_retired,
                final_written,
            }),
            State::ResetRecvd { .. } => qtrace!("[{self}] already in ResetRecvd state"),
        }
    }

    pub fn reset_lost(&mut self) {
        match self.state {
            State::ResetSent {
                ref mut priority, ..
            } => {
                *priority = Some(self.effective_priority);
            }
            State::ResetRecvd { .. } => (),
            _ => unreachable!(),
        }
    }

    /// Maybe write a `RESET_STREAM` frame.
    pub fn write_reset_frame<B: Buffer>(
        &mut self,
        p: TransmissionPriority,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) -> bool {
        if let State::ResetSent {
            final_size,
            err,
            ref mut priority,
            ..
        } = self.state
        {
            if *priority != Some(p) {
                return false;
            }
            if builder.write_varint_frame(&[
                FrameType::ResetStream.into(),
                self.stream_id.as_u64(),
                err,
                final_size,
            ]) {
                tokens.push(recovery::Token::Stream(StreamRecoveryToken::ResetStream {
                    stream_id: self.stream_id,
                }));
                stats.reset_stream += 1;
                *priority = None;
                true
            } else {
                false
            }
        } else {
            false
        }
    }

    pub fn blocked_lost(&mut self, limit: u64) {
        if let State::Ready { fc, .. } | State::Send { fc, .. } = &mut self.state {
            fc.frame_lost(limit);
        } else {
            qtrace!("[{self}] Ignoring lost STREAM_DATA_BLOCKED({limit})");
        }
    }

    /// Maybe write a `STREAM_DATA_BLOCKED` frame.
    pub fn write_blocked_frame<B: Buffer>(
        &mut self,
        priority: TransmissionPriority,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        if priority == self.priority
            && let State::Ready { fc, .. } | State::Send { fc, .. } = &mut self.state
        {
            fc.write_frames(builder, tokens, stats);
        }
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn mark_as_sent(&mut self, offset: u64, len: usize, fin: bool) {
        self.bytes_sent = max(
            self.bytes_sent,
            offset + u64::try_from(len).expect("usize fits in u64"),
        );

        if let Some(buf) = self.state.tx_buf_mut() {
            buf.mark_as_sent(offset, len);
            self.send_blocked_if_space_needed(0);
        }

        if fin && let State::DataSent { fin_sent, .. } = &mut self.state {
            *fin_sent = true;
        }
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn mark_as_acked(&mut self, offset: u64, len: usize, fin: bool) {
        match self.state {
            State::Send {
                ref mut send_buf, ..
            } => {
                let previous_limit = send_buf.avail();
                send_buf.mark_as_acked(offset, len);
                let current_limit = send_buf.avail();
                self.maybe_emit_writable_event(previous_limit, current_limit);
            }
            State::DataSent {
                ref mut send_buf,
                ref mut fin_acked,
                ..
            } => {
                send_buf.mark_as_acked(offset, len);
                if fin {
                    *fin_acked = true;
                }
                if *fin_acked && send_buf.buffered() == 0 {
                    self.conn_events.send_stream_complete(self.stream_id);
                    let retired = send_buf.retired();
                    let buffered = u64::try_from(send_buf.buffered()).expect("usize fits in u64");
                    self.state.transition(State::DataRecvd {
                        retired,
                        written: buffered,
                    });
                }
            }
            _ => qtrace!("[{self}] mark_as_acked called from state {:?}", self.state),
        }
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn mark_as_lost(&mut self, offset: u64, len: usize, fin: bool) {
        self.retransmission_offset = max(
            self.retransmission_offset,
            offset + u64::try_from(len).expect("usize fits in u64"),
        );
        qtrace!(
            "[{self}] mark_as_lost retransmission offset={}",
            self.retransmission_offset
        );
        if let Some(buf) = self.state.tx_buf_mut() {
            buf.mark_as_lost(offset, len);
        }

        if fin
            && let State::DataSent {
                fin_sent,
                fin_acked,
                ..
            } = &mut self.state
        {
            *fin_sent = *fin_acked;
        }
    }

    /// Bytes sendable on stream. Constrained by stream credit available,
    /// connection credit available, and space in the tx buffer.
    #[must_use]
    pub fn avail(&self) -> usize {
        if let State::Ready { fc, conn_fc } | State::Send { fc, conn_fc, .. } = &self.state {
            min(
                min(fc.available(), conn_fc.borrow().available()),
                self.state.tx_avail(),
            )
        } else {
            0
        }
    }

    /// Set low watermark for [`crate::ConnectionEvent::SendStreamWritable`]
    /// event.
    ///
    /// See [`crate::Connection::stream_set_writable_event_low_watermark`].
    pub const fn set_writable_event_low_watermark(&mut self, watermark: NonZeroUsize) {
        self.writable_event_low_watermark = watermark;
    }

    pub fn set_max_stream_data(&mut self, limit: u64) {
        qdebug!("setting max_stream_data to {limit}");
        if let State::Ready { fc, .. } | State::Send { fc, .. } = &mut self.state {
            let previous_limit = fc.available();
            if let Some(current_limit) = fc.update(limit) {
                self.maybe_emit_writable_event(previous_limit, current_limit);
            }
        }
    }

    #[must_use]
    pub const fn is_ended(&self) -> bool {
        matches!(
            self.state,
            State::DataRecvd { .. } | State::ResetRecvd { .. }
        )
    }

    /// # Errors
    /// When `buf` is empty or when the stream is already closed.
    pub fn send(&mut self, buf: &[u8]) -> Res<usize> {
        self.send_internal(buf, false)
    }

    /// # Errors
    /// When `buf` is empty or when the stream is already closed.
    pub fn send_atomic(&mut self, buf: &[u8]) -> Res<usize> {
        self.send_internal(buf, true)
    }

    fn send_blocked_if_space_needed(&mut self, needed_space: usize) {
        if let State::Ready { fc, conn_fc } | State::Send { fc, conn_fc, .. } = &mut self.state {
            if fc.available() <= needed_space {
                fc.blocked();
            }

            if conn_fc.borrow().available() <= needed_space {
                conn_fc.borrow_mut().blocked();
            }
        }
    }

    fn send_internal(&mut self, buf: &[u8], atomic: bool) -> Res<usize> {
        if buf.is_empty() {
            qerror!("[{self}] zero-length send on stream");
            return Err(Error::InvalidInput);
        }

        if let State::Ready { fc, conn_fc } = &mut self.state {
            let owned_fc = mem::replace(fc, SenderFlowControl::new(self.stream_id, 0));
            let owned_conn_fc = Rc::clone(conn_fc);
            self.state.transition(State::Send {
                fc: owned_fc,
                conn_fc: owned_conn_fc,
                send_buf: TxBuffer::new(),
            });
        }

        if !matches!(self.state, State::Send { .. }) {
            return Err(Error::FinalSize);
        }

        let buf = if self.avail() == 0 {
            return Ok(0);
        } else if self.avail() < buf.len() {
            if atomic {
                self.send_blocked_if_space_needed(buf.len());
                return Ok(0);
            }

            &buf[..self.avail()]
        } else {
            buf
        };

        match &mut self.state {
            State::Ready { .. } => unreachable!(),
            State::Send {
                fc,
                conn_fc,
                send_buf,
            } => {
                let sent = send_buf.send(buf);
                fc.consume(sent);
                conn_fc.borrow_mut().consume(sent);
                Ok(sent)
            }
            _ => Err(Error::FinalSize),
        }
    }

    pub fn close(&mut self) {
        match &mut self.state {
            State::Ready { .. } => {
                self.state.transition(State::DataSent {
                    send_buf: TxBuffer::new(),
                    fin_sent: false,
                    fin_acked: false,
                });
            }
            State::Send { send_buf, .. } => {
                let owned_buf = mem::replace(send_buf, TxBuffer::new());
                self.state.transition(State::DataSent {
                    send_buf: owned_buf,
                    fin_sent: false,
                    fin_acked: false,
                });
            }
            State::DataSent { .. } => qtrace!("[{self}] already in DataSent state"),
            State::DataRecvd { .. } => qtrace!("[{self}] already in DataRecvd state"),
            State::ResetSent { .. } => qtrace!("[{self}] already in ResetSent state"),
            State::ResetRecvd { .. } => qtrace!("[{self}] already in ResetRecvd state"),
        }
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn reset(&mut self, err: AppError) {
        match &self.state {
            State::Ready { fc, .. } => {
                let final_size = fc.used();
                self.state.transition(State::ResetSent {
                    err,
                    final_size,
                    priority: Some(self.priority),
                    final_retired: 0,
                    final_written: 0,
                });
            }
            State::Send { fc, send_buf, .. } => {
                let final_size = fc.used();
                let final_retired = send_buf.retired();
                let buffered = u64::try_from(send_buf.buffered()).expect("usize fits in u64");
                self.state.transition(State::ResetSent {
                    err,
                    final_size,
                    priority: Some(self.priority),
                    final_retired,
                    final_written: buffered,
                });
            }
            State::DataSent { send_buf, .. } => {
                let final_size = send_buf.used();
                let final_retired = send_buf.retired();
                let buffered = u64::try_from(send_buf.buffered()).expect("usize fits in u64");
                self.state.transition(State::ResetSent {
                    err,
                    final_size,
                    priority: Some(self.priority),
                    final_retired,
                    final_written: buffered,
                });
            }
            State::DataRecvd { .. } => qtrace!("[{self}] already in DataRecvd state"),
            State::ResetSent { .. } => qtrace!("[{self}] already in ResetSent state"),
            State::ResetRecvd { .. } => qtrace!("[{self}] already in ResetRecvd state"),
        }
    }


    pub(crate) fn maybe_emit_writable_event(&self, previous_limit: usize, current_limit: usize) {
        let low_watermark = self.writable_event_low_watermark.get();

        if low_watermark < previous_limit
            || current_limit < low_watermark
            || self.avail() < low_watermark
        {
            return;
        }

        self.conn_events.send_stream_writable(self.stream_id);
    }
}

impl Display for SendStream {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "SendStream {}", self.stream_id)
    }
}

#[derive(Debug, Default)]
pub struct OrderGroup {
    vec: Vec<StreamId>,

    next: usize,
}

pub struct OrderGroupIter<'a> {
    group: &'a mut OrderGroup,
    started_at: Option<usize>,
}

impl OrderGroup {
    pub const fn iter(&mut self) -> OrderGroupIter<'_> {
        if self.next >= self.vec.len() {
            self.next = 0;
        }
        OrderGroupIter {
            started_at: None,
            group: self,
        }
    }

    #[must_use]
    pub fn stream_ids(&self) -> &[StreamId] {
        &self.vec
    }

    pub fn clear(&mut self) {
        self.vec.clear();
    }

    pub fn push(&mut self, stream_id: StreamId) {
        self.vec.push(stream_id);
    }


    const fn update_next(&mut self) -> usize {
        let next = self.next;
        self.next = (self.next + 1) % self.vec.len();
        next
    }

    /// # Panics
    /// If the stream ID is already present.
    pub fn insert(&mut self, stream_id: StreamId) {
        let Err(pos) = self.vec.binary_search(&stream_id) else {
            panic!("Duplicate stream_id {stream_id}");
        };
        self.vec.insert(pos, stream_id);
    }

    /// # Panics
    /// If the stream ID is not present.
    pub fn remove(&mut self, stream_id: StreamId) {
        let Ok(pos) = self.vec.binary_search(&stream_id) else {
            panic!("Missing stream_id {stream_id}");
        };
        self.vec.remove(pos);
    }
}

impl Iterator for OrderGroupIter<'_> {
    type Item = StreamId;
    fn next(&mut self) -> Option<Self::Item> {
        if self.started_at == Some(self.group.next) || self.group.vec.is_empty() {
            return None;
        }
        self.started_at = self.started_at.or(Some(self.group.next));
        let orig = self.group.update_next();
        Some(self.group.vec[orig])
    }
}

#[derive(Debug, Default)]
pub struct SendStreams {
    map: IndexMap<StreamId, SendStream>,



    sendordered: BTreeMap<SendOrder, OrderGroup>,
    regular: OrderGroup, 
    /// Set when any stream has ended; cleared by `remove_ended`.
    has_ended: bool,
}

impl SendStreams {
    #[allow(
        clippy::allow_attributes,
        clippy::missing_errors_doc,
        reason = "OK here."
    )]
    pub fn get(&self, id: StreamId) -> Res<&SendStream> {
        self.map.get(&id).ok_or(Error::InvalidStreamId)
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_errors_doc,
        reason = "OK here."
    )]
    pub fn get_mut(&mut self, id: StreamId) -> Res<&mut SendStream> {
        self.map.get_mut(&id).ok_or(Error::InvalidStreamId)
    }

    #[must_use]
    pub fn exists(&self, id: StreamId) -> bool {
        self.map.contains_key(&id)
    }

    pub fn insert(&mut self, id: StreamId, stream: SendStream) {
        self.map.insert(id, stream);
    }

    fn group_mut(&mut self, sendorder: Option<SendOrder>) -> &mut OrderGroup {
        if let Some(order) = sendorder {
            self.sendordered.entry(order).or_default()
        } else {
            &mut self.regular
        }
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_errors_doc,
        reason = "OK here."
    )]
    pub fn set_sendorder(&mut self, stream_id: StreamId, sendorder: Option<SendOrder>) -> Res<()> {
        self.set_fairness(stream_id, true)?;
        if let Some(stream) = self.map.get_mut(&stream_id) {
            let old_sendorder = stream.sendorder();
            if old_sendorder != sendorder {
                let mut group = self.group_mut(old_sendorder);
                group.remove(stream_id);
                self.get_mut(stream_id)?.set_sendorder(sendorder);
                group = self.group_mut(sendorder);
                group.insert(stream_id);
                qtrace!(
                    "ordering of stream_ids: {:?}",
                    self.sendordered.values().collect::<Vec::<_>>()
                );
            }
            Ok(())
        } else {
            Err(Error::InvalidStreamId)
        }
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_errors_doc,
        reason = "OK here."
    )]
    pub fn set_fairness(&mut self, stream_id: StreamId, make_fair: bool) -> Res<()> {
        let stream: &mut SendStream = self.map.get_mut(&stream_id).ok_or(Error::InvalidStreamId)?;
        let was_fair = stream.fair;
        stream.set_fairness(make_fair);
        if !was_fair && make_fair {



            if matches!(self.regular.stream_ids().last(), Some(last) if stream_id > *last) {
                self.regular.push(stream_id);
            } else {
                self.regular.insert(stream_id);
            }
        } else if was_fair && !make_fair {
            let group = if let Some(sendorder) = stream.sendorder {
                self.sendordered
                    .get_mut(&sendorder)
                    .ok_or(Error::Internal)?
            } else {
                &mut self.regular
            };
            group.remove(stream_id);
        }
        Ok(())
    }

    pub fn acked(&mut self, token: &RecoveryToken) {
        if let Some(ss) = self.map.get_mut(&token.id) {
            ss.mark_as_acked(token.offset, token.length, token.fin);
            self.has_ended |= ss.is_ended();
        }
    }

    pub fn reset_acked(&mut self, id: StreamId) {
        if let Some(ss) = self.map.get_mut(&id) {
            ss.reset_acked();
            self.has_ended |= ss.is_ended();
        }
    }

    pub fn lost(&mut self, token: &RecoveryToken) {
        if let Some(ss) = self.map.get_mut(&token.id) {
            ss.mark_as_lost(token.offset, token.length, token.fin);
        }
    }

    pub fn reset_lost(&mut self, stream_id: StreamId) {
        if let Some(ss) = self.map.get_mut(&stream_id) {
            ss.reset_lost();
        }
    }

    pub fn blocked_lost(&mut self, stream_id: StreamId, limit: u64) {
        if let Some(ss) = self.map.get_mut(&stream_id) {
            ss.blocked_lost(limit);
        }
    }

    pub fn clear(&mut self) {
        self.map.clear();
        self.sendordered.clear();
        self.regular.clear();
        self.has_ended = false;
    }

    /// Remove ended streams. Returns `true` if any were removed.
    #[must_use]
    pub fn remove_ended(&mut self) -> bool {
        if !self.has_ended {
            return false;
        }
        self.has_ended = false;
        let mut removed = false;
        for (stream_id, stream) in self
            .map
            .extract_if(.., |_, stream: &mut SendStream| stream.is_ended())
        {
            removed = true;
            if stream.is_fair() {
                match stream.sendorder() {
                    None => self.regular.remove(stream_id),
                    Some(sendorder) => {
                        if let Some(group) = self.sendordered.get_mut(&sendorder) {
                            group.remove(stream_id);
                        }
                    }
                }
            }
        }
        removed
    }

    pub(crate) fn write_frames<B: Buffer>(
        &mut self,
        priority: TransmissionPriority,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {




        qtrace!("processing streams...  unfair:");
        for stream in self.map.values_mut() {
            if !stream.is_fair() {
                qtrace!("   {stream}");
                if !stream.write_frames(priority, builder, tokens, stats) {
                    break;
                }
            }
        }
        qtrace!("fair streams:");
        let stream_ids = self.regular.iter().chain(
            self.sendordered
                .values_mut()
                .rev()
                .flat_map(|group| group.iter()),
        );
        for stream_id in stream_ids {
            if let Some(stream) = self.map.get_mut(&stream_id) {
                if let Some(order) = stream.sendorder() {
                    qtrace!("   {stream_id} ({order})");
                } else {
                    qtrace!("   None");
                }
                if !stream.write_frames(priority, builder, tokens, stats) {
                    break;
                }
            }
        }
    }

    #[allow(
        clippy::allow_attributes,
        clippy::missing_panics_doc,
        reason = "OK here."
    )]
    pub fn update_initial_limit(&mut self, remote: &TransportParameters) {
        for (id, ss) in &mut self.map {
            let limit = if id.is_bidi() {
                assert!(!id.is_remote_initiated(Role::Client));
                remote.get_integer(InitialMaxStreamDataBidiRemote)
            } else {
                remote.get_integer(InitialMaxStreamDataUni)
            };
            ss.set_max_stream_data(limit);
        }
    }
}

#[allow(
    clippy::allow_attributes,
    clippy::into_iter_without_iter,
    reason = "OK here."
)]
impl<'a> IntoIterator for &'a mut SendStreams {
    type Item = (&'a StreamId, &'a mut SendStream);
    type IntoIter = indexmap::map::IterMut<'a, StreamId, SendStream>;

    fn into_iter(self) -> indexmap::map::IterMut<'a, StreamId, SendStream> {
        self.map.iter_mut()
    }
}

#[derive(Debug, Clone)]
pub struct RecoveryToken {
    id: StreamId,
    offset: u64,
    length: usize,
    fin: bool,
}
