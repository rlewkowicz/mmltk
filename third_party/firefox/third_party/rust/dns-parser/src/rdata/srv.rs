use {Name, Error};
use byteorder::{BigEndian, ByteOrder};

#[derive(Debug, Clone, Copy)]
pub struct Record<'a> {
    pub priority: u16,
    pub weight: u16,
    pub port: u16,
    pub target: Name<'a>,
}

impl<'a> super::Record<'a> for Record<'a> {

    const TYPE: isize = 33;

    fn parse(rdata: &'a [u8], original: &'a [u8]) -> super::RDataResult<'a> {
        if rdata.len() < 7 {
            return Err(Error::WrongRdataLength);
        }
        let record = Record {
            priority: BigEndian::read_u16(&rdata[..2]),
            weight: BigEndian::read_u16(&rdata[2..4]),
            port: BigEndian::read_u16(&rdata[4..6]),
            target: Name::scan(&rdata[6..], original)?,
        };
        Ok(super::RData::SRV(record))
    }
}
