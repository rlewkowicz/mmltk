use std::ops::Deref;

use syn::{Meta, Path};

use crate::ast::NestedMeta;
use crate::{Error, FromMeta, Result};

use super::path_to_string;

/// A list of `syn::Path` instances. This type is used to extract a list of paths from an
/// attribute.
///
/// # Usage
/// An `PathList` field on a struct implementing `FromMeta` will turn `#[builder(derive(serde::Debug, Clone))]` into:
///
/// ```rust,ignore
/// StructOptions {
///     derive: PathList(vec![syn::Path::new("serde::Debug"), syn::Path::new("Clone")])
/// }
/// ```
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct PathList(Vec<Path>);

impl PathList {
    /// Create a new list.
    pub fn new<T: Into<Path>>(vals: Vec<T>) -> Self {
        PathList(vals.into_iter().map(T::into).collect())
    }

    /// Create a new `Vec` containing the string representation of each path.
    pub fn to_strings(&self) -> Vec<String> {
        self.0.iter().map(path_to_string).collect()
    }
}

impl Deref for PathList {
    type Target = Vec<Path>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl From<Vec<Path>> for PathList {
    fn from(v: Vec<Path>) -> Self {
        PathList(v)
    }
}

impl FromMeta for PathList {
    fn from_list(v: &[NestedMeta]) -> Result<Self> {
        let mut paths = Vec::with_capacity(v.len());
        for nmi in v {
            if let NestedMeta::Meta(Meta::Path(ref path)) = *nmi {
                paths.push(path.clone());
            } else {
                return Err(Error::unexpected_type("non-word").with_span(nmi));
            }
        }

        Ok(PathList(paths))
    }
}
