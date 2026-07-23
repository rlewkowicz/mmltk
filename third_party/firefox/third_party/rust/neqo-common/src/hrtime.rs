// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    cell::RefCell,
    rc::{Rc, Weak},
    time::Duration,
};


/// A quantized `Duration`.  This currently just produces 16 discrete values
/// corresponding to whole milliseconds.  Future implementations might choose
/// a different allocation, such as a logarithmic scale.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct Period(u8);

impl Period {
    const MAX: Self = Self(16);
    const MIN: Self = Self(1);


}

impl From<Duration> for Period {
    fn from(p: Duration) -> Self {
        let rounded = u8::try_from(p.as_millis()).unwrap_or(Self::MAX.0);
        Self(rounded.clamp(Self::MIN.0, Self::MAX.0))
    }
}

/// This counts instances of `Period`, except those of `Period::MAX`.
#[derive(Default)]
struct PeriodSet {
    counts: [usize; (Period::MAX.0 - Period::MIN.0) as usize],
}

impl PeriodSet {
    fn idx(&mut self, p: Period) -> &mut usize {
        debug_assert!(p >= Period::MIN);
        &mut self.counts[usize::from(p.0 - Period::MIN.0)]
    }

    fn add(&mut self, p: Period) {
        if p != Period::MAX {
            *self.idx(p) += 1;
        }
    }

    fn remove(&mut self, p: Period) {
        if p != Period::MAX {
            let p = self.idx(p);
            debug_assert_ne!(*p, 0);
            *p -= 1;
        }
    }

    fn min(&self) -> Option<Period> {
        for (i, v) in self.counts.iter().enumerate() {
            if *v > 0 {
                return Some(Period(u8::try_from(i).ok()? + Period::MIN.0));
            }
        }
        None
    }
}


/// A handle for a high-resolution timer of a specific period.
pub struct Handle {
    hrt: Rc<RefCell<Time>>,
    active: Period,
    hysteresis: [Period; Self::HISTORY],
    hysteresis_index: usize,
}

impl Handle {
    const HISTORY: usize = 8;

    const fn new(hrt: Rc<RefCell<Time>>, active: Period) -> Self {
        Self {
            hrt,
            active,
            hysteresis: [Period::MAX; Self::HISTORY],
            hysteresis_index: 0,
        }
    }

    /// Update shortcut.  Equivalent to dropping the current reference and
    /// calling `HrTime::get` again with the new period, except that this applies
    /// a little hysteresis that smoothes out fluctuations.
    pub fn update(&mut self, period: Duration) {
        self.hysteresis[self.hysteresis_index] = Period::from(period);
        self.hysteresis_index += 1;
        self.hysteresis_index %= self.hysteresis.len();

        let mut first = Period::MAX;
        let mut second = Period::MAX;
        for i in &self.hysteresis {
            if *i < first {
                second = first;
                first = *i;
            } else if *i < second {
                second = *i;
            }
        }

        if second != self.active {
            let mut b = self.hrt.borrow_mut();
            b.periods.remove(self.active);
            self.active = second;
            b.periods.add(self.active);
            b.update();
        }
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        self.hrt.borrow_mut().remove(self.active);
    }
}

/// Holding an instance of this indicates that high resolution timers are enabled.
pub struct Time {
    periods: PeriodSet,
    active: Option<Period>,

#[cfg(any())]









    scale: f64,
#[cfg(any())]









    deflt: mac::thread_time_constraint_policy,
}
impl Time {
    fn new() -> Self {
        Self {
            periods: PeriodSet::default(),
            active: None,

#[cfg(any())]









            scale: mac::get_scale(),
#[cfg(any())]









            deflt: mac::get_default_policy(),
        }
    }



#[expect(
        clippy::unused_self,
        reason = "Not used on platforms other than macOS and Windows."
    )]
    const fn start(&self) {}


#[expect(
        clippy::unused_self,
        reason = "Not used on platforms other than Windows."
    )]
    const fn stop(&self) {}

    fn update(&mut self) {
        let next = self.periods.min();
        if next != self.active {
            self.stop();
            self.active = next;
            self.start();
        }
    }

    fn add(&mut self, p: Period) {
        self.periods.add(p);
        self.update();
    }

    fn remove(&mut self, p: Period) {
        self.periods.remove(p);
        self.update();
    }

    /// Enable high resolution time.  Returns a thread-bound handle that
    /// needs to be held until the high resolution time is no longer needed.
    /// The handle can also be used to update the resolution.
    #[must_use]
    pub fn get(period: Duration) -> Handle {
        thread_local!(static HR_TIME: RefCell<Weak<RefCell<Time>>> = RefCell::default());

        HR_TIME.with(|r| {
            let mut b = r.borrow_mut();
            let hrt = b.upgrade().unwrap_or_else(|| {
                let hrt = Rc::new(RefCell::new(Self::new()));
                *b = Rc::downgrade(&hrt);
                hrt
            });

            let p = Period::from(period);
            hrt.borrow_mut().add(p);
            Handle::new(hrt, p)
        })
    }
}

impl Drop for Time {
    fn drop(&mut self) {
        self.stop();

#[cfg(any())]









        {
            if self.active.is_some() {
                mac::set_thread_policy(self.deflt);
            }
        }
    }
}


