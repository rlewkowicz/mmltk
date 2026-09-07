use crate::SeaHasher;
use std::hash::Hasher;
use std::io;

impl io::Write for SeaHasher {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        Hasher::write(self, buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
