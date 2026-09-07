// musl as a whole is licensed under the following standard MIT license:
// Copyright © 2005-2020 Rich Felker, et al.
// Permission is hereby granted, free of charge, to any person obtaining
// distribute, sublicense, and/or sell copies of the Software, and to
// The above copyright notice and this permission notice shall be
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// Portions of this software are derived from third-party works licensed
// under terms compatible with the above MIT license:
// src/regex/tre*) is Copyright © 2001-2008 Ville Laurikari and licensed
// under a 2-clause BSD license (license text in the source files). The
// Copyright © 1993,2004 Sun Microsystems or
// Copyright © 2003-2011 David Schultz or
// Copyright © 2003-2009 Steven G. Kargl or
// Copyright © 2003-2009 Bruce D. Evans or
// Copyright © 2008 Stephen L. Moshier or
// Copyright © 2017-2018 Arm Limited
// have been licensed under extremely permissive terms.
// The ARM memcpy code (src/string/arm/memcpy.S) is Copyright © 2008
// The Android Open Source Project and is licensed under a two-clause BSD
// license. It was taken from Bionic libc, used on Android.
// Copyright © 1999-2019, Arm Limited.
// Copyright © 1994 David Burren. It is licensed under a BSD license.
// domain. The code also comes with a fallback permissive license for use
// The smoothsort implementation (src/stdlib/qsort.c) is Copyright © 2011
// Valentin Ochs and is licensed under an MIT-style license.
// The x86_64 port was written by Nicholas J. Kain and is licensed under
// integration. It is licensed under the standard MIT terms.
// licensed under the standard MIT terms.
// and later supplemented and integrated by John Spencer. It is licensed
// All other files which have no copyright comments are original works
// omission of copyright and license comments in each file is in the
// the copyright notice and permission notice otherwise required by the
// license, and to use these files without any requirement of
// to be subject to copyright, resulting in confusion over whether it
// negated the permissions granted in the license. In the spirit of


use std::fmt;

/// A date/time type which exists primarily to convert `SystemTime` timestamps into an ISO 8601
/// formatted string.
///
/// Yes, this exists. Before you have a heart attack, understand that the meat of this is musl's
/// [`__secs_to_tm`][1] converted to Rust via [c2rust][2] and then cleaned up by hand as part of
/// the [kudu-rs project][3], [released under MIT][4].
///
/// [1] http://git.musl-libc.org/cgit/musl/tree/src/time/__secs_to_tm.c
/// [2] https://c2rust.com/
/// [3] https://github.com/danburkert/kudu-rs/blob/c9660067e5f4c1a54143f169b5eeb49446f82e54/src/timestamp.rs#L5-L18
/// [4] https://github.com/tokio-rs/tracing/issues/1644#issuecomment-963888244
///
/// All existing `strftime`-like APIs I found were unable to handle the full range of timestamps representable
/// by `SystemTime`, including `strftime` itself, since tm.tm_year is an int.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct DateTime {
    year: i64,
    month: u8,
    day: u8,
    hour: u8,
    minute: u8,
    second: u8,
    nanos: u32,
}

impl fmt::Display for DateTime {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.year > 9999 {
            write!(f, "+{}", self.year)?;
        } else if self.year < 0 {
            write!(f, "{:05}", self.year)?;
        } else {
            write!(f, "{:04}", self.year)?;
        }

        write!(
            f,
            "-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z",
            self.month,
            self.day,
            self.hour,
            self.minute,
            self.second,
            self.nanos / 1_000
        )
    }
}

impl From<std::time::SystemTime> for DateTime {
    fn from(timestamp: std::time::SystemTime) -> DateTime {
        let (t, nanos) = match timestamp.duration_since(std::time::UNIX_EPOCH) {
            Ok(duration) => {
                debug_assert!(duration.as_secs() <= i64::MAX as u64);
                (duration.as_secs() as i64, duration.subsec_nanos())
            }
            Err(error) => {
                let duration = error.duration();
                debug_assert!(duration.as_secs() <= i64::MAX as u64);
                let (secs, nanos) = (duration.as_secs() as i64, duration.subsec_nanos());
                if nanos == 0 {
                    (-secs, 0)
                } else {
                    (-secs - 1, 1_000_000_000 - nanos)
                }
            }
        };

        const LEAPOCH: i64 = 946_684_800 + 86400 * (31 + 29);
        const DAYS_PER_400Y: i32 = 365 * 400 + 97;
        const DAYS_PER_100Y: i32 = 365 * 100 + 24;
        const DAYS_PER_4Y: i32 = 365 * 4 + 1;
        static DAYS_IN_MONTH: [i8; 12] = [31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 31, 29];

        let mut days: i64 = (t / 86_400) - (LEAPOCH / 86_400);
        let mut remsecs: i32 = (t % 86_400) as i32;
        if remsecs < 0i32 {
            remsecs += 86_400;
            days -= 1
        }

        let mut qc_cycles: i32 = (days / i64::from(DAYS_PER_400Y)) as i32;
        let mut remdays: i32 = (days % i64::from(DAYS_PER_400Y)) as i32;
        if remdays < 0 {
            remdays += DAYS_PER_400Y;
            qc_cycles -= 1;
        }

        let mut c_cycles: i32 = remdays / DAYS_PER_100Y;
        if c_cycles == 4 {
            c_cycles -= 1;
        }
        remdays -= c_cycles * DAYS_PER_100Y;

        let mut q_cycles: i32 = remdays / DAYS_PER_4Y;
        if q_cycles == 25 {
            q_cycles -= 1;
        }
        remdays -= q_cycles * DAYS_PER_4Y;

        let mut remyears: i32 = remdays / 365;
        if remyears == 4 {
            remyears -= 1;
        }
        remdays -= remyears * 365;

        let mut years: i64 = i64::from(remyears)
            + 4 * i64::from(q_cycles)
            + 100 * i64::from(c_cycles)
            + 400 * i64::from(qc_cycles);

        let mut months: i32 = 0;
        while i32::from(DAYS_IN_MONTH[months as usize]) <= remdays {
            remdays -= i32::from(DAYS_IN_MONTH[months as usize]);
            months += 1
        }

        if months >= 10 {
            months -= 12;
            years += 1;
        }

        DateTime {
            year: years + 2000,
            month: (months + 3) as u8,
            day: (remdays + 1) as u8,
            hour: (remsecs / 3600) as u8,
            minute: (remsecs / 60 % 60) as u8,
            second: (remsecs % 60) as u8,
            nanos,
        }
    }
}
