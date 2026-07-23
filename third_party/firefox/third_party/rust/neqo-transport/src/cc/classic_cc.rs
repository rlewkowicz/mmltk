// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    cmp::{max, min},
    fmt::{Debug, Display},
    time::{Duration, Instant},
};

use neqo_common::{const_max, const_min, qdebug, qinfo, qlog::Qlog, qtrace};
use rustc_hash::FxHashMap as HashMap;

use super::CongestionController;
use crate::{
    Pmtud,
    cc::CongestionTrigger::{self, Ecn, Loss},
    packet, qlog,
    recovery::sent,
    rtt::RttEstimate,
    sender::PACING_BURST_SIZE,
    stats::{CongestionControlStats, SlowStartExitReason},
};

pub const CWND_INITIAL_PKTS: usize = 10;
pub const PERSISTENT_CONG_THRESH: u32 = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq, strum::IntoStaticStr)]
pub enum Phase {
    /// In either slow start or congestion avoidance, not recovery.
    #[strum(to_string = "slow_start")]
    SlowStart,
    /// In congestion avoidance.
    #[strum(to_string = "congestion_avoidance")]
    CongestionAvoidance,
    /// In a recovery period, but no packets have been sent yet.  This is a
    /// transient phase because we want to exempt the first packet sent after
    /// entering recovery from the congestion window.
    #[strum(to_string = "recovery")]
    RecoveryStart,
    /// In a recovery period, with the first packet sent at this time.
    #[strum(to_string = "recovery")]
    Recovery,
    /// Start of persistent congestion, which is transient, like `RecoveryStart`.
    #[strum(to_string = "slow_start")]
    PersistentCongestion,
}

impl Phase {
    pub const fn in_recovery(self) -> bool {
        matches!(self, Self::RecoveryStart | Self::Recovery)
    }

    pub fn in_slow_start(self) -> bool {
        self == Self::SlowStart
    }

    /// These states are transient, we tell qlog on entry, but not on exit.
    pub const fn transient(self) -> bool {
        matches!(self, Self::RecoveryStart | Self::PersistentCongestion)
    }

    /// Update a transient phase to the actual phase.
    pub fn update(&mut self) {
        *self = match self {
            Self::PersistentCongestion => Self::SlowStart,
            Self::RecoveryStart => Self::Recovery,
            _ => unreachable!(),
        };
    }
}

pub trait WindowAdjustment: Display + Debug {
    /// This is called when an ack is received.
    /// The function calculates the amount of acked bytes congestion controller needs
    /// to collect before increasing its cwnd by `MAX_DATAGRAM_SIZE`.
    fn bytes_for_cwnd_increase(
        &mut self,
        curr_cwnd: usize,
        new_acked_bytes: usize,
        min_rtt: Duration,
        max_datagram_size: usize,
        now: Instant,
    ) -> usize;
    /// This function is called when a congestion event has been detected and it
    /// returns new (decreased) values of `curr_cwnd` and `acked_bytes`.
    /// This value can be very small; the calling code is responsible for ensuring that the
    /// congestion window doesn't drop below the minimum of `CWND_MIN`.
    fn reduce_cwnd(
        &mut self,
        curr_cwnd: usize,
        acked_bytes: usize,
        max_datagram_size: usize,
        congestion_trigger: CongestionTrigger,
        cc_stats: &mut CongestionControlStats,
    ) -> (usize, usize);
    /// Cubic needs this signal to reset its epoch.
    fn on_app_limited(&mut self);
    /// Store the current congestion controller state, to be recovered in the case of a spurious
    /// congestion event.
    fn save_undo_state(&mut self);

    /// Restore the previously stored congestion controller state, to recover from a spurious
    /// congestion event.
    fn restore_undo_state(&mut self, cc_stats: &mut CongestionControlStats);
}

/// Trait for slow start exit algorithms.
///
/// Implementations define when and if to exit from slow start, how the slow start threshold
/// (`ssthresh`) is set on exit and they can influence how fast the exponential congestion window
/// growth rate during slow start is.
pub trait SlowStart: Display + Debug {
    /// Enables a trait implementor to ingest info about sent packets.
    fn on_packet_sent(&mut self, _sent_pn: packet::Number, _sent_bytes: usize) {}

