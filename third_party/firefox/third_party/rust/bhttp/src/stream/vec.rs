use std::{
    future::Future,
    mem,
    pin::{pin, Pin},
    task::{Context, Poll},
};

use futures::{io::AsyncRead, FutureExt};

use super::int::{read_varint, ReadVarint};
use crate::{Error, Res};

/// A reader for a varint-length-prefixed buffer.
#[pin_project::pin_project(project = ReadVecProj)]
#[allow(clippy::module_name_repetitions)]
pub enum ReadVec<S> {
    ReadLen {
        src: Option<ReadVarint<S>>,
        cap: u64,
    },
    ReadBody {
        src: S,
        buf: Vec<u8>,
        remaining: usize,
    },
}

impl<S> ReadVec<S> {
    /// # Panics
    /// If `limit` is more than `usize::MAX` or
    /// if this is called after the length is read.
    pub fn limit(&mut self, limit: u64) {
        usize::try_from(limit).expect("cannot set a limit larger than usize::MAX");
        if let Self::ReadLen { ref mut cap, .. } = self {
            *cap = limit;
        } else {
            panic!("cannot set a limit once the size has been read");
        }
    }

    pub fn stream(self) -> S {
        match self {
            Self::ReadLen { mut src, .. } => src.take().unwrap().stream(),
            Self::ReadBody { src, .. } => src,
        }
    }
}

impl<S: AsyncRead + Unpin> Future for ReadVec<S> {
    type Output = Res<Option<Vec<u8>>>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.as_mut();
        if let Self::ReadLen { src, cap } = this.get_mut() {
            match src.as_mut().unwrap().poll_unpin(cx) {
                Poll::Ready(Ok(None)) => return Poll::Ready(Ok(None)),
                Poll::Ready(Ok(Some(0))) => return Poll::Ready(Ok(Some(Vec::new()))),
                Poll::Ready(Ok(Some(sz))) => {
                    if sz > *cap {
                        return Poll::Ready(Err(Error::LimitExceeded));
                    }
                    let sz = usize::try_from(sz).unwrap();
                    let body = Self::ReadBody {
                        src: src.take().unwrap().stream(),
                        buf: vec![0; sz],
                        remaining: sz,
                    };
                    self.set(body);
                }
                Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                Poll::Pending => return Poll::Pending,
            }
        }

        let ReadVecProj::ReadBody {
            src,
            buf,
            remaining,
        } = self.project()
        else {
            return Poll::Pending;
        };

        let offset = buf.len() - *remaining;
        match pin!(src).poll_read(cx, &mut buf[offset..]) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Err(e)) => Poll::Ready(Err(Error::from(e))),
            Poll::Ready(Ok(0)) => Poll::Ready(Err(Error::Truncated)),
            Poll::Ready(Ok(c)) => {
                *remaining -= c;
                if *remaining > 0 {
                    Poll::Pending
                } else {
                    Poll::Ready(Ok(Some(mem::take(buf))))
                }
            }
        }
    }
}

#[allow(clippy::module_name_repetitions)]
pub fn read_vec<S>(src: S) -> ReadVec<S> {
    ReadVec::ReadLen {
        src: Some(read_varint(src)),
        cap: u64::try_from(usize::MAX).unwrap_or(u64::MAX),
    }
}
