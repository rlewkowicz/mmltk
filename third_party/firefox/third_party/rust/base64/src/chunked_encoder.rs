use crate::{
    encode::add_padding,
    engine::{Config, Engine},
};
#[cfg(feature = "alloc")]
use alloc::string::String;
#[cfg(feature = "alloc")]
use core::str;

/// The output mechanism for ChunkedEncoder's encoded bytes.
pub trait Sink {
    type Error;

    /// Handle a chunk of encoded base64 data (as UTF-8 bytes)
    fn write_encoded_bytes(&mut self, encoded: &[u8]) -> Result<(), Self::Error>;
}

/// A base64 encoder that emits encoded bytes in chunks without heap allocation.
pub struct ChunkedEncoder<'e, E: Engine + ?Sized> {
    engine: &'e E,
}

impl<'e, E: Engine + ?Sized> ChunkedEncoder<'e, E> {
    pub fn new(engine: &'e E) -> ChunkedEncoder<'e, E> {
        ChunkedEncoder { engine }
    }

    pub fn encode<S: Sink>(&self, bytes: &[u8], sink: &mut S) -> Result<(), S::Error> {
        const BUF_SIZE: usize = 1024;
        const CHUNK_SIZE: usize = BUF_SIZE / 4 * 3;

        let mut buf = [0; BUF_SIZE];
        for chunk in bytes.chunks(CHUNK_SIZE) {
            let mut len = self.engine.internal_encode(chunk, &mut buf);
            if chunk.len() != CHUNK_SIZE && self.engine.config().encode_padding() {
                len += add_padding(len, &mut buf[len..]);
            }
            sink.write_encoded_bytes(&buf[..len])?;
        }

        Ok(())
    }
}

#[cfg(feature = "alloc")]
pub(crate) struct StringSink<'a> {
    string: &'a mut String,
}

#[cfg(feature = "alloc")]
impl<'a> StringSink<'a> {
    pub(crate) fn new(s: &mut String) -> StringSink {
        StringSink { string: s }
    }
}

#[cfg(feature = "alloc")]
impl<'a> Sink for StringSink<'a> {
    type Error = ();

    fn write_encoded_bytes(&mut self, s: &[u8]) -> Result<(), Self::Error> {
        self.string.push_str(str::from_utf8(s).unwrap());

        Ok(())
    }
}
