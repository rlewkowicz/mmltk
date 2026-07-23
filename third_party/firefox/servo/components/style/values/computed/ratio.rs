/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! `<ratio>` computed values.

use crate::values::animated::{Animate, Procedure};
use crate::values::computed::NonNegativeNumber;
use crate::values::distance::{ComputeSquaredDistance, SquaredDistance};
use crate::values::generics::ratio::Ratio as GenericRatio;
use crate::values::generics::NonNegative;
use crate::Zero;
use std::cmp::Ordering;

/// A computed <ratio> value.
pub type Ratio = GenericRatio<NonNegativeNumber>;

impl PartialOrd for Ratio {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        f64::partial_cmp(
            &((self.0).0 as f64 * (other.1).0 as f64),
            &((self.1).0 as f64 * (other.0).0 as f64),
        )
    }
}

impl Ratio {
    /// Returns the f32 value by dividing the first value by the second one.
    #[inline]
    fn to_f32(&self) -> f32 {
        debug_assert!(!self.is_degenerate());
        (self.0).0 / (self.1).0
    }
    /// Returns a new Ratio.
    #[inline]
    pub fn new(a: f32, b: f32) -> Self {
        GenericRatio(a.into(), b.into())
    }
}

/// https://drafts.csswg.org/css-values/#combine-ratio
impl Animate for Ratio {
    fn animate(&self, other: &Self, procedure: Procedure) -> Result<Self, ()> {
        if self.is_degenerate() || other.is_degenerate() {
            return Err(());
        }

        if matches!(procedure, Procedure::Add | Procedure::Accumulate { .. }) {
            return Ok(self.clone());
        }

        let start = self.to_f32().ln();
        let end = other.to_f32().ln();
        let e = std::f32::consts::E;
        let result = e.powf(start.animate(&end, procedure)?);
        if result.is_zero() || result.is_infinite() {
            return Err(());
        }
        Ok(GenericRatio(NonNegative(result), NonNegative(1.0)))
    }
}

impl ComputeSquaredDistance for Ratio {
    fn compute_squared_distance(&self, other: &Self) -> Result<SquaredDistance, ()> {
        if self.is_degenerate() || other.is_degenerate() {
            return Err(());
        }
        self.to_f32()
            .ln()
            .compute_squared_distance(&other.to_f32().ln())
    }
}
