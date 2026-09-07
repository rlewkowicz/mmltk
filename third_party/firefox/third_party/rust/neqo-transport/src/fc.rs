// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    cmp::min,
    fmt::{Debug, Display},
    num::NonZeroU64,
    ops::{Deref, DerefMut, Index, IndexMut},
    time::{Duration, Instant},
};

use enum_map::EnumMap;
use neqo_common::{Buffer, MAX_VARINT, Role, qdebug, qtrace};

use crate::{
    Error, Res,
    connection::params::{MAX_LOCAL_MAX_DATA, MAX_LOCAL_MAX_STREAM_DATA},
    frame::FrameType,
    packet,
    recovery::{self, StreamRecoveryToken},
    stats::FrameStats,
    stream_id::{StreamId, StreamType},
};

/// Fraction of a flow control window after which a receiver sends a window
/// update.
///
/// In steady-state and max utilization, a value of 4 leads to 4 window updates
/// per RTT.
///
/// Value aligns with [`crate::connection::params::ConnectionParameters::DEFAULT_ACK_RATIO`].
pub const WINDOW_UPDATE_FRACTION: u64 = 4;

/// Multiplier for auto-tuning the stream receive window.
///
/// See [`ReceiverFlowControl::auto_tune`].
///
/// Note that the flow control window should grow at least as fast as the
/// congestion control window, in order to not unnecessarily limit throughput.
const WINDOW_INCREASE_MULTIPLIER: u64 = 4;

/// Subject for flow control auto-tuning, used to avoid heap allocations
/// when logging.
#[derive(Debug, Clone, Copy)]
enum AutoTuneSubject {
    Connection,
    Stream(StreamId),
}

impl Display for AutoTuneSubject {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Connection => write!(f, "connection"),
            Self::Stream(id) => write!(f, "stream {id}"),
        }
    }
}

#[derive(Debug)]
pub struct SenderFlowControl<T>
where
    T: Debug + Sized,
{
    /// The thing that we're counting for.
    subject: T,
    /// The limit.
    limit: u64,
    /// How much of that limit we've used.
    used: u64,
    /// The limit at which blocking was last reported, or `None` if never blocked.
    /// Updated each time the sender decides it is blocked, ensuring that blocking
    /// at any given limit is only reported once.
    blocked_at: Option<u64>,
    /// Whether a blocked frame should be sent.
    blocked_frame: bool,
}

impl<T> SenderFlowControl<T>
where
    T: Debug + Sized,
{
    /// Make a new instance with the initial value and subject.
    pub const fn new(subject: T, initial: u64) -> Self {
        Self {
            subject,
            limit: initial,
            used: 0,
            blocked_at: None,
            blocked_frame: false,
        }
    }

    /// Update the maximum. Returns `Some` with the updated available flow
    /// control if the change was an increase and `None` otherwise.
    pub fn update(&mut self, limit: u64) -> Option<usize> {
        (limit > self.limit).then(|| {
            self.limit = limit;
            self.blocked_frame = false;
            self.available()
        })
    }

    /// Consume flow control.
    pub fn consume(&mut self, count: usize) {
        let amt = u64::try_from(count).expect("usize fits into u64");
        debug_assert!(self.used + amt <= self.limit);
        self.used += amt;
    }

    /// Get available flow control.
    pub fn available(&self) -> usize {
        usize::try_from(self.limit - self.used).unwrap_or(usize::MAX)
    }

    /// How much data has been written.
    pub const fn used(&self) -> u64 {
        self.used
    }

    /// Mark flow control as blocked.
    /// This only does something if the current limit exceeds the last reported blocking limit.
    pub const fn blocked(&mut self) {
        if let Some(block) = self.blocked_at
            && self.limit <= block
        {
            return;
        }
        self.blocked_at = Some(self.limit);
        self.blocked_frame = true;
    }

    /// Return whether a blocking frame needs to be sent.
    /// This is `Some` with the active limit if `blocked` has been called,
    /// if a blocking frame has not been sent (or it has been lost), and
    /// if the blocking condition remains.
    fn blocked_needed(&self) -> Option<u64> {
        self.blocked_at
            .filter(|&l| self.blocked_frame && self.limit <= l)
    }

    /// Clear the need to send a blocked frame.
    const fn blocked_sent(&mut self) {
        self.blocked_frame = false;
    }

    /// Mark a blocked frame as having been lost.
    /// Only send again if value of `self.blocked_at` hasn't increased since sending.
    /// That would imply that the limit has since increased.
    pub const fn frame_lost(&mut self, limit: u64) {
        if let Some(block) = self.blocked_at
            && block == limit
        {
            self.blocked_frame = true;
        }
    }
}

