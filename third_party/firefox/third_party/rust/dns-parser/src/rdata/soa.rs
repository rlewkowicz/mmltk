use {Name, Error};
use byteorder::{BigEndian, ByteOrder};

/// The SOA (Start of Authority) record
#[derive(Debug, Clone, Copy)]
pub struct Record<'a> {
    pub primary_ns: Name<'a>,
    pub mailbox: Name<'a>,
    pub serial: u32,
    pub refresh: u32,
    pub retry: u32,
    pub expire: u32,
    pub minimum_ttl: u32,
}

impl<'a> super::Record<'a> for Record<'a> {

    const TYPE: isize = 6;

    fn parse(rdata: &'a [u8], original: &'a [u8]) -> super::RDataResult<'a> {
        let mut pos = 0;
        let primary_name_server = try!(Name::scan(rdata, original));
        pos += primary_name_server.byte_len();
        let mailbox = try!(Name::scan(&rdata[pos..], original));
        pos += mailbox.byte_len();
        if rdata[pos..].len() < 20 {
            return Err(Error::WrongRdataLength);
        }
        let record = Record {
            primary_ns: primary_name_server,
            mailbox: mailbox,
            serial: BigEndian::read_u32(&rdata[pos..(pos+4)]),
            refresh: BigEndian::read_u32(&rdata[(pos+4)..(pos+8)]),
            retry: BigEndian::read_u32(&rdata[(pos+8)..(pos+12)]),
            expire: BigEndian::read_u32(&rdata[(pos+12)..(pos+16)]),
            minimum_ttl: BigEndian::read_u32(&rdata[(pos+16)..(pos+20)]),
        };
        Ok(super::RData::SOA(record))
    }
}
