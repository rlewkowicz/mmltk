// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

#![expect(
    clippy::unwrap_used,
    reason = "Let's assume the use of `unwrap` was checked when the use of `unsafe` was reviewed."
)]

use std::{
    convert::{TryFrom as _, TryInto as _},
    ops::Deref,
    os::raw::c_void,
    pin::Pin,
    time::{Duration, Instant},
};

use once_cell::sync::OnceCell;

use crate::{
    agent::as_c_void,
    err::{Error, Res},
    prio::PRFileDesc,
    ssl::SSLTimeFunc,
};

include!(concat!(env!("OUT_DIR"), "/nspr_time.rs"));

experimental_api!(SSL_SetTimeFunc(
    fd: *mut PRFileDesc,
    cb: SSLTimeFunc,
    arg: *mut c_void,
));

/// This struct holds the zero time used for converting between `Instant` and `PRTime`.
#[derive(Debug)]
struct TimeZero {
    instant: Instant,
    prtime: PRTime,
}

impl TimeZero {
    /// This function sets a baseline from an instance of `Instant`.
    /// This allows for the possibility that code that uses these APIs will create
    /// instances of `Instant` before any of this code is run.  If `Instant`s older than
    /// `BASE_TIME` are used with these conversion functions, they will fail.
    /// To avoid that, we make sure that this sets the base time using the first value
    /// it sees if it is in the past.  If it is not, then use `Instant::now()` instead.
    pub fn baseline(t: Instant) -> Self {
        #[expect(
            clippy::disallowed_methods,
            reason = "Special handling for NSPR time conversion"
        )]
        let now = Instant::now();
        let prnow = unsafe { PR_Now() };

        if now <= t {
            Self {
                instant: now,
                prtime: prnow,
            }
        } else {
            let elapsed = Interval::from(now.duration_since(now));
            let prelapsed: PRTime = elapsed.try_into().unwrap();
            Self {
                instant: t,
                prtime: prnow.checked_sub(prelapsed).unwrap(),
            }
        }
    }
}

static BASE_TIME: OnceCell<TimeZero> = OnceCell::new();

fn get_base() -> &'static TimeZero {
    BASE_TIME.get_or_init(|| TimeZero {
        #[expect(
            clippy::disallowed_methods,
            reason = "Special handling for NSPR time conversion"
        )]
        instant: Instant::now(),
        prtime: unsafe { PR_Now() },
    })
}

pub fn init() {
    _ = get_base();
}

/// Time wraps Instant and provides conversion functions into `PRTime`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Time {
    t: Instant,
}

impl Deref for Time {
    type Target = Instant;
    fn deref(&self) -> &Self::Target {
        &self.t
    }
}

impl From<Instant> for Time {
    /// Convert from an Instant into a Time.
    fn from(t: Instant) -> Self {
        BASE_TIME.get_or_init(|| TimeZero::baseline(t));
        Self { t }
    }
}

impl TryFrom<PRTime> for Time {
    type Error = Error;
    fn try_from(prtime: PRTime) -> Res<Self> {
        let base = get_base();
        let delta = prtime.checked_sub(base.prtime).ok_or(Error::TimeTravel)?;
        let d = Duration::from_micros(u64::try_from(delta.abs())?);
        let t = if delta >= 0 {
            base.instant.checked_add(d)
        } else {
            base.instant.checked_sub(d)
        };
        let t = t.ok_or(Error::TimeTravel)?;
        Ok(Self { t })
    }
}

impl TryInto<PRTime> for Time {
    type Error = Error;
    fn try_into(self) -> Res<PRTime> {
        let base = get_base();

        self.t.checked_duration_since(base.instant).map_or_else(
            || {
                let backwards = base.instant - self.t; 
                PRTime::try_from(backwards.as_micros()).map_or(Err(Error::TimeTravel), |d| {
                    base.prtime.checked_sub(d).ok_or(Error::TimeTravel)
                })
            },
            |delta| {
                PRTime::try_from(delta.as_micros()).map_or(Err(Error::TimeTravel), |d| {
                    d.checked_add(base.prtime).ok_or(Error::TimeTravel)
                })
            },
        )
    }
}

impl From<Time> for Instant {
    fn from(t: Time) -> Self {
        t.t
    }
}

/// Interval wraps Duration and provides conversion functions into `PRTime`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Interval {
    d: Duration,
}

impl TryFrom<PRTime> for Interval {
    type Error = Error;
    fn try_from(prtime: PRTime) -> Res<Self> {
        Ok(Self {
            d: Duration::from_micros(u64::try_from(prtime)?),
        })
    }
}

impl From<Duration> for Interval {
    fn from(d: Duration) -> Self {
        Self { d }
    }
}

impl TryInto<PRTime> for Interval {
    type Error = Error;
    fn try_into(self) -> Res<PRTime> {
        Ok(PRTime::try_from(self.d.as_micros())?)
    }
}

/// `TimeHolder` maintains a `PRTime` value in a form that is accessible to the TLS stack.
#[derive(Debug)]
pub struct TimeHolder {
    t: Pin<Box<PRTime>>,
}

impl TimeHolder {
    const unsafe extern "C" fn time_func(arg: *mut c_void) -> PRTime {
        let p = arg as *const PRTime;
        unsafe { *p.as_ref().unwrap() }
    }

    #[expect(clippy::not_unsafe_ptr_arg_deref)]
    pub fn bind(&mut self, fd: *mut PRFileDesc) -> Res<()> {
        unsafe { SSL_SetTimeFunc(fd, Some(Self::time_func), as_c_void(&mut self.t)) }
    }

    pub fn set(&mut self, t: Instant) -> Res<()> {
        *self.t = Time::from(t).try_into()?;
        Ok(())
    }
}

impl Default for TimeHolder {
    fn default() -> Self {
        Self { t: Box::pin(0) }
    }
}
