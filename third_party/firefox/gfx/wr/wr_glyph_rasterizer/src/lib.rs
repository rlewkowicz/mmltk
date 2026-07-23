/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! A glyph rasterizer for webrender
//!
//! ## Overview
//!
//! ## Usage
//!

#![allow(unknown_lints, mismatched_lifetime_syntaxes)]

mod gamma_lut;
mod rasterizer;
mod types;

pub mod timing;

pub use rasterizer::*;
pub use types::*;

#[macro_use]
extern crate malloc_size_of_derive;
#[macro_use]
extern crate log;
#[macro_use]
extern crate lazy_static;
#[macro_use]
extern crate smallvec;

#[cfg(feature = "serde")]
#[macro_use]
extern crate serde;

extern crate malloc_size_of;

pub mod platform {

    pub use crate::platform::unix::font;


    pub mod unix {
        pub mod font;
    }
}
