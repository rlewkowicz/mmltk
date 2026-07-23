#![allow(dead_code)]
#![allow(clippy::incompatible_msrv)] 

use std::{
    cmp::min,
    io::{Cursor, Error as IoError, Result as IoResult},
    mem,
    pin::{pin, Pin},
    task::{Context, Poll},
};

use futures::{stream::unfold, AsyncRead, Stream, TryStreamExt};

use crate::{
    err::Res,
    stream::{int::read_varint, vec::read_vec},
    ControlData, Error, Field, FieldSection, Header, InformationalResponse, Message, Mode, COOKIE,
};
mod int;
mod vec;

trait AsyncReadControlData: Sized {
    async fn async_read<S: AsyncRead + Unpin>(request: bool, src: S) -> Res<Self>;
}

impl AsyncReadControlData for ControlData {
    async fn async_read<S: AsyncRead + Unpin>(request: bool, mut src: S) -> Res<Self> {
        let v = if request {
            let method = read_vec(&mut src).await?.ok_or(Error::Truncated)?;
            let scheme = read_vec(&mut src).await?.ok_or(Error::Truncated)?;
            let authority = read_vec(&mut src).await?.ok_or(Error::Truncated)?;
            let path = read_vec(&mut src).await?.ok_or(Error::Truncated)?;
            Self::Request {
                method,
                scheme,
                authority,
                path,
            }
        } else {
            let code = read_varint(&mut src).await?.ok_or(Error::Truncated)?;
            Self::Response(crate::StatusCode::try_from(code)?)
        };
        Ok(v)
    }
}

trait AsyncReadFieldSection: Sized {
    async fn async_read<S: AsyncRead + Unpin>(mode: Mode, src: S) -> Res<Self>;
}

impl AsyncReadFieldSection for FieldSection {
    async fn async_read<S: AsyncRead + Unpin>(mode: Mode, mut src: S) -> Res<Self> {
        let fields = if mode == Mode::KnownLength {
            if let Some(buf) = read_vec(&mut src).await? {
                Self::read_bhttp_fields(false, &mut Cursor::new(&buf[..]))?
            } else {
                Vec::new()
            }
        } else {
            let mut fields: Vec<Field> = Vec::new();
            let mut cookie_index: Option<usize> = None;
            loop {
                if let Some(n) = read_vec(&mut src).await? {
                    if n.is_empty() {
                        break fields;
                    }
                    let mut v = read_vec(&mut src).await?.ok_or(Error::Truncated)?;
                    if n == COOKIE {
                        if let Some(i) = &cookie_index {
                            fields[*i].value.extend_from_slice(b"; ");
                            fields[*i].value.append(&mut v);
                            continue;
                        }
                        cookie_index = Some(fields.len());
                    }
                    fields.push(Field::new(n, v));
                } else if fields.is_empty() {
                    break fields;
                } else {
                    return Err(Error::Truncated);
                }
            }
        };
        Ok(Self(fields))
    }
}

#[derive(Default)]
enum BodyState {
    #[default]
    Init,
    ReadLength {
        buf: [u8; 8],
        read: usize,
    },
    ReadData {
        remaining: usize,
    },
}

impl BodyState {
    fn read_len() -> Self {
        Self::ReadLength {
            buf: [0; 8],
            read: 0,
        }
    }
}

pub struct Body<'b, S> {
    msg: &'b mut AsyncMessage<S>,
}

impl<S> Body<'_, S> {}

impl<S: AsyncRead + Unpin> AsyncRead for Body<'_, S> {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<IoResult<usize>> {
        self.msg.read_body(cx, buf).map_err(IoError::other)
    }
}

/// A helper function for the more complex body-reading code.
fn poll_error(e: Error) -> Poll<IoResult<usize>> {
    Poll::Ready(Err(IoError::other(e)))
}

enum AsyncMessageState {
    Init,
    Informational(bool),
    Header(ControlData),
    Body(BodyState),
    Trailer,
    Done,
}

pub struct AsyncMessage<S> {
    mode: Option<Mode>,
    state: AsyncMessageState,
    src: S,
}

