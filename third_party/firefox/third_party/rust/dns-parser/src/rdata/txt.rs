use Error;

#[derive(Debug, Clone)]
pub struct Record<'a> {
    bytes: &'a [u8],
}

#[derive(Debug)]
pub struct RecordIter<'a> {
    bytes: &'a [u8],
}

impl<'a> Iterator for RecordIter<'a> {
    type Item = &'a [u8];
    fn next(&mut self) -> Option<&'a [u8]> {
        if self.bytes.len() >= 1 {
            let len = self.bytes[0] as usize;
            debug_assert!(self.bytes.len() >= len+1);
            let (head, tail) = self.bytes[1..].split_at(len);
            self.bytes = tail;
            return Some(head);
        }
        return None;
    }
}

impl<'a> Record<'a> {

    pub fn iter(&self) -> RecordIter<'a> {
        RecordIter {
            bytes: self.bytes,
        }
    }
}

impl<'a> super::Record<'a> for Record<'a> {

    const TYPE: isize = 16;

    fn parse(rdata: &'a [u8], _original: &'a [u8]) -> super::RDataResult<'a> {
        let len = rdata.len();
        if len < 1 {
            return Err(Error::WrongRdataLength);
        }
        let mut pos = 0;
        while pos < len {
            let rdlen = rdata[pos] as usize;
            pos += 1;
            if len < rdlen + pos {
                return Err(Error::WrongRdataLength);
            }
            pos += rdlen;
        }
        Ok(super::RData::TXT(Record {
            bytes: rdata,
        }))
    }
}