    /// This is needed by SEARCH to keep its cumulative byte counters in sync during app-limited
    /// periods, when [`SlowStart::on_packets_acked`] is not called.
    fn record_acked_bytes(&mut self, _new_acked_bytes: usize) {}

    /// Handle packets being acknowledged during slow start. Returns the congestion window in bytes
    /// that slow start should be exited with. If slow start isn't exited returns `None`.
    fn on_packets_acked(
        &mut self,
        rtt_est: &RttEstimate,
        largest_acked: packet::Number,
        curr_cwnd: usize,
        cc_stats: &mut CongestionControlStats,
        now: Instant,
    ) -> Option<usize>;

    /// Calculates the congestion window increase in bytes during slow start. The default
    /// implementation returns `new_acked`, i.e. classic exponential slow start growth.
    fn calc_cwnd_increase(&self, new_acked: usize, _max_datagram_size: usize) -> usize {
        new_acked
    }

    /// Resets slow start state. Is used after persistent congestion so slow start algorithms
    /// perform cleanly in non-initial slow starts. Can also be used by the implementing algorithm
    /// for internal state reset when needed.
    fn reset(&mut self) {}
}

#[derive(Debug)]
struct MaybeLostPacket {
    time_sent: Instant,
}

#[derive(Debug, Clone)]
struct State {
    phase: Phase,
    congestion_window: usize,
    acked_bytes: usize,
    ssthresh: Option<usize>,
    /// Packet number of the first packet that was sent after a congestion event. When this one is
    /// acked we will exit [`Phase::Recovery`] and enter [`Phase::CongestionAvoidance`].
    recovery_start: Option<packet::Number>,
}

impl Display for State {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "State [phase: {:?}, cwnd: {}, ssthresh: {:?}, recovery_start: {:?}]",
            self.phase, self.congestion_window, self.ssthresh, self.recovery_start
        )
    }
}

impl State {
    pub const fn new(mtu: usize) -> Self {
        Self {
            phase: Phase::SlowStart,
            congestion_window: cwnd_initial(mtu),
            acked_bytes: 0,
            ssthresh: None,
            recovery_start: None,
        }
    }
}

#[derive(Debug)]
pub struct ClassicCongestionController<S, T> {
    slow_start: S,
    congestion_control: T,
    bytes_in_flight: usize,
    /// Packets that have supposedly been lost. These are used for spurious congestion event
    /// detection. Gets drained when the same packets are later acked and regularly purged from too
    /// old packets in [`Self::cleanup_maybe_lost_packets`]. Needs a tuple of `(packet::Number,
    /// packet::Type)` to identify packets across packet number spaces.
    maybe_lost_packets: HashMap<(packet::Number, packet::Type), MaybeLostPacket>,
    /// `first_app_limited` indicates the packet number after which the application might be
    /// underutilizing the congestion window. When underutilizing the congestion window due to not
    /// sending out enough data, we SHOULD NOT increase the congestion window.[1] Packets sent
    /// before this point are deemed to fully utilize the congestion window and count towards
    /// increasing the congestion window.
    ///
    /// [1]: https://datatracker.ietf.org/doc/html/rfc9002#section-7.8
    first_app_limited: Option<packet::Number>,
    pmtud: Pmtud,
    qlog: Qlog,
    /// Current congestion controller parameters.
    current: State,
    /// Congestion controller parameters that were stored on a congestion event to restore prior
    /// state in case the congestion event turns out to be spurious.
    ///
    /// For reference:
    /// - [`State::acked_bytes`] is stored because that is where we accumulate our window increase
    ///   credit and it is also reduced on a congestion event.
    /// - [`Self::bytes_in_flight`] is not stored because if it was to be restored it might get
    ///   out-of-sync with the actual number of bytes-in-flight on the path.
    stored: Option<State>,
    /// Whether to recover from spurious congestion events by restoring prior state.
    spurious_recovery: bool,
}

