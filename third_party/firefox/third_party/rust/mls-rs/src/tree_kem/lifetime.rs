// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::{client::MlsError, time::MlsTime};
use core::time::Duration;
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};

#[derive(Clone, Debug, PartialEq, Eq, MlsSize, MlsEncode, MlsDecode, Default)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[non_exhaustive]
pub struct Lifetime {
    pub not_before: MlsTime,
    pub not_after: MlsTime,
}

impl Lifetime {
    pub fn new(not_before: MlsTime, not_after: MlsTime) -> Lifetime {
        Lifetime {
            not_before,
            not_after,
        }
    }

    pub fn seconds(s: u64, maybe_not_before: Option<MlsTime>) -> Result<Self, MlsError> {
        #[cfg(feature = "std")]
        let not_before = MlsTime::now();
        #[cfg(not(feature = "std"))]
        let not_before = MlsTime::from(3600u64);

        let not_before = if let Some(not_before_time) = maybe_not_before {
            not_before_time
        } else {
            not_before
        };

        let not_after = MlsTime::from(
            not_before
                .seconds_since_epoch()
                .checked_add(s)
                .ok_or(MlsError::TimeOverflow)?,
        );

        Ok(Lifetime {
            not_before: not_before - Duration::from_secs(3600),
            not_after,
        })
    }

    pub fn days(d: u32, maybe_not_before: Option<MlsTime>) -> Result<Self, MlsError> {
        Self::seconds((d * 86400) as u64, maybe_not_before)
    }

    pub fn years(y: u8, maybe_not_before: Option<MlsTime>) -> Result<Self, MlsError> {
        Self::days(365 * y as u32, maybe_not_before)
    }

    pub(crate) fn within_lifetime(&self, time: MlsTime) -> bool {
        self.not_before <= time && time <= self.not_after
    }
}
