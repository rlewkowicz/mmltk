// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    fmt::{self, Debug, Formatter},
    net::SocketAddr,
    num::NonZeroUsize,
    ops::{Deref, DerefMut},
};

use crate::{Bytes, Tos, hex_with_len};

/// A UDP datagram.
///
/// Guaranteed to not be empty.
#[derive(Clone, PartialEq, Eq)]
pub struct Datagram<D = Vec<u8>> {
    src: SocketAddr,
    dst: SocketAddr,
    tos: Tos,
    d: D,
}

impl TryFrom<Batch> for Datagram {
    type Error = ();

    fn try_from(d: Batch) -> Result<Self, Self::Error> {
        if d.num_datagrams() != 1 {
            return Err(());
        }
        Ok(Self {
            src: d.src,
            dst: d.dst,
            tos: d.tos,
            d: d.d,
        })
    }
}

impl<D> Datagram<D> {
    #[must_use]
    pub const fn source(&self) -> SocketAddr {
        self.src
    }

    #[must_use]
    pub const fn destination(&self) -> SocketAddr {
        self.dst
    }

    #[must_use]
    pub const fn tos(&self) -> Tos {
        self.tos
    }

    pub const fn set_tos(&mut self, tos: Tos) {
        self.tos = tos;
    }
}

impl<D: AsRef<[u8]>> Datagram<D> {
    #[expect(clippy::len_without_is_empty, reason = "is_empty() is always false")]
    #[must_use]
    pub fn len(&self) -> usize {
        self.d.as_ref().len()
    }

    #[must_use]
    pub fn to_owned(&self) -> Datagram {
        Datagram {
            src: self.src,
            dst: self.dst,
            tos: self.tos,
            d: self.d.as_ref().to_vec(),
        }
    }
}

impl<D: AsMut<[u8]> + AsRef<[u8]>> AsMut<[u8]> for Datagram<D> {
    fn as_mut(&mut self) -> &mut [u8] {
        self.d.as_mut()
    }
}

impl Datagram<Vec<u8>> {
    /// # Panics
    ///
    /// Panics if `d` converts to an empty vector.
    #[must_use]
    pub fn new<V: Into<Vec<u8>>>(src: SocketAddr, dst: SocketAddr, tos: Tos, d: V) -> Self {
        let d = d.into();
        assert!(!d.is_empty(), "Datagram data cannot be empty");
        Self { src, dst, tos, d }
    }
}

impl<D: AsRef<[u8]> + AsMut<[u8]>> DerefMut for Datagram<D> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        AsMut::<[u8]>::as_mut(self)
    }
}

impl<D: AsRef<[u8]>> Deref for Datagram<D> {
    type Target = [u8];
    fn deref(&self) -> &Self::Target {
        AsRef::<[u8]>::as_ref(self)
    }
}

impl<D: AsRef<[u8]>> Debug for Datagram<D> {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(
            f,
            "Datagram {:?} {:?}->{:?}: {}",
            self.tos,
            self.src,
            self.dst,
            hex_with_len(&self.d)
        )
    }
}

impl<'a> Datagram<&'a mut [u8]> {
    /// # Panics
    ///
    /// Panics if the data is empty.
    #[must_use]
    pub fn from_slice(src: SocketAddr, dst: SocketAddr, tos: Tos, d: &'a mut [u8]) -> Self {
        assert!(!d.is_empty(), "Datagram data cannot be empty");
        Self { src, dst, tos, d }
    }
}

impl Datagram<Bytes> {
    /// # Panics
    ///
    /// Panics if the data is empty.
    #[must_use]
    pub fn from_bytes(src: SocketAddr, dst: SocketAddr, tos: Tos, d: Bytes) -> Self {
        assert!(!d.is_empty(), "Datagram data cannot be empty");
        Self { src, dst, tos, d }
    }
}

impl<D: AsRef<[u8]>> AsRef<[u8]> for Datagram<D> {
    fn as_ref(&self) -> &[u8] {
        self.d.as_ref()
    }
}

/// A batch of [`Datagram`]s with the same metadata, e.g., destination.
///
/// Upholds Linux GSO requirement. That is, all but the last datagram in the
/// batch have the same size. The last datagram may be equal or smaller.
#[derive(Clone, PartialEq, Eq)]
pub struct Batch {
    src: SocketAddr,
    dst: SocketAddr,
    tos: Tos,
    datagram_size: NonZeroUsize,
    d: Vec<u8>,
}

impl Debug for Batch {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(
            f,
            "datagram::Batch {:?} {:?}->{:?} {:?}: {}",
            self.tos,
            self.src,
            self.dst,
            self.datagram_size,
            hex_with_len(&self.d)
        )
    }
}

impl From<Datagram<Vec<u8>>> for Batch {
    fn from(d: Datagram<Vec<u8>>) -> Self {
        Self {
            src: d.src,
            dst: d.dst,
            tos: d.tos,
            datagram_size: NonZeroUsize::new(d.d.len())
                .expect("Datagram is guaranteed to be non-empty"),
            d: d.d,
        }
    }
}

impl Batch {
    #[must_use]
    pub const fn new(
        src: SocketAddr,
        dst: SocketAddr,
        tos: Tos,
        datagram_size: NonZeroUsize,
        d: Vec<u8>,
    ) -> Self {
        Self {
            src,
            dst,
            tos,
            datagram_size,
            d,
        }
    }

    #[must_use]
    pub const fn source(&self) -> SocketAddr {
        self.src
    }

    #[must_use]
    pub const fn destination(&self) -> SocketAddr {
        self.dst
    }

    #[must_use]
    pub const fn tos(&self) -> Tos {
        self.tos
    }

    pub const fn set_tos(&mut self, tos: Tos) {
        self.tos = tos;
    }

    #[must_use]
    pub const fn datagram_size(&self) -> NonZeroUsize {
        self.datagram_size
    }

    #[must_use]
    pub fn data(&self) -> &[u8] {
        &self.d
    }

    #[must_use]
    pub const fn num_datagrams(&self) -> usize {
        self.d.len().div_ceil(self.datagram_size.get())
    }

    pub fn iter(&self) -> impl Iterator<Item = Datagram<&[u8]>> {
        self.d.chunks(self.datagram_size.get()).map(|d| Datagram {
            src: self.src,
            dst: self.dst,
            tos: self.tos,
            d,
        })
    }

    pub fn iter_mut(&mut self) -> impl Iterator<Item = Datagram<&mut [u8]>> {
        self.d
            .chunks_mut(self.datagram_size.get())
            .map(|d| Datagram {
                src: self.src,
                dst: self.dst,
                tos: self.tos,
                d,
            })
    }
}
