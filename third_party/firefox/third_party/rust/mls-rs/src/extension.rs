// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

pub use mls_rs_core::extension::{ExtensionType, MlsCodecExtension, MlsExtension};

pub(crate) use built_in::*;
#[cfg(feature = "last_resort_key_package_ext")]
pub(crate) use recommended::*;

/// Default extension types required by the MLS RFC.
pub mod built_in;

/// Extension types which are not mandatory, but still recommended.
#[cfg(feature = "last_resort_key_package_ext")]
pub mod recommended;
