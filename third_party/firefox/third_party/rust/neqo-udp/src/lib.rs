// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

#![cfg_attr(all(coverage_nightly, test), feature(coverage_attribute))]
#![expect(
    clippy::missing_errors_doc,
    reason = "Functions simply delegate to tokio and quinn-udp."
)]

use std::{
    array,
    io::{self, IoSliceMut},
    iter,
    net::SocketAddr,
    slice::{self, ChunksMut},
};

use log::{Level, log_enabled};
use neqo_common::{Datagram, Tos, datagram, qdebug, qtrace};
use quinn_udp::{EcnCodepoint, RecvMeta, Transmit, UdpSocketState};

/// Receive buffer size
///
/// Fits a maximum size UDP datagram, or, on platforms with segmentation
/// offloading, multiple smaller datagrams.
const RECV_BUF_SIZE: usize = u16::MAX as usize;

/// The number of buffers to pass to the OS on [`Socket::recv`].
///
/// Platforms without segmentation offloading, i.e. platforms not able to read
/// multiple datagrams into a single buffer, can benefit from using multiple
/// buffers instead.
///
/// Platforms with segmentation offloading have not shown performance
/// improvements when additionally using multiple buffers.
///
/// - Linux/Android: use segmentation offloading via GRO
/// - Windows: use segmentation offloading via URO (caveat see <https://github.com/quinn-rs/quinn/issues/2041>)
/// - Apple: no segmentation offloading available, use multiple buffers
#[cfg(not(apple))]
const NUM_BUFS: usize = 1;
#[cfg(apple)]
const NUM_BUFS: usize = 16;

/// A UDP receive buffer.
pub struct RecvBuf(Vec<Vec<u8>>);

impl Default for RecvBuf {
    fn default() -> Self {
        Self(vec![vec![0; RECV_BUF_SIZE]; NUM_BUFS])
    }
}

pub fn send_inner(
    state: &UdpSocketState,
    socket: quinn_udp::UdpSockRef<'_>,
    d: &datagram::Batch,
) -> io::Result<()> {
    let transmit = Transmit {
        destination: d.destination(),
        ecn: EcnCodepoint::from_bits(Into::<u8>::into(d.tos())),
        contents: d.data(),
        segment_size: Some(d.datagram_size().get()),
        src_ip: None,
    };

    match state.try_send(socket, &transmit) {
        Ok(()) => {}
        Err(e) if is_emsgsize(&e) => {
            qdebug!(
                "Failed to send datagram of size {} bytes, in {} segments, each {} bytes, from {} to {}. PMTUD probe? Ignoring error: {e}",
                d.data().len(),
                d.num_datagrams(),
                d.datagram_size().get(),
                d.source(),
                d.destination()
            );
            return Ok(());
        }
        Err(e) if is_enobufs(&e) => {
            qdebug!("Interface send queue full (ENOBUFS), dropping packet: {e}");
            return Ok(());
        }
        e @ Err(_) => return e,
    }

    qtrace!(
        "sent {} bytes, in {} segments, each {} bytes, from {} to {} ",
        d.data().len(),
        d.num_datagrams(),
        d.datagram_size().get(),
        d.source(),
        d.destination(),
    );

    Ok(())
}

fn is_emsgsize(e: &io::Error) -> bool {
    e.raw_os_error() == Some(libc::EMSGSIZE)
}



fn is_enobufs(e: &io::Error) -> bool {
    e.raw_os_error() == Some(libc::ENOBUFS)
}



use std::os::fd::AsFd as SocketRef;

#[expect(clippy::missing_panics_doc, reason = "OK here.")]
pub fn recv_inner<'a, S: SocketRef>(
    local_address: SocketAddr,
    state: &UdpSocketState,
    socket: S,
    recv_buf: &'a mut RecvBuf,
) -> Result<DatagramIter<'a>, io::Error> {
    let mut metas = [RecvMeta::default(); NUM_BUFS];
    let mut iovs: [IoSliceMut; NUM_BUFS] = {
        let mut bufs = recv_buf.0.iter_mut().map(|b| IoSliceMut::new(b));
        array::from_fn(|_| bufs.next().expect("NUM_BUFS elements"))
    };

    let n = state.recv((&socket).into(), &mut iovs, &mut metas)?;

    if log_enabled!(Level::Trace) {
        for meta in metas.iter().take(n) {
            qtrace!(
                "received {} bytes, in {} segments, each {} bytes, from {} to {local_address}",
                meta.len,
                if meta.stride == 0 {
                    0
                } else {
                    meta.len.div_ceil(meta.stride)
                },
                meta.stride,
                meta.addr,
            );
        }
    }

    Ok(DatagramIter {
        current_buffer: None,
        remaining_buffers: metas.into_iter().zip(recv_buf.0.iter_mut()).take(n),
        local_address,
    })
}

