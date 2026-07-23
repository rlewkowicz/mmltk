use crate::{Builder, Uuid};

impl Uuid {
    /// Creates a custom UUID comprised almost entirely of user-supplied bytes.
    ///
    /// This will inject the UUID Version at 4 bits starting at the 48th bit
    /// and the Variant into 2 bits 64th bit. Any existing bits in the user-supplied bytes
    /// at those locations will be overridden.
    ///
    /// Note that usage of this method requires the `v8` feature of this crate
    /// to be enabled.
    ///
    /// # Examples
    ///
    /// Basic usage:
    ///
    /// ```
    /// # use uuid::{Uuid, Version};
    /// let buf: [u8; 16] = *b"abcdefghijklmnop";
    /// let uuid = Uuid::new_v8(buf);
    ///
    /// assert_eq!(Some(Version::Custom), uuid.get_version());
    /// ```
    ///
    /// # References
    ///
    /// * [Version 8 UUIDs in Draft RFC: New UUID Formats, Version 4](https://datatracker.ietf.org/doc/html/draft-peabody-dispatch-new-uuid-format-04#section-5.3)
    pub fn new_v8(buf: [u8; 16]) -> Uuid {
        Builder::from_custom_bytes(buf).into_uuid()
    }
}
