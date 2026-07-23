use crate::Uuid;

impl Uuid {
    /// Creates a UUID using a name from a namespace, based on the SHA-1 hash.
    ///
    /// A number of namespaces are available as constants in this crate:
    ///
    /// * [`NAMESPACE_DNS`]
    /// * [`NAMESPACE_OID`]
    /// * [`NAMESPACE_URL`]
    /// * [`NAMESPACE_X500`]
    ///
    /// Note that usage of this method requires the `v5` feature of this crate
    /// to be enabled.
    ///
    /// # Examples
    ///
    /// Generating a SHA1 DNS UUID for `rust-lang.org`:
    ///
    /// ```
    /// # use uuid::{Uuid, Version};
    /// let uuid = Uuid::new_v5(&Uuid::NAMESPACE_DNS, b"rust-lang.org");
    ///
    /// assert_eq!(Some(Version::Sha1), uuid.get_version());
    /// ```
    ///
    /// # References
    ///
    /// * [Version 3 and 5 UUIDs in RFC4122](https://www.rfc-editor.org/rfc/rfc4122#section-4.3)
    ///
    /// [`NAMESPACE_DNS`]: struct.Uuid.html#associatedconst.NAMESPACE_DNS
    /// [`NAMESPACE_OID`]: struct.Uuid.html#associatedconst.NAMESPACE_OID
    /// [`NAMESPACE_URL`]: struct.Uuid.html#associatedconst.NAMESPACE_URL
    /// [`NAMESPACE_X500`]: struct.Uuid.html#associatedconst.NAMESPACE_X500
    pub fn new_v5(namespace: &Uuid, name: &[u8]) -> Uuid {
        crate::Builder::from_sha1_bytes(crate::sha1::hash(namespace.as_bytes(), name)).into_uuid()
    }
}