impl<S: Display, T: Display> Display for ClassicCongestionController<S, T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{}/{} CongCtrl [bif: {}, {}]",
            self.slow_start, self.congestion_control, self.bytes_in_flight, self.current
        )
    }
}

impl<S, T> ClassicCongestionController<S, T> {
    pub const fn max_datagram_size(&self) -> usize {
        self.pmtud.plpmtu()
    }

}

impl<S, T> CongestionController for ClassicCongestionController<S, T>
where
    S: SlowStart,
    T: WindowAdjustment,
{
    fn set_qlog(&mut self, qlog: Qlog) {
        self.pmtud.set_qlog(qlog.clone());
        self.qlog = qlog;
    }

    fn cwnd(&self) -> usize {
        self.current.congestion_window
    }

    fn bytes_in_flight(&self) -> usize {
        self.bytes_in_flight
    }

    fn cwnd_avail(&self) -> usize {
        self.current
            .congestion_window
            .saturating_sub(self.bytes_in_flight)
    }

    fn cwnd_min(&self) -> usize {
        self.max_datagram_size() * 2
    }

    fn pmtud(&self) -> &Pmtud {
        &self.pmtud
    }

    fn pmtud_mut(&mut self) -> &mut Pmtud {
        &mut self.pmtud
    }

    #[expect(
        clippy::too_many_lines,
        reason = "The main congestion control function contains a lot of logic."
    )]
    fn on_packets_acked(
        &mut self,
        acked_pkts: &[sent::Packet],
        rtt_est: &RttEstimate,
        now: Instant,
        cc_stats: &mut CongestionControlStats,
    ) {
        let mut is_app_limited = true;
        let mut new_acked = 0;
        let largest_packet_acked = acked_pkts
            .first()
            .expect("`acked_pkts.first().is_some()` is checked in `Loss::on_ack_received`");

        cc_stats.cwnd.get_or_insert(self.current.congestion_window);

        self.cleanup_maybe_lost_packets(now, rtt_est.pto(true));

        self.detect_spurious_congestion_event(acked_pkts, cc_stats);

        for pkt in acked_pkts {
            qtrace!(
                "packet_acked this={self:p}, pn={}, ps={}, ignored={}, lost={}, rtt_est={rtt_est:?}",
                pkt.pn(),
                pkt.len(),
                i32::from(!pkt.cc_outstanding()),
                i32::from(pkt.lost()),
            );
            if !pkt.cc_outstanding() {
                continue;
            }
            if self.first_app_limited.is_some_and(|f| pkt.pn() < f) {
                is_app_limited = false;
            }
            self.bytes_in_flight = self.bytes_in_flight.saturating_sub(pkt.len());

            if !self.after_recovery_start(pkt) {
                continue;
            }

            if self.current.phase.in_recovery() {
                self.set_phase(Phase::CongestionAvoidance, None, now);
            }

            new_acked += pkt.len();
        }

        if self.current.phase.in_slow_start() {
            self.slow_start.record_acked_bytes(new_acked);
        }

        if is_app_limited {
            self.congestion_control.on_app_limited();
            qdebug!(
                "on_packets_acked this={self:p}, limited=1, bytes_in_flight={}, cwnd={}, phase={:?}, new_acked={new_acked}",
                self.bytes_in_flight,
                self.current.congestion_window,
                self.current.phase
            );
            qlog::metrics_updated(
                &mut self.qlog,
                [qlog::Metric::BytesInFlight(self.bytes_in_flight)],
                now,
            );
            return;
        }

        if self
            .current
            .ssthresh
            .is_none_or(|s| self.current.congestion_window < s)
        {
            if let Some(exit_cwnd) = self.slow_start.on_packets_acked(
                rtt_est,
                largest_packet_acked.pn(),
                self.current.congestion_window,
                cc_stats,
                now,
            ) {
                qdebug!("Exited slow start by algorithm");
                self.current.congestion_window = exit_cwnd;
                self.current.ssthresh = Some(exit_cwnd);
                cc_stats.slow_start_exit_cwnd = Some(exit_cwnd);
                cc_stats.slow_start_exit_reason = Some(SlowStartExitReason::Heuristic);
                self.set_phase(Phase::CongestionAvoidance, None, now);
            } else {
                let cwnd_increase = self
                    .slow_start
                    .calc_cwnd_increase(new_acked, self.max_datagram_size());
                self.current.congestion_window += cwnd_increase;
                qtrace!("[{self}] slow start += {cwnd_increase}");

                if let Some(ssthresh) = self.current.ssthresh
                    && self.current.congestion_window >= ssthresh
                {
                    qdebug!(
                        "Exited slow start because the threshold was reached, ssthresh: {ssthresh}",
                    );
                    self.current.congestion_window = ssthresh;
                    self.set_phase(Phase::CongestionAvoidance, None, now);
                }
            }
        }

        if self
            .current
            .ssthresh
            .is_some_and(|s| self.current.congestion_window >= s)
        {
            let bytes_for_increase = self.congestion_control.bytes_for_cwnd_increase(
                self.current.congestion_window,
                new_acked,
                rtt_est.minimum(),
                self.max_datagram_size(),
                now,
            );
            debug_assert!(bytes_for_increase > 0);
            if self.current.acked_bytes >= bytes_for_increase {
                self.current.acked_bytes = 0;
                self.current.congestion_window += self.max_datagram_size();
            }
            self.current.acked_bytes += new_acked;
            if self.current.acked_bytes >= bytes_for_increase {
                self.current.acked_bytes -= bytes_for_increase;
                self.current.congestion_window += self.max_datagram_size(); 
            }
            self.current.acked_bytes = min(bytes_for_increase, self.current.acked_bytes);
        }

        cc_stats.cwnd = Some(self.current.congestion_window);
        qlog::metrics_updated(
            &mut self.qlog,
            [
                qlog::Metric::CongestionWindow(self.current.congestion_window),
                qlog::Metric::BytesInFlight(self.bytes_in_flight),
            ]
            .into_iter()
            .chain(self.current.ssthresh.map(qlog::Metric::SsThresh)),
            now,
        );

        qdebug!(
            "[{self}] on_packets_acked this={self:p}, limited=0, bytes_in_flight={}, cwnd={}, phase={:?}, new_acked={new_acked}",
            self.bytes_in_flight,
            self.current.congestion_window,
            self.current.phase
        );
    }

    /// Update congestion controller state based on lost packets.
    fn on_packets_lost(
        &mut self,
        first_rtt_sample_time: Option<Instant>,
        prev_largest_acked_sent: Option<Instant>,
        pto: Duration,
        lost_packets: &[sent::Packet],
        now: Instant,
        cc_stats: &mut CongestionControlStats,
    ) -> bool {
        if lost_packets.is_empty() {
            return false;
        }

        for pkt in lost_packets {
            if pkt.cc_in_flight() {
                qdebug!(
                    "packet_lost this={self:p}, pn={}, ps={}",
                    pkt.pn(),
                    pkt.len()
                );
                self.bytes_in_flight = self.bytes_in_flight.saturating_sub(pkt.len());
                if !pkt.is_pmtud_probe() {
                    let present = self.maybe_lost_packets.insert(
                        (pkt.pn(), pkt.packet_type()),
                        MaybeLostPacket {
                            time_sent: pkt.time_sent(),
                        },
                    );
                    qdebug!(
                        "Spurious detection: added MaybeLostPacket: pn {}, type {:?}, time_sent {:?}",
                        pkt.pn(),
                        pkt.packet_type(),
                        pkt.time_sent()
                    );
                    debug_assert!(present.is_none());
                }
            }
        }

        qlog::metrics_updated(
            &mut self.qlog,
            [qlog::Metric::BytesInFlight(self.bytes_in_flight)],
            now,
        );

        let lost_packets_no_pmtud = || lost_packets.iter().filter(|pkt| !pkt.is_pmtud_probe());

        let Some(last_lost_packet) = lost_packets_no_pmtud().rfind(|pkt| pkt.cc_in_flight()) else {
            return false;
        };

        let congestion = self.on_congestion_event(last_lost_packet, Loss, now, cc_stats);
        let persistent_congestion = self.detect_persistent_congestion(
            first_rtt_sample_time,
            prev_largest_acked_sent,
            pto,
            lost_packets_no_pmtud(),
            now,
            cc_stats,
        );
        qdebug!(
            "on_packets_lost this={self:p}, bytes_in_flight={}, cwnd={}, phase={:?}",
            self.bytes_in_flight,
            self.current.congestion_window,
            self.current.phase
        );
        congestion || persistent_congestion
    }

    /// Report received ECN CE mark(s) to the congestion controller as a
    /// congestion event.
    ///
    /// See <https://datatracker.ietf.org/doc/html/rfc9002#section-b.7>.
    fn on_ecn_ce_received(
        &mut self,
        largest_acked_pkt: &sent::Packet,
        now: Instant,
        cc_stats: &mut CongestionControlStats,
    ) -> bool {
        self.on_congestion_event(largest_acked_pkt, Ecn, now, cc_stats)
    }

    fn discard(&mut self, pkt: &sent::Packet, now: Instant) {
        if pkt.cc_outstanding() {
            assert!(self.bytes_in_flight >= pkt.len());
            self.bytes_in_flight -= pkt.len();
            qlog::metrics_updated(
                &mut self.qlog,
                [qlog::Metric::BytesInFlight(self.bytes_in_flight)],
                now,
            );
            qtrace!("[{self}] Ignore pkt with size {}", pkt.len());
        }
    }

    fn discard_in_flight(&mut self, now: Instant) {
        self.bytes_in_flight = 0;
        qlog::metrics_updated(
            &mut self.qlog,
            [qlog::Metric::BytesInFlight(self.bytes_in_flight)],
            now,
        );
    }

    fn on_packet_sent(&mut self, pkt: &sent::Packet, now: Instant, pacing_limited: bool) {
        if self.current.phase.transient() {
            self.current.recovery_start = Some(pkt.pn());
            qdebug!("set recovery_start to pn={}", pkt.pn());
            self.current.phase.update();
        }

        if !pkt.cc_in_flight() {
            return;
        }

        if self.current.phase.in_slow_start() {
            self.slow_start.on_packet_sent(pkt.pn(), pkt.len());
        }

        if pacing_limited || !self.app_limited() {
            self.first_app_limited = Some(pkt.pn() + 1);
        }

        self.bytes_in_flight += pkt.len();
        qdebug!(
            "packet_sent this={self:p}, pn={}, ps={}",
            pkt.pn(),
            pkt.len()
        );
        qlog::metrics_updated(
            &mut self.qlog,
            [qlog::Metric::BytesInFlight(self.bytes_in_flight)],
            now,
        );
    }

    /// Whether a packet can be sent immediately as a result of entering recovery.
    fn recovery_packet(&self) -> bool {
        self.current.phase == Phase::RecoveryStart
    }
}

