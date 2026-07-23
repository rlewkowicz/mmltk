use crate::Uuid;

impl Uuid {
    /// Creates a UUID using a name from a namespace, based on the MD5
    /// hash.
    ///
    /// A number of namespaces are available as constants in this crate:
    ///
    /// * [`NAMESPACE_DNS`]
    /// * [`NAMESPACE_OID`]
    /// * [`NAMESPACE_URL`]
    /// * [`NAMESPACE_X500`]
    ///
    /// Note that usage of this method requires the `v3` feature of this crate
    /// to be enabled.
    ///
    /// # Examples
    ///
    /// Generating a MD5 DNS UUID for `rust-lang.org`:
    ///
    /// ```
    /// # use uuid::{Uuid, Version};
    /// let uuid = Uuid::new_v3(&Uuid::NAMESPACE_DNS, b"rust-lang.org");
    ///
    /// assert_eq!(Some(Version::Md5), uuid.get_version());
    /// ```
    ///
    /// # References
    ///
    /// * [Version 3 and 5 UUIDs in RFC4122](https://www.rfc-editor.org/rfc/rfc4122#section-4.3)
    ///
    /// [`NAMESPACE_DNS`]: #associatedconstant.NAMESPACE_DNS
    /// [`NAMESPACE_OID`]: #associatedconstant.NAMESPACE_OID
    /// [`NAMESPACE_URL`]: #associatedconstant.NAMESPACE_URL
    /// [`NAMESPACE_X500`]: #associatedconstant.NAMESPACE_X500
    pub fn new_v3(namespace: &Uuid, name: &[u8]) -> Uuid {
        crate::Builder::from_md5_bytes(crate::md5::hash(namespace.as_bytes(), name)).into_uuid()
    }
}
