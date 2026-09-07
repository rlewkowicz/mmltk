// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    cmp::min,
    fmt::{self, Debug, Display, Formatter},
    time::{Duration, Instant},
};

use neqo_common::qtrace;

use crate::rtt::GRANULARITY;

/// A pacer that uses a leaky bucket.
pub struct Pacer {
    /// Whether pacing is enabled.
    enabled: bool,
    /// The last update time.
    t: Instant,
    /// The maximum capacity, or burst size, in bytes.
    m: usize,
    /// The current capacity, in bytes. When negative, represents accumulated debt
    /// from sub-granularity sends that will be paid off in future pacing calculations.
    c: isize,
    /// The packet size or minimum capacity for sending, in bytes.
    p: usize,
}

impl Pacer {
    /// This value determines how much faster the pacer operates than the
    /// congestion window.
    ///
    /// A value of 1 would cause all packets to be spaced over the entire RTT,
    /// which is a little slow and might act as an additional restriction in
    /// the case the congestion controller increases the congestion window.
    /// This value spaces packets over half the congestion window, which matches
    /// our current congestion controller, which double the window every RTT.
    const SPEEDUP: u64 = 2;

    /// Create a new `Pacer`.  This takes the current time, the maximum burst size,
    /// and the packet size.
    ///
    /// The value of `m` is the maximum capacity in bytes.  `m` primes the pacer
    /// with credit and determines the burst size.  `m` must not exceed
    /// the initial congestion window, but it should probably be lower.
    ///
    /// The value of `p` is the packet size in bytes, which determines the minimum
    /// credit needed before a packet is sent.  This should be a substantial
    /// fraction of the maximum packet size, if not the packet size.
    pub fn new(enabled: bool, now: Instant, m: usize, p: usize) -> Self {
        assert!(m >= p, "maximum capacity has to be at least one packet");
        assert!(isize::try_from(p).is_ok(), "p ({p}) exceeds isize::MAX");
        Self {
            enabled,
            t: now,
            m,
            c: isize::try_from(m).expect("maximum capacity fits into isize"),
            p,
        }
    }

    pub const fn mtu(&self) -> usize {
        self.p
    }

    pub const fn set_mtu(&mut self, mtu: usize) {
        self.p = mtu;
    }

    /// Determine when the next packet will be available based on the provided
    /// RTT, provided congestion window and accumulated credit or debt.  This
    /// doesn't update state.  This returns a time, which could be in the past
    /// (this object doesn't know what the current time is).
    pub fn next(&self, rtt: Duration, cwnd: usize) -> Instant {
        let packet = isize::try_from(self.p).expect("packet size fits into isize");

        if self.c >= packet {
            qtrace!("[{self}] next {cwnd}/{rtt:?} no wait = {:?}", self.t);
            return self.t;
        }

        let Ok(deficit) = u64::try_from(packet - self.c) else {
            qtrace!("[{self}] next {cwnd}/{rtt:?} deficit overflow");
            return self.t;
        };
        let rtt_ns = u64::try_from(rtt.as_nanos()).unwrap_or(u64::MAX);
        let divisor = (cwnd as u64).saturating_mul(Self::SPEEDUP);
        let w_ns = rtt_ns.saturating_mul(deficit) / divisor;

        #[expect(
            clippy::cast_possible_truncation,
            reason = "GRANULARITY is 1ms, fits in u64"
        )]
        if w_ns < GRANULARITY.as_nanos() as u64 {
            qtrace!("[{self}] next {cwnd}/{rtt:?} below granularity ({w_ns}ns)");
            return self.t;
        }

        let nxt = self.t + Duration::from_nanos(w_ns);
        qtrace!("[{self}] next {cwnd}/{rtt:?} wait {w_ns}ns = {nxt:?}");
        nxt
    }

    /// Bytes sendable at `SPEEDUP * cwnd / rtt` pace over `elapsed`.
    /// Returns `None` if `rtt` is zero.
    ///
    /// The key product is `elapsed_ns * cwnd * SPEEDUP`.  At 400 Gbps with a
    /// 100 ms RTT the BDP is ~5 GB, so `factor` = cwnd * 2 ≈ 10^10.  The
    /// inter-packet interval at that rate is ~24 ns, giving a product of
    /// ~2.4*10^11, well within u64.  Even a full-RTT elapsed (10^8 ns) gives
    /// 10^8 * 10^10 = 10^18 < `u64::MAX` (1.8*10^19).  Beyond that the
    /// `saturating_mul` caps the value and callers clamp to `self.m`.
    fn bytes_for(cwnd: usize, rtt: Duration, elapsed: Duration) -> Option<u64> {
        let rtt_ns = u64::try_from(rtt.as_nanos()).unwrap_or(u64::MAX);
        let elapsed_ns = u64::try_from(elapsed.as_nanos()).unwrap_or(u64::MAX);
        let factor = (cwnd as u64).saturating_mul(Self::SPEEDUP);
        elapsed_ns.saturating_mul(factor).checked_div(rtt_ns)
    }

    /// Compute the effective pacing rate in bytes per second.
    ///
    /// Returns `None` if `rtt` is zero.
    pub(crate) fn rate(cwnd: usize, rtt: Duration) -> Option<u64> {
        Self::bytes_for(cwnd, rtt, Duration::from_secs(1))
    }

    /// Spend credit. Returns `true` when the next send would be pacing-limited,
    /// i.e., [`Pacer::next`] now returns a time strictly after `now`.
    /// Always returns `false` when pacing is disabled.
    ///
    /// This cannot fail, but instead may carry debt into the future (see
    /// [`Pacer::c`]).
    pub fn spend(&mut self, now: Instant, rtt: Duration, cwnd: usize, count: usize) -> bool {
        if !self.enabled {
            self.t = now;
            return false;
        }

        qtrace!("[{self}] spend {count} over {cwnd}, {rtt:?}");
        let incr = Self::bytes_for(cwnd, rtt, now.saturating_duration_since(self.t))
            .and_then(|b| usize::try_from(b).ok())
            .unwrap_or(self.m);

        self.c = min(
            isize::try_from(self.m).unwrap_or(isize::MAX),
            self.c
                .saturating_add(isize::try_from(incr).unwrap_or(isize::MAX))
                .saturating_sub(isize::try_from(count).unwrap_or(isize::MAX)),
        );
        self.t = now;
        self.next(rtt, cwnd) > now
    }
}

impl Display for Pacer {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "Pacer {}/{}", self.c, self.p)
    }
}

impl Debug for Pacer {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "Pacer@{:?} {}/{}..{}", self.t, self.c, self.p, self.m)
    }
}