impl SenderFlowControl<()> {
    pub fn write_frames<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        if let Some(limit) = self.blocked_needed()
            && builder.write_varint_frame(&[FrameType::DataBlocked.into(), limit])
        {
            stats.data_blocked += 1;
            tokens.push(recovery::Token::Stream(StreamRecoveryToken::DataBlocked(
                limit,
            )));
            self.blocked_sent();
        }
    }
}

impl SenderFlowControl<StreamId> {
    pub fn write_frames<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        if let Some(limit) = self.blocked_needed()
            && builder.write_varint_frame(&[
                FrameType::StreamDataBlocked.into(),
                self.subject.as_u64(),
                limit,
            ])
        {
            stats.stream_data_blocked += 1;
            tokens.push(recovery::Token::Stream(
                StreamRecoveryToken::StreamDataBlocked {
                    stream_id: self.subject,
                    limit,
                },
            ));
            self.blocked_sent();
        }
    }
}

impl SenderFlowControl<StreamType> {
    pub fn write_frames<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        if let Some(limit) = self.blocked_needed() {
            let frame = match self.subject {
                StreamType::BiDi => FrameType::StreamsBlockedBiDi,
                StreamType::UniDi => FrameType::StreamsBlockedUniDi,
            };
            if builder.write_varint_frame(&[frame.into(), limit]) {
                stats.streams_blocked += 1;
                tokens.push(recovery::Token::Stream(
                    StreamRecoveryToken::StreamsBlocked {
                        stream_type: self.subject,
                        limit,
                    },
                ));
                self.blocked_sent();
            }
        }
    }
}

#[derive(Debug, Default)]
pub struct ReceiverFlowControl<T>
where
    T: Debug + Sized,
{
    /// The thing that we're counting for.
    subject: T,
    /// The maximum amount of items that can be active (e.g., the size of the receive buffer).
    max_active: u64,
    /// Last max allowed sent.
    max_allowed: u64,
    /// Last time a flow control update was sent.
    ///
    /// Used by auto-tuning logic to estimate sending rate between updates.
    /// This is active for both stream-level
    /// ([`ReceiverFlowControl<StreamId>`]) and connection-level
    /// ([`ReceiverFlowControl<()>`]) flow control.
    last_update: Option<Instant>,
    /// Item received, but not retired yet.
    /// This will be used for byte flow control: each stream will remember its largest byte
    /// offset received and session flow control will remember the sum of all bytes consumed
    /// by all streams.
    consumed: u64,
    /// Retired items.
    retired: u64,
    frame_pending: bool,
}

