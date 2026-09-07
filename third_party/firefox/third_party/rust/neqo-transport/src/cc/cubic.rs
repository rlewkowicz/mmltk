// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! CUBIC congestion control (RFC 9438)

use std::{
    fmt::{self, Display},
    time::{Duration, Instant},
};

use neqo_common::{qdebug, qtrace};

use crate::{
    cc::{
        CongestionTrigger::{self, Ecn},
        classic_cc::WindowAdjustment,
    },
    stats::CongestionControlStats,
};

/// Convert an integer congestion window value into a floating point value.
/// This has the effect of reducing larger values to `1<<53`.
/// If you have a congestion window that large, something is probably wrong.
pub fn convert_to_f64(v: usize) -> f64 {
    let mut f_64 = f64::from(u32::try_from(v >> 21).unwrap_or(u32::MAX));
    f_64 *= 2_097_152.0; 
    #[expect(clippy::cast_possible_truncation, reason = "The mask makes this safe.")]
    let v_trunc = (v & 0x1f_ffff) as u32;
    f_64 += f64::from(v_trunc);
    f_64
}

#[derive(Debug, Default, Clone)]
pub struct State {
    /// > An estimate for the congestion window \[...\] in the Reno-friendly region -- that
    /// > is, an estimate for the congestion window of Reno.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-variables-of-interest>
    ///
    /// > Reno performs well in certain types of networks -- for example, under short RTTs and
    /// > small bandwidths (or small BDPs). In these networks, CUBIC remains in the Reno-friendly
    /// > region to achieve at least the same throughput as Reno.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-reno-friendly-region>
    w_est: f64,
    /// > The time period in seconds it takes to increase the congestion window size
    /// > at the beginning of the current congestion avoidance stage to `w_max`.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-variables-of-interest>
    ///
    /// Formula:
    ///
    /// `k = cubic_root((w_max - cwnd_epoch) / C)`
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-window-increase-function>
    k: f64,
    /// > Size of `cwnd` in \[bytes\] just before `cwnd` was reduced in the last congestion
    /// > event \[...\]. \[With\] fast convergence enabled, `w_max` may be further reduced based on
    /// > the current saturation point.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-variables-of-interest>
    ///
    /// `w_max` acts as the plateau for the cubic function where it switches from the concave to
    /// the convex region.
    ///
    /// It is calculated with the following logic:
    ///
    /// ```pseudo
    /// if (w_max > cwnd) {
    ///     w_max = cwnd * FAST_CONVERGENCE_FACTOR;
    /// } else {
    ///     w_max = cwnd;
    /// }
    /// ```
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-fast-convergence>
    w_max: Option<f64>,
    /// > The time in seconds at which the current congestion avoidance stage started.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-variables-of-interest>
    ///
    /// This is also reset on being application limited.
    t_epoch: Option<Instant>,
    /// New and unused leftover acked bytes for calculating the reno region increases to `w_est`.
    reno_acked_bytes: f64,
}

impl Display for State {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(
            f,
            "state [w_max: {:?}, k: {}, t_epoch: {:?}]",
            self.w_max, self.k, self.t_epoch
        )?;
        Ok(())
    }
}

#[derive(Debug, Default)]
pub struct Cubic {
    /// Current CUBIC parameters.
    current: State,
    /// CUBIC parameters that have been stored on a congestion event to restore later in case it
    /// turns out to have been spurious.
    stored: Option<State>,
}

impl Display for Cubic {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "Cubic {}", self.current)
    }
}

impl Cubic {
    /// > Constant that determines the aggressiveness of CUBIC in competing with other congestion
    /// > control algorithms in high-BDP networks.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-constants-of-interest>
    ///
    /// See section 5.1 of RFC9438 for discussion on how to set the concrete value:
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-fairness-to-reno>
    pub const C: f64 = 0.4;

    /// > CUBIC additive increase factor used in the Reno-friendly region \[to achieve approximately
    /// > the same average congestion window size as Reno\].
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-constants-of-interest>
    ///
    /// > The model used to calculate CUBIC_ALPHA is not absolutely precise,
    /// > but analysis and simulation \[...\], as well as over a decade of experience with
    /// > CUBIC in the public Internet, show that this approach produces acceptable
    /// > levels of rate fairness between CUBIC and Reno flows.
    ///
    /// Formula:
    ///
    /// `ALPHA = 3.0 * (1.0 - CUBIC_BETA) / (1.0 + CUBIC_BETA)`
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-reno-friendly-region>
    pub const ALPHA: f64 = 3.0 * (1.0 - 0.7) / (1.0 + 0.7); 

