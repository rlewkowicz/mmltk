use std::net::Ipv6Addr;

use Error;
use byteorder::{BigEndian, ByteOrder};

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub struct Record(pub Ipv6Addr);

impl<'a> super::Record<'a> for Record {

    const TYPE: isize = 28;

    fn parse(rdata: &'a [u8], _record: &'a [u8]) -> super::RDataResult<'a> {
        if rdata.len() != 16 {
            return Err(Error::WrongRdataLength);
        }
        let address = Ipv6Addr::new(
            BigEndian::read_u16(&rdata[0..2]),
            BigEndian::read_u16(&rdata[2..4]),
            BigEndian::read_u16(&rdata[4..6]),
            BigEndian::read_u16(&rdata[6..8]),
            BigEndian::read_u16(&rdata[8..10]),
            BigEndian::read_u16(&rdata[10..12]),
            BigEndian::read_u16(&rdata[12..14]),
            BigEndian::read_u16(&rdata[14..16]),
            );
        let record = Record(address);
        Ok(super::RData::AAAA(record))
    }
}