impl<T> ReceiverFlowControl<T>
where
    T: Debug + Sized,
{
    /// Make a new instance with the initial value and subject.
    pub const fn new(subject: T, max: u64) -> Self {
        Self {
            subject,
            max_active: max,
            max_allowed: max,
            last_update: None,
            consumed: 0,
            retired: 0,
            frame_pending: false,
        }
    }

    /// Retire some items and maybe send flow control
    /// update.
    pub const fn retire(&mut self, retired: u64) {
        if retired <= self.retired {
            return;
        }

        self.retired = retired;
        if self.should_send_update() {
            self.frame_pending = true;
        }
    }

    /// This function is called when `STREAM_DATA_BLOCKED` frame is received.
    /// The flow control will try to send an update if possible.
    pub const fn send_flowc_update(&mut self) {
        if self.retired + self.max_active > self.max_allowed {
            self.frame_pending = true;
        }
    }

    const fn should_send_update(&self) -> bool {
        let window_bytes_unused = self.max_allowed - self.retired;
        window_bytes_unused < self.max_active - self.max_active / WINDOW_UPDATE_FRACTION
    }

    pub const fn frame_needed(&self) -> bool {
        self.frame_pending
    }

    pub fn next_limit(&self) -> u64 {
        min(
            self.retired + self.max_active,
            MAX_VARINT,
        )
    }

    pub const fn max_active(&self) -> u64 {
        self.max_active
    }

    pub const fn frame_lost(&mut self, maximum_data: u64) {
        if maximum_data == self.max_allowed {
            self.frame_pending = true;
        }
    }

    const fn frame_sent(&mut self, new_max: u64) {
        self.max_allowed = new_max;
        self.frame_pending = false;
    }

    pub const fn set_max_active(&mut self, max: u64) {
        self.frame_pending |= self.max_active < max;
        self.max_active = max;
    }

    pub const fn retired(&self) -> u64 {
        self.retired
    }

    pub const fn consumed(&self) -> u64 {
        self.consumed
    }

    /// Core auto-tuning logic for adjusting the maximum flow control window.
    ///
    /// This method is called by both connection-level and stream-level
    /// implementations. It increases `max_active` when the sending rate exceeds
    /// what the current window and RTT would allow, capping at `max_window`.
    fn auto_tune_inner(
        &mut self,
        now: Instant,
        rtt: Duration,
        max_window: u64,
        subject: AutoTuneSubject,
    ) {
        let Some(max_allowed_sent_at) = self.last_update else {
            return;
        };

        let Ok(elapsed): Result<u64, _> = now
            .duration_since(max_allowed_sent_at)
            .as_micros()
            .try_into()
        else {
            return;
        };

        let Ok(rtt): Result<NonZeroU64, _> = rtt
            .as_micros()
            .try_into()
            .and_then(|rtt: u64| NonZeroU64::try_from(rtt))
        else {
            return;
        };

        let effective_window =
            (self.max_active * (WINDOW_UPDATE_FRACTION - 1)) / (WINDOW_UPDATE_FRACTION);

        let window_bytes_expected = (effective_window * elapsed) / (rtt);

        let window_bytes_used = self.max_active - (self.max_allowed - self.retired);
        let Some(excess) = window_bytes_used.checked_sub(window_bytes_expected) else {
            return;
        };

        let prev_max_active = self.max_active;
        let new_max_active = min(
            self.max_active + excess * WINDOW_INCREASE_MULTIPLIER,
            max_window,
        );

        if new_max_active <= prev_max_active {
            return;
        }

        self.max_active = new_max_active;
        qdebug!(
            "Increasing max {subject} receive window by {} B, \
                previous max_active: {} MiB, \
                new max_active: {} MiB, \
                last update: {:?}, \
                rtt: {rtt:?}",
            new_max_active - prev_max_active,
            prev_max_active / 1024 / 1024,
            self.max_active / 1024 / 1024,
            now - max_allowed_sent_at,
        );
    }
}