    /// > CUBIC multiplicative decrease factor
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-constants-of-interest>
    ///
    /// > To balance between the scalability and convergence speed, CUBIC sets the multiplicative
    /// > window
    /// > decrease factor to 0.7 while Standard TCP uses 0.5. While this improves the scalability of
    /// > CUBIC, a side effect of this decision is slower convergence, especially under low
    /// > statistical multiplexing environments.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-principle-4-for-the-cubic-d>
    ///
    /// For implementation reasons neqo uses a dividend and divisor approach with `usize` typing.
    /// The divisor is set to `100` to also accommodate the `0.85` beta value for ECN induced
    /// congestion events.
    pub const BETA_USIZE_DIVISOR: usize = 100;

    /// > CUBIC multiplicative decrease factor
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-constants-of-interest>
    ///
    /// > To balance between the scalability and convergence speed, CUBIC sets the multiplicative
    /// > window
    /// > decrease factor to 0.7 while Standard TCP uses 0.5. While this improves the scalability of
    /// > CUBIC, a side effect of this decision is slower convergence, especially under low
    /// > statistical multiplexing environments.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-principle-4-for-the-cubic-d>
    ///
    /// For implementation reasons neqo uses a dividend and divisor approach with `usize` typing to
    /// construct `CUBIC_BETA = 0.7` from `70/100`.
    pub const BETA_USIZE_DIVIDEND: usize = 70;

    /// As per RFC 8511 it makes sense to have a different decrease factor for ECN-CE congestion
    /// events than for loss induced congestion events.
    ///
    /// > CUBIC connections benefit from beta_{ecn} of 0.85.
    ///
    /// <https://www.rfc-editor.org/rfc/rfc8511.html#section-3.1>
    ///
    /// For implementation reasons neqo uses a dividend and divisor approach with `usize` typing to
    /// construct the beta value from `85/100`.
    pub const BETA_USIZE_DIVIDEND_ECN: usize = 85;

    /// This is the factor that is used by fast convergence to further reduce the next `W_max` when
    /// a congestion event occurs while `cwnd < W_max`. This speeds up the bandwidth release for
    /// when a new flow joins the network.
    ///
    /// The calculation assumes `CUBIC_BETA = 0.7`.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#name-fast-convergence>
    pub const FAST_CONVERGENCE_FACTOR: f64 = f64::midpoint(1.0, 0.7);

    /// Original equation is:
    ///
    /// `k = cubic_root((w_max - cwnd_epoch)/C)`
    ///
    /// with `cwnd_epoch` being the congestion window at the start of the current congestion
    /// avoidance stage (so at time `t_epoch`).
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#figure-2>
    ///
    /// Taking into account that neqo is using bytes but the formula assumes segments for both
    /// `w_max` and `cwnd_epoch` it becomes:
    ///
    /// `k = cubic_root((w_max - cwnd_epoch)/SMSS/C)`
    fn calc_k(w_max: f64, cwnd_epoch: f64, max_datagram_size: f64) -> f64 {
        ((w_max - cwnd_epoch) / max_datagram_size / Self::C).cbrt()
    }

    /// `w_cubic(t) = C*(t-K)^3 + w_max`
    ///
    /// with `t = t_current - t_epoch`.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#figure-1>
    ///
    /// Taking into account that neqo is using bytes and the formula returns segments and that
    /// `w_max` already is in bytes the formula becomes:
    ///
    /// `w_cubic(t) = (C*(t-K)^3) * SMSS + w_max`
    fn w_cubic(&self, t: f64, w_max: f64, max_datagram_size: f64) -> f64 {
        (Self::C * (t - self.current.k).powi(3)).mul_add(max_datagram_size, w_max)
    }