const fn cwnd_initial(mtu: usize) -> usize {
    const_min(CWND_INITIAL_PKTS * mtu, const_max(2 * mtu, 14_720))
}

impl<S, T> ClassicCongestionController<S, T>
where
    S: SlowStart,
    T: WindowAdjustment,
{
    pub fn new(
        slow_start: S,
        congestion_control: T,
        pmtud: Pmtud,
        spurious_recovery: bool,
    ) -> Self {
        let mtu = pmtud.plpmtu();
        Self {
            slow_start,
            congestion_control,
            bytes_in_flight: 0,
            maybe_lost_packets: HashMap::default(),
            qlog: Qlog::disabled(),
            first_app_limited: None,
            pmtud,
            current: State::new(mtu),
            stored: None,
            spurious_recovery,
        }
    }

#[cfg(any())]









    #[must_use]
    pub const fn ssthresh(&self) -> Option<usize> {
        self.current.ssthresh
    }


    /// Accessor for [`ClassicCongestionController::congestion_control`]. Is used to call Cubic
    /// getters in tests.

    /// Mutable accessor for [`ClassicCongestionController::congestion_control`]. Is used to call
    /// Cubic setters in tests.


    fn set_phase(
        &mut self,
        phase: Phase,
        trigger: Option<qlog::CongestionStateTrigger>,
        now: Instant,
    ) {
        if self.current.phase == phase {
            return;
        }
        qdebug!("[{self}] phase -> {phase:?}");
        let old_state = self.current.phase;
        if !str::eq(old_state.into(), phase.into()) {
            qlog::congestion_state_updated(
                &mut self.qlog,
                Some(old_state.into()),
                phase.into(),
                trigger,
                now,
            );
        }
        self.current.phase = phase;
    }

    fn detect_spurious_congestion_event(
        &mut self,
        acked_packets: &[sent::Packet],
        cc_stats: &mut CongestionControlStats,
    ) {
        if self.maybe_lost_packets.is_empty() {
            return;
        }

        for acked_packet in acked_packets {
            if self
                .maybe_lost_packets
                .remove(&(acked_packet.pn(), acked_packet.packet_type()))
                .is_some()
            {
                qdebug!(
                    "Spurious detection: removed MaybeLostPacket with pn {}, type {:?}",
                    acked_packet.pn(),
                    acked_packet.packet_type(),
                );
            }
        }

        if self.maybe_lost_packets.is_empty() {
            qdebug!(
                "Spurious detection: maybe_lost_packets emptied -> calling on_spurious_congestion_event"
            );
            self.on_spurious_congestion_event(cc_stats);
        }
    }

    /// Cleanup lost packets that we are fairly sure will never be getting a late acknowledgment
    /// for.
    fn cleanup_maybe_lost_packets(&mut self, now: Instant, pto: Duration) {
        let max_age = pto * 2;
        self.maybe_lost_packets.retain(|(pn, pt), packet| {
            let keep = now.saturating_duration_since(packet.time_sent) <= max_age;
            if !keep {
                qdebug!(
                    "Spurious detection: cleaned up old MaybeLostPacket with pn {pn}, type {pt:?}"
                );
            }
            keep
        });
    }

    fn on_spurious_congestion_event(&mut self, cc_stats: &mut CongestionControlStats) {
        let Some(stored) = self.stored.take() else {
            qdebug!(
                "[{self}] Spurious cong event -> ABORT, no stored params to restore available."
            );
            return;
        };

        cc_stats.congestion_events.spurious += 1;

        if stored.congestion_window <= self.current.congestion_window {
            qinfo!(
                "[{self}] Spurious cong event -> IGNORED because stored.cwnd {} < self.cwnd {};",
                stored.congestion_window,
                self.current.congestion_window
            );
            return;
        }

        if !self.spurious_recovery {
            qinfo!("[{self}] Spurious cong event detected -> recovery disabled;");
            return;
        }

        self.congestion_control.restore_undo_state(cc_stats);
        qdebug!(
            "Spurious cong event: recovering cc params from {} to {stored}",
            self.current
        );
        self.current = stored;

        if self.current.phase.in_slow_start() {
            cc_stats.slow_start_exit_cwnd = None;
            cc_stats.slow_start_exit_reason = None;
        }
        qinfo!("[{self}] Spurious cong event -> RESTORED;");
    }

    fn detect_persistent_congestion<'a>(
        &mut self,
        first_rtt_sample_time: Option<Instant>,
        prev_largest_acked_sent: Option<Instant>,
        pto: Duration,
        lost_packets: impl IntoIterator<Item = &'a sent::Packet>,
        now: Instant,
        cc_stats: &mut CongestionControlStats,
    ) -> bool {
        if first_rtt_sample_time.is_none() {
            return false;
        }

        let pc_period = pto * PERSISTENT_CONG_THRESH;

        let mut last_pn: Option<packet::Number> = None;
        let mut start = None;

        let cutoff = max(first_rtt_sample_time, prev_largest_acked_sent);
        for p in lost_packets
            .into_iter()
            .skip_while(|p| Some(p.time_sent()) < cutoff)
        {
            if last_pn.is_none_or(|l| p.pn() != l + 1) {
                start = None;
            }
            last_pn = Some(p.pn());
            if !p.cc_in_flight() {
                continue;
            }
            if let Some(t) = start {
                let elapsed = p
                    .time_sent()
                    .checked_duration_since(t)
                    .expect("time is monotonic");
                if elapsed > pc_period {
                    qinfo!("[{self}] persistent congestion");
                    self.current.congestion_window = self.cwnd_min();
                    self.current.acked_bytes = 0;
                    self.set_phase(
                        Phase::PersistentCongestion,
                        Some(qlog::CongestionStateTrigger::PersistentCongestion),
                        now,
                    );
                    self.slow_start.reset();

                    cc_stats.cwnd = Some(self.current.congestion_window);
                    qlog::metrics_updated(
                        &mut self.qlog,
                        [
                            Some(qlog::Metric::CongestionWindow(
                                self.current.congestion_window,
                            )),
                            self.current.ssthresh.map(qlog::Metric::SsThresh),
                        ]
                        .into_iter()
                        .flatten(),
                        now,
                    );

                    return true;
                }
            } else {
                start = Some(p.time_sent());
            }
        }
        false
    }

    #[must_use]
    fn after_recovery_start(&self, packet: &sent::Packet) -> bool {
        !self.current.phase.transient()
            && self
                .current
                .recovery_start
                .is_none_or(|pn| packet.pn() >= pn)
    }

    /// Handle a congestion event.
    /// Returns true if this was a true congestion event.
    fn on_congestion_event(
        &mut self,
        last_packet: &sent::Packet,
        congestion_trigger: CongestionTrigger,
        now: Instant,
        cc_stats: &mut CongestionControlStats,
    ) -> bool {
        if !self.after_recovery_start(last_packet) {
            qdebug!(
                "Called on_congestion_event during recovery -> don't react; last_packet {}, recovery_start {}",
                last_packet.pn(),
                self.current.recovery_start.unwrap_or(0)
            );
            return false;
        }

        if congestion_trigger != Ecn {
            self.stored = Some(self.current.clone());
            self.congestion_control.save_undo_state();
        }

        let (cwnd, acked_bytes) = self.congestion_control.reduce_cwnd(
            self.current.congestion_window,
            self.current.acked_bytes,
            self.max_datagram_size(),
            congestion_trigger,
            cc_stats,
        );
        self.current.congestion_window = max(cwnd, self.cwnd_min());
        self.current.acked_bytes = acked_bytes;
        self.current.ssthresh = Some(self.current.congestion_window);
        qinfo!(
            "[{self}] Cong event -> recovery; cwnd {}, ssthresh {:?}",
            self.current.congestion_window,
            self.current.ssthresh
        );

        match congestion_trigger {
            Loss => cc_stats.congestion_events.loss += 1,
            Ecn => cc_stats.congestion_events.ecn += 1,
        }
        cc_stats.cwnd = Some(self.current.congestion_window);
        if self.current.phase.in_slow_start() {
            cc_stats.slow_start_exit_cwnd = Some(self.current.congestion_window);
            cc_stats.slow_start_exit_reason = Some(SlowStartExitReason::CongestionEvent);
        }

        qlog::metrics_updated(
            &mut self.qlog,
            [
                Some(qlog::Metric::CongestionWindow(
                    self.current.congestion_window,
                )),
                self.current.ssthresh.map(qlog::Metric::SsThresh),
            ]
            .into_iter()
            .flatten(),
            now,
        );
        let trigger = (congestion_trigger == Ecn).then_some(qlog::CongestionStateTrigger::Ecn);
        self.set_phase(Phase::RecoveryStart, trigger, now);
        true
    }

    fn app_limited(&self) -> bool {
        if self.bytes_in_flight >= self.current.congestion_window {
            false
        } else if self.current.phase.in_slow_start() {
            self.bytes_in_flight < self.current.congestion_window / 2
        } else {
            (self.bytes_in_flight + self.max_datagram_size() * PACING_BURST_SIZE)
                < self.current.congestion_window
        }
    }
}