impl ReceiverFlowControl<()> {
    pub fn write_frames<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
        now: Instant,
        rtt: Duration,
    ) {
        if !self.frame_needed() {
            return;
        }

        self.auto_tune(now, rtt);

        let max_allowed = self.next_limit();
        if builder.write_varint_frame(&[FrameType::MaxData.into(), max_allowed]) {
            stats.max_data += 1;
            tokens.push(recovery::Token::Stream(StreamRecoveryToken::MaxData(
                max_allowed,
            )));
            self.frame_sent(max_allowed);
            self.last_update = Some(now);
        }
    }

    /// Auto-tune [`ReceiverFlowControl::max_active`], i.e. the connection flow
    /// control window.
    ///
    /// If the sending rate (`window_bytes_used`) exceeds the rate allowed by
    /// the maximum flow control window and the current rtt
    /// (`window_bytes_expected`), try to increase the maximum flow control
    /// window ([`ReceiverFlowControl::max_active`]).
    fn auto_tune(&mut self, now: Instant, rtt: Duration) {
        self.auto_tune_inner(now, rtt, MAX_LOCAL_MAX_DATA, AutoTuneSubject::Connection);
    }

    pub fn add_retired(&mut self, count: u64) {
        debug_assert!(self.retired + count <= self.consumed);
        self.retired += count;
        if self.should_send_update() {
            self.frame_pending = true;
        }
    }

    pub fn consume(&mut self, count: u64) -> Res<()> {
        if self.consumed + count > self.max_allowed {
            qtrace!(
                "Session RX window exceeded: consumed:{} new:{count} limit:{}",
                self.consumed,
                self.max_allowed
            );
            return Err(Error::FlowControl);
        }
        self.consumed += count;
        Ok(())
    }
}

impl ReceiverFlowControl<StreamId> {
    pub fn write_frames<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
        now: Instant,
        rtt: Duration,
    ) {
        if !self.frame_needed() {
            return;
        }

        self.auto_tune(now, rtt);

        let max_allowed = self.next_limit();
        if builder.write_varint_frame(&[
            FrameType::MaxStreamData.into(),
            self.subject.as_u64(),
            max_allowed,
        ]) {
            stats.max_stream_data += 1;
            tokens.push(recovery::Token::Stream(
                StreamRecoveryToken::MaxStreamData {
                    stream_id: self.subject,
                    max_data: max_allowed,
                },
            ));
            self.frame_sent(max_allowed);
            self.last_update = Some(now);
        }
    }

    /// Auto-tune [`ReceiverFlowControl::max_active`], i.e. the stream flow
    /// control window.
    ///
    /// If the sending rate (`window_bytes_used`) exceeds the rate allowed by
    /// the maximum flow control window and the current rtt
    /// (`window_bytes_expected`), try to increase the maximum flow control
    /// window ([`ReceiverFlowControl::max_active`]).
    fn auto_tune(&mut self, now: Instant, rtt: Duration) {
        self.auto_tune_inner(
            now,
            rtt,
            MAX_LOCAL_MAX_STREAM_DATA,
            AutoTuneSubject::Stream(self.subject),
        );
    }

    pub fn add_retired(&mut self, count: u64) {
        debug_assert!(self.retired + count <= self.consumed);
        self.retired += count;
        if self.should_send_update() {
            self.frame_pending = true;
        }
    }

    pub fn set_consumed(&mut self, consumed: u64) -> Res<u64> {
        if consumed <= self.consumed {
            return Ok(0);
        }

        if consumed > self.max_allowed {
            qtrace!("Stream RX window exceeded: {consumed}");
            return Err(Error::FlowControl);
        }
        let new_consumed = consumed - self.consumed;
        self.consumed = consumed;
        Ok(new_consumed)
    }
}

impl ReceiverFlowControl<StreamType> {
    pub fn write_frames<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        if !self.frame_needed() {
            return;
        }
        let max_streams = self.next_limit();
        let frame = match self.subject {
            StreamType::BiDi => FrameType::MaxStreamsBiDi,
            StreamType::UniDi => FrameType::MaxStreamsUniDi,
        };
        if builder.write_varint_frame(&[frame.into(), max_streams]) {
            stats.max_streams += 1;
            tokens.push(recovery::Token::Stream(StreamRecoveryToken::MaxStreams {
                stream_type: self.subject,
                max_streams,
            }));
            self.frame_sent(max_streams);
        }
    }

    /// Check if received item exceeds the allowed flow control limit.
    pub const fn check_allowed(&self, new_end: u64) -> bool {
        new_end < self.max_allowed
    }

    /// Retire given amount of additional data.
    /// This function will send flow updates immediately.
    pub const fn add_retired(&mut self, count: u64) {
        self.retired += count;
        if count > 0 {
            self.send_flowc_update();
        }
    }
}

