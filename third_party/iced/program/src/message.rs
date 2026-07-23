#[cfg(feature = "time-travel")]
pub trait MaybeClone: Clone {}

#[cfg(feature = "time-travel")]
impl<T> MaybeClone for T where T: Clone {}

#[cfg(not(feature = "time-travel"))]
pub trait MaybeClone {}

#[cfg(not(feature = "time-travel"))]
impl<T> MaybeClone for T {}

#[cfg(feature = "debug")]
pub trait MaybeDebug: std::fmt::Debug {}

#[cfg(feature = "debug")]
impl<T> MaybeDebug for T where T: std::fmt::Debug {}

#[cfg(not(feature = "debug"))]
pub trait MaybeDebug {}

#[cfg(not(feature = "debug"))]
impl<T> MaybeDebug for T {}
