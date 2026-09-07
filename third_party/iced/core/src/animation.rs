use crate::time::{Duration, Instant};

pub use lilt::{Easing, FloatRepresentable as Float, Interpolable};

#[derive(Debug, Clone)]
pub struct Animation<T>
where
    T: Clone + Copy + PartialEq + Float,
{
    raw: lilt::Animated<T, Instant>,
    duration: Duration, 
}

impl<T> Animation<T>
where
    T: Clone + Copy + PartialEq + Float,
{
        pub fn new(state: T) -> Self {
        Self {
            raw: lilt::Animated::new(state),
            duration: Duration::from_millis(100),
        }
    }

                    pub fn easing(mut self, easing: Easing) -> Self {
        self.raw = self.raw.easing(easing);
        self
    }

        pub fn very_quick(self) -> Self {
        self.duration(Duration::from_millis(100))
    }

        pub fn quick(self) -> Self {
        self.duration(Duration::from_millis(200))
    }

        pub fn slow(self) -> Self {
        self.duration(Duration::from_millis(400))
    }

        pub fn very_slow(self) -> Self {
        self.duration(Duration::from_millis(500))
    }

        pub fn duration(mut self, duration: Duration) -> Self {
        self.raw = self.raw.duration(duration.as_secs_f32() * 1_000.0);
        self.duration = duration;
        self
    }

        pub fn delay(mut self, duration: Duration) -> Self {
        self.raw = self.raw.delay(duration.as_secs_f64() as f32 * 1000.0);
        self
    }

                pub fn repeat(mut self, repetitions: u32) -> Self {
        self.raw = self.raw.repeat(repetitions);
        self
    }

        pub fn repeat_forever(mut self) -> Self {
        self.raw = self.raw.repeat_forever();
        self
    }

        pub fn auto_reverse(mut self) -> Self {
        self.raw = self.raw.auto_reverse();
        self
    }

            pub fn go(mut self, new_state: T, at: Instant) -> Self {
        self.go_mut(new_state, at);
        self
    }

            pub fn go_mut(&mut self, new_state: T, at: Instant) {
        self.raw.transition(new_state, at);
    }

                pub fn is_animating(&self, at: Instant) -> bool {
        self.raw.in_progress(at)
    }

                            pub fn interpolate_with<I>(&self, f: impl Fn(T) -> I, at: Instant) -> I
    where
        I: Interpolable,
    {
        self.raw.animate(f, at)
    }

        pub fn value(&self) -> T {
        self.raw.value
    }
}

impl Animation<f32> {
        pub fn interpolate(&self, at: Instant) -> f32 {
        self.interpolate_with(std::convert::identity, at)
    }
}

impl Animation<bool> {
            pub fn interpolate<I>(&self, start: I, end: I, at: Instant) -> I
    where
        I: Interpolable + Clone,
    {
        self.raw.animate_bool(start, end, at)
    }

        pub fn remaining(&self, at: Instant) -> Duration {
        Duration::from_secs_f32(self.interpolate(self.duration.as_secs_f32(), 0.0, at))
    }
}
