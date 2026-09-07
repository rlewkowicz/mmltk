use std::io::{self, Read};
use std::str;
use std::fmt;

#[derive(Debug)]
pub enum CharReadError {
    UnexpectedEof,
    Utf8(str::Utf8Error),
    Io(io::Error)
}

impl From<str::Utf8Error> for CharReadError {
    fn from(e: str::Utf8Error) -> CharReadError {
        CharReadError::Utf8(e)
    }
}

impl From<io::Error> for CharReadError {
    fn from(e: io::Error) -> CharReadError {
        CharReadError::Io(e)
    }
}

impl fmt::Display for CharReadError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        use self::CharReadError::*;
        match *self {
            UnexpectedEof => write!(f, "unexpected end of stream"),
            Utf8(ref e) => write!(f, "UTF-8 decoding error: {}", e),
            Io(ref e) => write!(f, "I/O error: {}", e)
        }
    }
}

pub fn next_char_from<R: Read>(source: &mut R) -> Result<Option<char>, CharReadError> {
    const MAX_CODEPOINT_LEN: usize = 4;

    let mut bytes = source.bytes();
    let mut buf = [0u8; MAX_CODEPOINT_LEN];
    let mut pos = 0;

    loop {
        let next = match bytes.next() {
            Some(Ok(b)) => b,
            Some(Err(e)) => return Err(e.into()),
            None if pos == 0 => return Ok(None),
            None => return Err(CharReadError::UnexpectedEof)
        };
        buf[pos] = next;
        pos += 1;

        match str::from_utf8(&buf[..pos]) {
            Ok(s) => return Ok(s.chars().next()),  
            Err(_) if pos < MAX_CODEPOINT_LEN => {},
            Err(e) => return Err(e.into())
        }
    }
}
