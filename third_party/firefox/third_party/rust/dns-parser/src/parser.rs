use std::i32;

use byteorder::{BigEndian, ByteOrder};

use {Header, Packet, Error, Question, Name, QueryType, QueryClass};
use {Type, Class, ResourceRecord, RData};
use rdata::opt::Record as Opt;

const OPT_RR_START: [u8; 3] = [0, 0, 41];

impl<'a> Packet<'a> {
    /// Parse a full DNS Packet and return a structure that has all the
    /// data borrowed from the passed buffer.
    pub fn parse(data: &[u8]) -> Result<Packet, Error> {
        let header = try!(Header::parse(data));
        let mut offset = Header::size();
        let mut questions = Vec::with_capacity(header.questions as usize);
        for _ in 0..header.questions {
            let name = try!(Name::scan(&data[offset..], data));
            offset += name.byte_len();
            if offset + 4 > data.len() {
                return Err(Error::UnexpectedEOF);
            }
            let qtype = try!(QueryType::parse(
                BigEndian::read_u16(&data[offset..offset+2])));
            offset += 2;

            let (prefer_unicast, qclass) = try!(parse_qclass_code(
                BigEndian::read_u16(&data[offset..offset+2])));
            offset += 2;

            questions.push(Question {
                qname: name,
                qtype: qtype,
                prefer_unicast: prefer_unicast,
                qclass: qclass,
            });
        }
        let mut answers = Vec::with_capacity(header.answers as usize);
        for _ in 0..header.answers {
            answers.push(try!(parse_record(data, &mut offset)));
        }
        let mut nameservers = Vec::with_capacity(header.nameservers as usize);
        for _ in 0..header.nameservers {
            nameservers.push(try!(parse_record(data, &mut offset)));
        }
        let mut additional = Vec::with_capacity(header.additional as usize);
        let mut opt = None;
        for _ in 0..header.additional {
            if offset + 3 <= data.len() && data[offset..offset+3] == OPT_RR_START {
                if opt.is_none() {
                    opt = Some(try!(parse_opt_record(data, &mut offset)));
                } else {
                    return Err(Error::AdditionalOPT);
                }
            } else {
                additional.push(try!(parse_record(data, &mut offset)));
            }
        }
        Ok(Packet {
            header: header,
            questions: questions,
            answers: answers,
            nameservers: nameservers,
            additional: additional,
            opt: opt,
        })
    }
}

fn parse_qclass_code(value: u16) -> Result<(bool, QueryClass), Error> {
    let prefer_unicast = value & 0x8000 == 0x8000;
    let qclass_code = value & 0x7FFF;

    let qclass = try!(QueryClass::parse(qclass_code));
    Ok((prefer_unicast, qclass))
}

fn parse_class_code(value: u16) -> Result<(bool, Class), Error> {
    let is_unique = value & 0x8000 == 0x8000;
    let class_code = value & 0x7FFF;

    let cls = try!(Class::parse(class_code));
    Ok((is_unique, cls))
}

fn parse_record<'a>(data: &'a [u8], offset: &mut usize) -> Result<ResourceRecord<'a>, Error> {
    let name = try!(Name::scan(&data[*offset..], data));
    *offset += name.byte_len();
    if *offset + 10 > data.len() {
        return Err(Error::UnexpectedEOF);
    }
    let typ = try!(Type::parse(
        BigEndian::read_u16(&data[*offset..*offset+2])));
    *offset += 2;

    let class_code = BigEndian::read_u16(&data[*offset..*offset+2]);
    let (multicast_unique, cls) = try!(parse_class_code(class_code));
    *offset += 2;

    let mut ttl = BigEndian::read_u32(&data[*offset..*offset+4]);
    if ttl > i32::MAX as u32 {
        ttl = 0;
    }
    *offset += 4;
    let rdlen = BigEndian::read_u16(&data[*offset..*offset+2]) as usize;
    *offset += 2;
    if *offset + rdlen > data.len() {
        return Err(Error::UnexpectedEOF);
    }
    let data = try!(RData::parse(typ,
        &data[*offset..*offset+rdlen], data));
    *offset += rdlen;
    Ok(ResourceRecord {
        name: name,
        multicast_unique: multicast_unique,
        cls: cls,
        ttl: ttl,
        data: data,
    })
}

fn parse_opt_record<'a>(data: &'a [u8], offset: &mut usize) -> Result<Opt<'a>, Error> {
    if *offset + 11 > data.len() {
        return Err(Error::UnexpectedEOF);
    }
    *offset += 1;
    let typ = try!(Type::parse(
        BigEndian::read_u16(&data[*offset..*offset+2])));
    if typ != Type::OPT {
        return Err(Error::InvalidType(typ as u16));
    }
    *offset += 2;
    let udp = BigEndian::read_u16(&data[*offset..*offset+2]);
    *offset += 2;
    let extrcode = data[*offset];
    *offset += 1;
    let version = data[*offset];
    *offset += 1;
    let flags = BigEndian::read_u16(&data[*offset..*offset+2]);
    *offset += 2;
    let rdlen = BigEndian::read_u16(&data[*offset..*offset+2]) as usize;
    *offset += 2;
    if *offset + rdlen > data.len() {
        return Err(Error::UnexpectedEOF);
    }
    let data = try!(RData::parse(typ,
        &data[*offset..*offset+rdlen], data));
    *offset += rdlen;

    Ok(Opt {
        udp: udp,
        extrcode: extrcode,
        version: version,
        flags: flags,
        data: data,
    })
}