unsafe impl<S: Send> Send for AsyncMessage<S> {}

impl<S: AsyncRead + Unpin> AsyncMessage<S> {
    async fn next_info(&mut self) -> Res<Option<InformationalResponse>> {
        let request = if matches!(self.state, AsyncMessageState::Init) {
            let t = read_varint(&mut self.src).await?.ok_or(Error::Truncated)?;
            let request = t == 0 || t == 2;
            self.mode = Some(Mode::try_from(t)?);
            self.state = AsyncMessageState::Informational(request);
            request
        } else {
            let AsyncMessageState::Informational(request) = self.state else {
                return Err(Error::InvalidState);
            };
            request
        };

        let control = ControlData::async_read(request, &mut self.src).await?;
        if let Some(status) = control.informational() {
            let mode = self.mode.unwrap();
            let fields = FieldSection::async_read(mode, &mut self.src).await?;
            Ok(Some(InformationalResponse::new(status, fields)))
        } else {
            self.state = AsyncMessageState::Header(control);
            Ok(None)
        }
    }

    /// Produces a stream of informational responses from a fresh message.
    /// Returns an empty stream if passed a request (or if there are no informational responses).
    /// Error values on the stream indicate failures.
    ///
    /// There is no need to call this method to read a request, though
    /// doing so is harmless.
    ///
    /// You can discard the stream that this function returns
    /// without affecting the message.  You can then either call this
    /// method again to get any additional informational responses or
    /// call `header()` to get the message header.
    pub fn informational(&mut self) -> impl Stream<Item = Res<InformationalResponse>> + '_ {
        unfold(self, |this| async move {
            this.next_info().await.transpose().map(|info| (info, this))
        })
    }

    /// This reads the header.  If you have not called `informational`
    /// and drained the resulting stream, this will do that for you.
    /// # Panics
    /// Never.
    pub async fn header(&mut self) -> Res<Header> {
        if matches!(
            self.state,
            AsyncMessageState::Init | AsyncMessageState::Informational(_)
        ) {
            _ = self.informational().try_any(|_| async { false }).await?;
        }

        if matches!(self.state, AsyncMessageState::Header(_)) {
            let mode = self.mode.unwrap();
            let hfields = FieldSection::async_read(mode, &mut self.src).await?;

            let AsyncMessageState::Header(control) = mem::replace(
                &mut self.state,
                AsyncMessageState::Body(BodyState::default()),
            ) else {
                unreachable!();
            };
            Ok(Header::from((control, hfields)))
        } else {
            Err(Error::InvalidState)
        }
    }

    fn body_state(&mut self, s: BodyState) {
        self.state = AsyncMessageState::Body(s);
    }

    fn body_done(&mut self) {
        self.state = AsyncMessageState::Trailer;
    }

    /// Read the length of a body chunk.
    /// This updates the values of `read` and `buf` to track the portion of the length
    /// that was successfully read.
    /// Returns `Some` with the error code that should be used if the reading
    /// resulted in a conclusive outcome.
    fn read_body_len(
        cx: &mut Context<'_>,
        src: &mut S,
        first: bool,
        read: &mut usize,
        buf: &mut [u8; 8],
    ) -> Option<Poll<Result<usize, IoError>>> {
        let mut src = pin!(src);
        if *read == 0 {
            let mut b = [0; 1];
            match src.as_mut().poll_read(cx, &mut b[..]) {
                Poll::Pending => return Some(Poll::Pending),
                Poll::Ready(Ok(0)) => {
                    return if first {
                        *read = 8;
                        None
                    } else {
                        Some(poll_error(Error::Truncated))
                    };
                }
                Poll::Ready(Ok(1)) => match b[0] >> 6 {
                    0 => {
                        buf[7] = b[0] & 0x3f;
                        *read = 8;
                    }
                    1 => {
                        buf[6] = b[0] & 0x3f;
                        *read = 7;
                    }
                    2 => {
                        buf[4] = b[0] & 0x3f;
                        *read = 5;
                    }
                    3 => {
                        buf[0] = b[0] & 0x3f;
                        *read = 1;
                    }
                    _ => unreachable!(),
                },
                Poll::Ready(Ok(_)) => unreachable!(),
                Poll::Ready(Err(e)) => return Some(Poll::Ready(Err(e))),
            }
        }
        if *read < 8 {
            match src.as_mut().poll_read(cx, &mut buf[*read..]) {
                Poll::Pending => return Some(Poll::Pending),
                Poll::Ready(Ok(0)) => return Some(poll_error(Error::Truncated)),
                Poll::Ready(Ok(len)) => {
                    *read += len;
                }
                Poll::Ready(Err(e)) => return Some(Poll::Ready(Err(e))),
            }
        }
        None
    }

    fn read_body(&mut self, cx: &mut Context<'_>, buf: &mut [u8]) -> Poll<IoResult<usize>> {
        let first = if let AsyncMessageState::Body(BodyState::Init) = &self.state {
            self.body_state(BodyState::read_len());
            true
        } else {
            false
        };

        if let AsyncMessageState::Body(BodyState::ReadLength { buf, read }) = &mut self.state {
            if let Some(res) = Self::read_body_len(cx, &mut self.src, first, read, buf) {
                return res;
            }
            if *read == 8 {
                match usize::try_from(u64::from_be_bytes(*buf)) {
                    Ok(0) => {
                        self.body_done();
                        return Poll::Ready(Ok(0));
                    }
                    Ok(remaining) => {
                        self.body_state(BodyState::ReadData { remaining });
                    }
                    Err(e) => return poll_error(Error::IntRange(e)),
                }
            }
        }

        match &mut self.state {
            AsyncMessageState::Body(BodyState::ReadData { remaining }) => {
                let amount = min(*remaining, buf.len());
                let res = pin!(&mut self.src).poll_read(cx, &mut buf[..amount]);
                match res {
                    Poll::Pending => Poll::Pending,
                    Poll::Ready(Ok(0)) => poll_error(Error::Truncated),
                    Poll::Ready(Ok(len)) => {
                        *remaining -= len;
                        if *remaining == 0 {
                            let mode = self.mode.unwrap();
                            if mode == Mode::IndeterminateLength {
                                self.body_state(BodyState::read_len());
                            } else {
                                self.body_done();
                            }
                        }
                        Poll::Ready(Ok(len))
                    }
                    Poll::Ready(Err(e)) => Poll::Ready(Err(e)),
                }
            }
            AsyncMessageState::Trailer => Poll::Ready(Ok(0)),
            _ => Poll::Pending,
        }
    }

    /// Read the body.
    /// This produces an implementation of `AsyncRead` that filters out
    /// the framing from the message body.
    /// # Errors
    /// This errors when the header has not been read.
    /// Any IO errors are generated by the returned `Body` instance.
    pub fn body(&mut self) -> Res<Body<'_, S>> {
        match self.state {
            AsyncMessageState::Body(_) => Ok(Body { msg: self }),
            _ => Err(Error::InvalidState),
        }
    }

    /// Read any trailer.
    /// This might be empty.
    /// # Errors
    /// This errors when the body has not been read.
    /// # Panics
    /// Never.
    pub async fn trailer(&mut self) -> Res<FieldSection> {
        if matches!(self.state, AsyncMessageState::Trailer) {
            let trailer = FieldSection::async_read(self.mode.unwrap(), &mut self.src).await?;
            self.state = AsyncMessageState::Done;
            Ok(trailer)
        } else {
            Err(Error::InvalidState)
        }
    }
}

/// Asynchronous reading for a [`Message`].
pub trait AsyncReadMessage: Sized {
    fn async_read<S: AsyncRead + Unpin>(src: S) -> AsyncMessage<S>;
}

impl AsyncReadMessage for Message {
    fn async_read<S: AsyncRead + Unpin>(src: S) -> AsyncMessage<S> {
        AsyncMessage {
            mode: None,
            state: AsyncMessageState::Init,
            src,
        }
    }
}
