use {Name, Error};
use byteorder::{BigEndian, ByteOrder};

#[derive(Debug, Clone, Copy)]
pub struct Record<'a> {
    pub preference: u16,
    pub exchange: Name<'a>,
}

impl<'a> super::Record<'a> for Record<'a> {

    const TYPE: isize = 15;

    fn parse(rdata: &'a [u8], original: &'a [u8]) -> super::RDataResult<'a> {
        if rdata.len() < 3 {
            return Err(Error::WrongRdataLength);
        }
        let record = Record {
            preference: BigEndian::read_u16(&rdata[..2]),
            exchange: Name::scan(&rdata[2..], original)?,
        };
        Ok(super::RData::MX(record))
    }
}
