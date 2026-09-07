//! The implementation for Version 7 UUIDs.
//!
//! Note that you need to enable the `v7` Cargo feature
//! in order to use this module.

use crate::{std::convert::TryInto, rng, timestamp::Timestamp, Builder, Uuid};

impl Uuid {
    /// Create a new version 7 UUID using the current time value and random bytes.
    ///
    /// This method is a convenient alternative to [`Uuid::new_v7`] that uses the current system time
    /// as the source timestamp.
    #[cfg(feature = "std")]
    pub fn now_v7() -> Self {
        Self::new_v7(Timestamp::now(crate::NoContext))
    }

    /// Create a new version 7 UUID using a time value and random bytes.
    ///
    /// When the `std` feature is enabled, you can also use [`Uuid::now_v7`].
    ///
    /// Note that usage of this method requires the `v7` feature of this crate
    /// to be enabled.
    ///
    /// Also see [`Uuid::now_v7`] for a convenient way to generate version 7
    /// UUIDs using the current system time.
    ///
    /// # Examples
    ///
    /// A v7 UUID can be created from a unix [`Timestamp`] plus a 128 bit
    /// random number. When supplied as such, the data will be
    ///
    /// ```rust
    /// # use uuid::{Uuid, Timestamp, NoContext};
    /// let ts = Timestamp::from_unix(NoContext, 1497624119, 1234);
    ///
    /// let uuid = Uuid::new_v7(ts);
    ///
    /// assert!(
    ///     uuid.hyphenated().to_string().starts_with("015cb15a-86d8-7")
    /// );
    /// ```
    ///
    /// # References
    ///
    /// * [Version 7 UUIDs in Draft RFC: New UUID Formats, Version 4](https://datatracker.ietf.org/doc/html/draft-peabody-dispatch-new-uuid-format-04#section-5.2)
    pub fn new_v7(ts: Timestamp) -> Self {
        let (secs, nanos) = ts.to_unix();
        let millis = (secs * 1000).saturating_add(nanos as u64 / 1_000_000);

        Builder::from_unix_timestamp_millis(millis, &rng::bytes()[..10].try_into().unwrap())
            .into_uuid()
    }
}
