// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    num::NonZeroU64,
    time::{Duration, Instant},
};

#[derive(Debug, Clone)]
pub struct Scone {
    updated: Instant,
    rate: Bitrate,
}

impl Scone {
    pub(crate) const PERIOD: Duration = Duration::from_secs(67);

    #[must_use]
    pub const fn new(updated: Instant, rate: Bitrate) -> Self {
        Self { updated, rate }
    }

    /// Determine if the advice has expired.
    #[must_use]
    pub fn expired(&self, now: Instant) -> bool {
        self.updated + Self::PERIOD <= now
    }

    #[must_use]
    pub const fn rate(&self) -> Bitrate {
        self.rate
    }

    /// Update the value, return true if updated.
    pub fn update(&mut self, now: Instant, rate: Option<Bitrate>) -> bool {
        if rate.is_some_and(|r| r <= self.rate) || self.expired(now) {
            self.updated = now;
            let rate = rate.unwrap_or_default();
            let changed = rate != self.rate;
            self.rate = rate;
            changed
        } else {
            false
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Bitrate(u8);

impl Bitrate {
    pub const UNKNOWN: Self = Self(0x7f);

    pub const fn is_set(self) -> bool {
        self.0 != Self::UNKNOWN.0
    }
}

impl Default for Bitrate {
    fn default() -> Self {
        Self::UNKNOWN
    }
}

impl PartialOrd for Bitrate {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        self.0.partial_cmp(&other.0)
    }
}

impl From<(u8, u32)> for Bitrate {
    fn from((first, version): (u8, u32)) -> Self {
        Self(u8::try_from(version >> 31).expect("always u8") | ((first & 0x3f) << 1))
    }
}

impl From<Bitrate> for Option<NonZeroU64> {
    #[expect(clippy::cast_possible_truncation, reason = "We want truncation here")]
    #[expect(clippy::cast_sign_loss, reason = "negative values are impossible")]
    fn from(value: Bitrate) -> Self {
        value
            .is_set()
            .then(|| {
                NonZeroU64::new(1.122_018_454_301_963_3_f64.powi(100 + i32::from(value.0)) as u64)
            })
            .flatten()
    }
}