pub struct RemoteStreamLimit {
    streams_fc: ReceiverFlowControl<StreamType>,
    next_stream: StreamId,
}

impl RemoteStreamLimit {
    pub const fn new(stream_type: StreamType, max_streams: u64, role: Role) -> Self {
        Self {
            streams_fc: ReceiverFlowControl::new(stream_type, max_streams),
            next_stream: StreamId::init(stream_type, role.remote()),
        }
    }

    pub const fn is_allowed(&self, stream_id: StreamId) -> bool {
        let stream_idx = stream_id.as_u64() >> 2;
        self.streams_fc.check_allowed(stream_idx)
    }

    pub fn is_new_stream(&self, stream_id: StreamId) -> Res<bool> {
        if !self.is_allowed(stream_id) {
            return Err(Error::StreamLimit);
        }
        Ok(stream_id >= self.next_stream)
    }

    pub fn take_stream_id(&mut self) -> StreamId {
        let new_stream = self.next_stream;
        self.next_stream.next();
        assert!(self.is_allowed(new_stream));
        new_stream
    }
}

impl Deref for RemoteStreamLimit {
    type Target = ReceiverFlowControl<StreamType>;
    fn deref(&self) -> &Self::Target {
        &self.streams_fc
    }
}

impl DerefMut for RemoteStreamLimit {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.streams_fc
    }
}

pub struct RemoteStreamLimits(EnumMap<StreamType, RemoteStreamLimit>);

impl RemoteStreamLimits {
    pub const fn new(local_max_stream_bidi: u64, local_max_stream_uni: u64, role: Role) -> Self {
        Self(EnumMap::from_array([
            RemoteStreamLimit::new(StreamType::BiDi, local_max_stream_bidi, role),
            RemoteStreamLimit::new(StreamType::UniDi, local_max_stream_uni, role),
        ]))
    }
}

impl Index<StreamType> for RemoteStreamLimits {
    type Output = RemoteStreamLimit;

    fn index(&self, index: StreamType) -> &Self::Output {
        &self.0[index]
    }
}

impl IndexMut<StreamType> for RemoteStreamLimits {
    fn index_mut(&mut self, index: StreamType) -> &mut Self::Output {
        &mut self.0[index]
    }
}

pub struct LocalStreamLimits {
    limits: EnumMap<StreamType, SenderFlowControl<StreamType>>,
    role_bit: u64,
}

impl LocalStreamLimits {
    pub const fn new(role: Role) -> Self {
        Self {
            limits: EnumMap::from_array([
                SenderFlowControl::new(StreamType::BiDi, 0),
                SenderFlowControl::new(StreamType::UniDi, 0),
            ]),
            role_bit: StreamId::role_bit(role),
        }
    }

    pub fn take_stream_id(&mut self, stream_type: StreamType) -> Option<StreamId> {
        let fc = &mut self.limits[stream_type];
        if fc.available() > 0 {
            let new_stream = fc.used();
            fc.consume(1);
            Some(StreamId::from(
                (new_stream << 2) + stream_type as u64 + self.role_bit,
            ))
        } else {
            fc.blocked();
            None
        }
    }
}

impl Index<StreamType> for LocalStreamLimits {
    type Output = SenderFlowControl<StreamType>;

    fn index(&self, index: StreamType) -> &Self::Output {
        &self.limits[index]
    }
}

impl IndexMut<StreamType> for LocalStreamLimits {
    fn index_mut(&mut self, index: StreamType) -> &mut Self::Output {
        &mut self.limits[index]
    }
}
