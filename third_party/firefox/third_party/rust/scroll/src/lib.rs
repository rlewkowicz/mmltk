//! # Scroll
//!
//! ```text, no_run
//!         _______________
//!    ()==(              (@==()
//!         '______________'|
//!           |             |
//!           |   ἀρετή     |
//!         __)_____________|
//!    ()==(               (@==()
//!         '--------------'
//!
//! ```
//!
//! Scroll is a library for easily and efficiently reading/writing types from data containers like
//! byte arrays.
//!
//! ## Easily:
//!
//! Scroll sets down a number of traits:
//!
//! [FromCtx](ctx/trait.FromCtx.html), [IntoCtx](ctx/trait.IntoCtx.html),
//! [TryFromCtx](ctx/trait.TryFromCtx.html) and [TryIntoCtx](ctx/trait.TryIntoCtx.html) — further
//! explained in the [ctx module](ctx/index.html); to be implemented on custom types to allow
//! reading, writing, and potentially fallible reading/writing respectively.
//!
//! [Pread](trait.Pread.html) and [Pwrite](trait.Pwrite.html) which are implemented on data
//! containers such as byte arrays to define how to read or respectively write types implementing
//! the *Ctx traits above.
//! In addition scroll also defines [IOread](trait.IOread.html) and
//! [IOwrite](trait.IOwrite.html) with additional constraits that then allow reading and writing
//! from `std::io` [Read](https://doc.rust-lang.org/nightly/std/io/trait.Read.html) and
//! [Write](https://doc.rust-lang.org/nightly/std/io/trait.Write.html).
//!
//!
//! In most cases you can use [scroll_derive](https://docs.rs/scroll_derive) to derive sensible
//! defaults for `Pread`, `Pwrite`, their IO counterpart and `SizeWith`.  More complex situations
//! call for manual implementation of those traits; refer to [the ctx module](ctx/index.html) for
//! details.
//!
//!
//! ## Efficiently:
//!
//! Reading Slices — including [&str](https://doc.rust-lang.org/std/primitive.str.html) — supports
//! zero-copy. Scroll is designed with a `no_std` context in mind; every dependency on `std` is
//! cfg-gated and errors need not allocate.
//!
//! Reads by default take only immutable references wherever possible, allowing for trivial
//! parallelization.
//!
//! # Examples
//!
//! Let's start with a simple example
//!
//! ```rust
//! use scroll::{ctx, Pread};
//!
//! // Let's first define some data, cfg-gated so our assertions later on hold.
//! #[cfg(target_endian = "little")]
//! let bytes: [u8; 4] = [0xde, 0xad, 0xbe, 0xef];
//! #[cfg(target_endian = "big")]
//! let bytes: [u8; 4] = [0xef, 0xbe, 0xad, 0xde];
//!
//! // We can read a u32 from the array `bytes` at offset 0.
//! // This will use a default context for the type being parsed;
//! // in the case of u32 this defines to use the host's endianess.
//! let number = bytes.pread::<u32>(0).unwrap();
//! assert_eq!(number, 0xefbeadde);
//!
//!
//! // Similarly we can also read a single byte at offset 2
//! // This time using type ascription instead of the turbofish (::<>) operator.
//! let byte: u8 = bytes.pread(2).unwrap();
//! #[cfg(target_endian = "little")]
//! assert_eq!(byte, 0xbe);
//! #[cfg(target_endian = "big")]
//! assert_eq!(byte, 0xad);
//!
//!
//! // If required we can also provide a specific parsing context; e.g. if we want to explicitly
//! // define the endianess to use:
//! let be_number: u32 = bytes.pread_with(0, scroll::BE).unwrap();
//! #[cfg(target_endian = "little")]
//! assert_eq!(be_number, 0xdeadbeef);
//! #[cfg(target_endian = "big")]
//! assert_eq!(be_number, 0xefbeadde);
//!
//! let be_number16 = bytes.pread_with::<u16>(1, scroll::BE).unwrap();
//! #[cfg(target_endian = "little")]
//! assert_eq!(be_number16, 0xadbe);
//! #[cfg(target_endian = "big")]
//! assert_eq!(be_number16, 0xbead);
//!
//!
//! // Reads may fail; in this example due to a too large read for the given container.
//! // Scroll's error type does not by default allocate to work in environments like no_std.
//! let byte_err: scroll::Result<i64> = bytes.pread(0);
//! assert!(byte_err.is_err());
//!
//!
//! // We can parse out custom datatypes, or types with lifetimes, as long as they implement
//! // the conversion traits `TryFromCtx/FromCtx`.
//! // Here we use the default context for &str which parses are C-style '\0'-delimited string.
//! let hello: &[u8] = b"hello world\0more words";
//! let hello_world: &str = hello.pread(0).unwrap();
//! assert_eq!("hello world", hello_world);
//!
//! // We can again provide a custom context; for example to parse Space-delimited strings.
//! // As you can see while we still call `pread` changing the context can influence the output —
//! // instead of splitting at '\0' we split at spaces
//! let hello2: &[u8] = b"hello world\0more words";
//! let world: &str = hello2.pread_with(6, ctx::StrCtx::Delimiter(ctx::SPACE)).unwrap();
//! assert_eq!("world\0more", world);
//! ```
//!
//! ## `std::io` API
//!
//! Scroll also allows reading from `std::io`. For this the types to read need to implement
//! [FromCtx](ctx/trait.FromCtx.html) and [SizeWith](ctx/trait.SizeWith.html).
//!
//! ```rust
//! ##[cfg(feature = "std")] {
//! use std::io::Cursor;
//! use scroll::{IOread, ctx, Endian};
//! let bytes = [0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0xef,0xbe,0x00,0x00,];
//! let mut cursor = Cursor::new(bytes);
//!
//! // IOread uses std::io::Read methods, thus the Cursor will be incremented on these reads:
//! let prev = cursor.position();
//!
//! let integer = cursor.ioread_with::<u64>(Endian::Little).unwrap();
//!
//! let after = cursor.position();
//!
//! assert!(prev < after);
//!
//! // SizeWith allows us to define a context-sensitive size of a read type:
//! // Contexts can have different instantiations; e.g. the `Endian` context can be either Little or
//! // Big. This is useful if for example the context contains the word-size of fields to be
//! // read/written, e.g. switching between ELF32 or ELF64 at runtime.
//! let size = <u64 as ctx::SizeWith<Endian>>::size_with(&Endian::Little) as u64;
//! assert_eq!(prev + size, after);
//! # }
//! ```
//!
//! In the same vein as IOread we can use IOwrite to write a type to anything implementing
//! `std::io::Write`:
//!
//! ```rust
//! ##[cfg(feature = "std")] {
//! use std::io::Cursor;
//! use scroll::{IOwrite};
//!
//! let mut bytes = [0x0u8; 5];
//! let mut cursor = Cursor::new(&mut bytes[..]);
//!
//! // This of course once again increments the cursor position
//! cursor.iowrite_with(0xdeadbeef as u32, scroll::BE).unwrap();
//!
//! assert_eq!(cursor.into_inner(), [0xde, 0xad, 0xbe, 0xef, 0x0]);
//! # }
//! ```
//!
//! ## Complex use cases
//!
//! Scoll is designed to be highly adaptable while providing a strong abstraction between the types
//! being read/written and the data container containing them.
//!
//! In this example we'll define a custom Data and allow it to be read from an arbitrary byte
//! buffer.
//!
//! ```rust
//! use scroll::{self, ctx, Pread, Endian};
//! use scroll::ctx::StrCtx;
//!
//! // Our custom context type. In a more complex situation you could for example store details on
//! // how to write or read your type, field-sizes or other information.
//! // In this simple example we could also do without using a custom context in the first place.
//! #[derive(Copy, Clone)]
//! struct Context(Endian);
//!
//! // Our custom data type
//! struct Data<'zerocopy> {
//!   // This is only a reference to the actual data; we make use of scroll's zero-copy capability
//!   name: &'zerocopy str,
//!   id: u32,
//! }
//!
//! // To allow for safe zero-copying scroll allows to specify lifetimes explicitly:
//! // The context
//! impl<'a> ctx::TryFromCtx<'a, Context> for Data<'a> {
//!   // If necessary you can set a custom error type here, which will be returned by Pread/Pwrite
//!   type Error = scroll::Error;
//!
//!   // Using the explicit lifetime specification again you ensure that read data doesn't outlife
//!   // its source buffer without having to resort to copying.
//!   fn try_from_ctx (src: &'a [u8], ctx: Context)
//!     // the `usize` returned here is the amount of bytes read.
//!     -> Result<(Self, usize), Self::Error>
//!   {
//!     let offset = &mut 0;
//!
//!     let id = src.gread_with(offset, ctx.0)?;
//!
//!     // In a more serious application you would validate data here of course.
//!     let namelen: u16 = src.gread_with(offset, ctx.0)?;
//!     let name = src.gread_with::<&str>(offset, StrCtx::Length(namelen as usize))?;
//!
//!     Ok((Data { name: name, id: id }, *offset))
//!   }
//! }
//!
//! // In lieu of a complex byte buffer we hearken back to a simple &[u8]; the default source
//! // of TryFromCtx. However, any type that implements Pread to produce a &[u8] can now read
//! // `Data` thanks to it's implementation of TryFromCtx.
//! let bytes = b"\x01\x02\x03\x04\x00\x08UserName";
//! let data: Data = bytes.pread_with(0, Context(Endian::Big)).unwrap();
//!
//! assert_eq!(data.id, 0x01020304);
//! assert_eq!(data.name.to_string(), "UserName".to_string());
//! ```
//!
//! For further explanation of the traits and how to implement them manually refer to
//! [Pread](trait.Pread.html) and [TryFromCtx](ctx/trait.TryFromCtx.html).

#![cfg_attr(not(feature = "std"), no_std)]

#[cfg(feature = "derive")]
#[allow(unused_imports)]
pub use scroll_derive::{IOread, IOwrite, Pread, Pwrite, SizeWith};

#[cfg(feature = "std")]
extern crate core;

pub mod ctx;
mod endian;
mod error;
mod greater;
mod leb128;
#[cfg(feature = "std")]
mod lesser;
mod pread;
mod pwrite;

pub use crate::endian::*;
pub use crate::error::*;
pub use crate::greater::*;
pub use crate::leb128::*;
#[cfg(feature = "std")]
pub use crate::lesser::*;
pub use crate::pread::*;
pub use crate::pwrite::*;

#[doc(hidden)]
pub mod export {
    pub use ::core::{mem, result};
}

#[allow(unused)]
macro_rules! doc_comment {
    ($x:expr) => {
        #[doc = $x]
        #[doc(hidden)]
        mod readme_tests {}
    };
}
