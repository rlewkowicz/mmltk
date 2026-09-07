//! Comparison, arithmetic, and conversion between various types in `time` and the standard library.
//!
//! Currently, full interoperability is present between [`OffsetDateTime`](crate::OffsetDateTime),
//! [`UtcDateTime`](crate::UtcDateTime), and [`SystemTime`](std::time::SystemTime). Partial
//! interoperability is present with [`js_sys::Date`]. Note that
//! [`PrimitiveDateTime`](crate::PrimitiveDateTime) is not interoperable with any of these types due
//! to the lack of an associated UTC offset.


#[cfg(feature = "std")]
mod offsetdatetime_systemtime;
mod offsetdatetime_utcdatetime;
#[cfg(feature = "std")]
mod utcdatetime_systemtime;