    /// Sets `w_est`, `k`, `t_epoch` and `reno_acked_bytes` at the start of a new epoch
    /// (new congestion avoidance stage) according to RFC 9438. The `w_max` variable has
    /// been set in `reduce_cwnd()` prior to this call.
    ///
    /// > `w_est` is set equal to `cwnd_epoch` at the start of the congestion avoidance stage.
    ///
    /// <https://datatracker.ietf.org/doc/html/rfc9438#section-4.3-9>
    ///
    /// Also initializes `k` and `w_max` if we start an epoch without having ever had
    /// a congestion event, which can happen upon exiting slow start.
    fn start_epoch(
        &mut self,
        curr_cwnd: f64,
        new_acked_bytes: f64,
        max_datagram_size: f64,
        now: Instant,
    ) {
        self.current.t_epoch = Some(now);
        self.current.reno_acked_bytes = new_acked_bytes;
        self.current.w_est = curr_cwnd;
        self.current.k = match self.current.w_max {
            Some(w_max) if w_max > curr_cwnd => Self::calc_k(w_max, curr_cwnd, max_datagram_size),
            _ => {
                self.current.w_max = Some(curr_cwnd);
                0.0
            }
        };
        qtrace!("[{self}] New epoch");
    }


}

impl WindowAdjustment for Cubic {
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "Cast from f64 to usize."
    )]
    fn bytes_for_cwnd_increase(
        &mut self,
        curr_cwnd: usize,
        new_acked_bytes: usize,
        min_rtt: Duration,
        max_datagram_size: usize,
        now: Instant,
    ) -> usize {
        let curr_cwnd = convert_to_f64(curr_cwnd);
        let new_acked_bytes = convert_to_f64(new_acked_bytes);
        let max_datagram_size = convert_to_f64(max_datagram_size);

        let t_epoch = if let Some(t) = self.current.t_epoch {
            self.current.reno_acked_bytes += new_acked_bytes;
            t
        } else {
            self.start_epoch(curr_cwnd, new_acked_bytes, max_datagram_size, now);
            self.current
                .t_epoch
                .expect("unwrapping `None` value -- it should've been set by `start_epoch`")
        };

        let t = now.saturating_duration_since(t_epoch);
        let w_max = self
            .current
            .w_max
            .expect("w_max must be set in self.start_epoch");
        let target_cubic = f64::clamp(
            self.w_cubic((t + min_rtt).as_secs_f64(), w_max, max_datagram_size),
            curr_cwnd,
            curr_cwnd * 1.5,
        );


        let increase = (Self::ALPHA * self.current.reno_acked_bytes / curr_cwnd).floor();

        if increase > 0.0 {
            self.current.w_est = increase.mul_add(max_datagram_size, self.current.w_est);
            let acked_bytes_used = increase * curr_cwnd / Self::ALPHA;
            self.current.reno_acked_bytes -= acked_bytes_used;
        }

        let target = target_cubic.max(self.current.w_est);

        let cwnd_increase = target - curr_cwnd;

        (max_datagram_size * curr_cwnd / cwnd_increase.max(1.0)) as usize
    }

    fn reduce_cwnd(
        &mut self,
        curr_cwnd: usize,
        acked_bytes: usize,
        max_datagram_size: usize,
        congestion_trigger: CongestionTrigger,
        cc_stats: &mut CongestionControlStats,
    ) -> (usize, usize) {
        let curr_cwnd_f64 = convert_to_f64(curr_cwnd);
        self.current.w_max = Some(
            if self
                .current
                .w_max
                .is_some_and(|w| curr_cwnd_f64 + convert_to_f64(max_datagram_size) < w)
            {
                curr_cwnd_f64 * Self::FAST_CONVERGENCE_FACTOR
            } else {
                curr_cwnd_f64
            },
        );
        cc_stats.w_max = self.current.w_max;

        self.current.t_epoch = None;
        let beta_dividend = if congestion_trigger == Ecn {
            Self::BETA_USIZE_DIVIDEND_ECN
        } else {
            Self::BETA_USIZE_DIVIDEND
        };
        (
            curr_cwnd * beta_dividend / Self::BETA_USIZE_DIVISOR,
            acked_bytes * beta_dividend / Self::BETA_USIZE_DIVISOR,
        )
    }

    fn on_app_limited(&mut self) {
        self.current.t_epoch = None;
    }

    fn save_undo_state(&mut self) {
        self.stored = Some(self.current.clone());
    }

    fn restore_undo_state(&mut self, cc_stats: &mut CongestionControlStats) {
        let Some(stored) = self.stored.take() else {
            debug_assert!(false, "couldn't restore {self} specific undo state");
            return;
        };

        qdebug!(
            "Spurious cong event: recovering cubic state from {} to {stored}",
            self.current
        );
        self.current = stored;
        cc_stats.w_max = self.current.w_max;
    }
}