pub struct DatagramIter<'a> {
    /// The current buffer, containing zero or more datagrams, each sharing the
    /// same [`RecvMeta`].
    current_buffer: Option<(RecvMeta, ChunksMut<'a, u8>)>,
    /// Remaining buffers, each containing zero or more datagrams, one
    /// [`RecvMeta`] per buffer.
    remaining_buffers:
        iter::Take<iter::Zip<array::IntoIter<RecvMeta, NUM_BUFS>, slice::IterMut<'a, Vec<u8>>>>,
    /// The local address of the UDP socket used to receive the datagrams.
    local_address: SocketAddr,
}

impl<'a> Iterator for DatagramIter<'a> {
    type Item = Datagram<&'a mut [u8]>;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            if let Some((meta, d)) = self
                .current_buffer
                .as_mut()
                .and_then(|(meta, ds)| ds.next().map(|d| (meta, d)))
            {
                return Some(Datagram::from_slice(
                    meta.addr,
                    self.local_address,
                    meta.ecn.map(|n| Tos::from(n as u8)).unwrap_or_default(),
                    d,
                ));
            }

            let Some((meta, buf)) = self.remaining_buffers.next() else {
                return None;
            };

            if meta.len == 0 || meta.stride == 0 {
                qdebug!(
                    "ignoring empty datagram from {} to {} len {} stride {}",
                    meta.addr,
                    self.local_address,
                    meta.len,
                    meta.stride
                );
                continue;
            }

            self.current_buffer = Some((meta, buf[0..meta.len].chunks_mut(meta.stride)));
        }
    }
}

/// A wrapper around a UDP socket, sending and receiving [`Datagram`]s.
pub struct Socket<S> {
    state: UdpSocketState,
    inner: S,
}

impl<S: SocketRef> Socket<S> {
    /// Create a new [`Socket`] given a raw file descriptor managed externally.
    pub fn new(socket: S) -> Result<Self, io::Error> {
        let state = UdpSocketState::new((&socket).into())?;
        Ok(Self {
            state,
            inner: socket,
        })
    }

    /// Enable the Apple fast UDP datapath (`sendmsg_x`/`recvmsg_x`) for this
    /// socket.
    ///
    /// # Safety
    ///
    /// `sendmsg_x` and `recvmsg_x` are private Apple APIs. Quinn-udp resolves
    /// them at runtime via `dlsym` and falls back to standard `sendmsg`/`recvmsg`
    /// if they are unavailable, so this will not crash on unsupported OS versions.
    /// The `unsafe` contract is inherited from [`quinn_udp::UdpSocketState::set_apple_fast_path`].
    #[cfg(apple)]
    pub unsafe fn enable_apple_fast_path(&self) {
        unsafe { self.state.set_apple_fast_path() }
    }

    /// Send a [`datagram::Batch`] on the given [`Socket`].
    pub fn send(&self, d: &datagram::Batch) -> io::Result<()> {
        send_inner(&self.state, (&self.inner).into(), d)
    }

    /// Returns the maximum number of GSO segments supported by this socket.
    pub fn max_gso_segments(&self) -> usize {
        self.state.max_gso_segments()
    }

    /// Receive a batch of [`Datagram`]s on the given [`Socket`], each
    /// set with the provided local address.
    pub fn recv<'a>(
        &self,
        local_address: SocketAddr,
        recv_buf: &'a mut RecvBuf,
    ) -> Result<DatagramIter<'a>, io::Error> {
        recv_inner(local_address, &self.state, &self.inner, recv_buf)
    }

    /// Whether transmitted datagrams might get fragmented by the IP layer
    ///
    /// Returns `false` on targets which employ e.g. the `IPV6_DONTFRAG` socket option.
    pub fn may_fragment(&self) -> bool {
        self.state.may_fragment()
    }
}
