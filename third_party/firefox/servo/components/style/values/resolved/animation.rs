/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! Resolved animation values.

use super::{Context, ToResolvedValue};

use crate::values::computed::time::Time;
use crate::values::computed::AnimationDuration;

impl ToResolvedValue for AnimationDuration {
    type ResolvedValue = Self;

    fn to_resolved_value(self, context: &Context) -> Self::ResolvedValue {
        match self {
            Self::Auto if context.style.get_ui().has_initial_animation_timeline() => {
                Self::Time(Time::from_seconds(0.0f32))
            },
            _ => self,
        }
    }

    #[inline]
    fn from_resolved_value(value: Self::ResolvedValue) -> Self {
        value
    }
}
