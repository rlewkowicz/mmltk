// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use super::{Extension, ExtensionError, ExtensionType, MlsExtension};
use alloc::vec::Vec;
use core::ops::Deref;
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};

/// A collection of MLS [Extensions](super::Extension).
///
///
/// # Warning
///
/// Extension lists require that each type of extension has at most one entry.
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[derive(Debug, Clone, Default, MlsSize, MlsEncode, Eq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct ExtensionList(Vec<Extension>);

impl Deref for ExtensionList {
    type Target = Vec<Extension>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl PartialEq for ExtensionList {
    fn eq(&self, other: &Self) -> bool {
        self.len() == other.len()
            && self
                .iter()
                .all(|ext| other.get(ext.extension_type).as_ref() == Some(ext))
    }
}

impl MlsDecode for ExtensionList {
    fn mls_decode(reader: &mut &[u8]) -> Result<Self, mls_rs_codec::Error> {
        mls_rs_codec::iter::mls_decode_collection(reader, |data| {
            let mut list = ExtensionList::new();

            while !data.is_empty() {
                let ext = Extension::mls_decode(data)?;
                let ext_type = ext.extension_type;

                if list.0.iter().any(|e| e.extension_type == ext_type) {

                    return Err(mls_rs_codec::Error::Custom(1));
                }

                list.0.push(ext);
            }

            Ok(list)
        })
    }
}

impl From<Vec<Extension>> for ExtensionList {
    fn from(extensions: Vec<Extension>) -> Self {
        extensions.into_iter().collect()
    }
}

impl Extend<Extension> for ExtensionList {
    fn extend<T: IntoIterator<Item = Extension>>(&mut self, iter: T) {
        iter.into_iter().for_each(|ext| self.set(ext));
    }
}

impl FromIterator<Extension> for ExtensionList {
    fn from_iter<T: IntoIterator<Item = Extension>>(iter: T) -> Self {
        let mut list = Self::new();
        list.extend(iter);
        list
    }
}

impl ExtensionList {
    /// Create a new empty extension list.
    pub fn new() -> ExtensionList {
        Default::default()
    }

    /// Retrieve an extension by providing a type that implements the
    /// [MlsExtension](super::MlsExtension) trait.
    ///
    /// Returns an error if the underlying deserialization of the extension
    /// data fails.
    pub fn get_as<E: MlsExtension>(&self) -> Result<Option<E>, ExtensionError> {
        self.0
            .iter()
            .find(|e| e.extension_type == E::extension_type())
            .map(E::from_extension)
            .transpose()
    }

    /// Determine if a specific extension exists within the list.
    pub fn has_extension(&self, ext_id: ExtensionType) -> bool {
        self.0.iter().any(|e| e.extension_type == ext_id)
    }

    /// Set an extension in the list based on a provided type that implements
    /// the [MlsExtension](super::MlsExtension) trait.
    ///
    /// If there is already an entry in the list for the same extension type,
    /// then the prior value is removed as part of the insertion.
    ///
    /// This function will return an error if `ext` fails to serialize
    /// properly.
    pub fn set_from<E: MlsExtension>(&mut self, ext: E) -> Result<(), ExtensionError> {
        let ext = ext.into_extension()?;
        self.set(ext);
        Ok(())
    }

    /// Set an extension in the list based on a raw
    /// [Extension](super::Extension) value.
    ///
    /// If there is already an entry in the list for the same extension type,
    /// then the prior value is removed as part of the insertion.
    pub fn set(&mut self, ext: Extension) {
        let mut found = self
            .0
            .iter_mut()
            .find(|e| e.extension_type == ext.extension_type);

        if let Some(found) = found.take() {
            *found = ext;
        } else {
            self.0.push(ext);
        }
    }

    /// Get a raw [Extension](super::Extension) value based on an
    /// [ExtensionType](super::ExtensionType).
    pub fn get(&self, extension_type: ExtensionType) -> Option<Extension> {
        self.0
            .iter()
            .find(|e| e.extension_type == extension_type)
            .cloned()
    }

    /// Remove an extension from the list by
    /// [ExtensionType](super::ExtensionType)
    pub fn remove(&mut self, ext_type: ExtensionType) {
        self.0.retain(|e| e.extension_type != ext_type)
    }

    /// Append another extension list to this one.
    ///
    /// If there is already an entry in the list for the same extension type,
    /// then the existing value is removed.
    pub fn append(&mut self, others: Self) {
        self.extend(others.0);
    }
}
