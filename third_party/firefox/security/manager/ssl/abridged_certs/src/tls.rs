/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

use super::AbridgedError;
use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use log::trace;
use std::io::Cursor;
use std::io::Write;
use std::u8;

/// Parses a TLS vector as defined in https://datatracker.ietf.org/doc/html/rfc8446#section-3.4
/// The length prefix is specified as WIDTH within 1..4 inclusive.
/// The result either indicates an error or produces the body of the vec
/// and any remaining data.
fn read_tls_vec<const WIDTH: u8>(
    value: &[u8],
) -> Result<(&[u8], &[u8]), AbridgedError> {
    debug_assert!(WIDTH <= 4, "Invalid width specified");

    let Some((len_bytes, remainder)) = value.split_at_checked(usize::from(WIDTH)) else {
        return Err(AbridgedError::ParsingInvalidTLSVec);
    };

    let io_err_wrapper = |x| AbridgedError::ReadingError(x);
    let mut len_rdr = Cursor::new(len_bytes);
    let len: u32 = match WIDTH {
        1 => len_rdr.read_u8().map_err(io_err_wrapper)?.into(),
        2 => len_rdr
            .read_u16::<BigEndian>()
            .map_err(io_err_wrapper)?
            .into(),
        3 => len_rdr.read_u24::<BigEndian>().map_err(io_err_wrapper)?,
        4 => len_rdr.read_u32::<BigEndian>().map_err(io_err_wrapper)?,
        _ => return Err(AbridgedError::InvalidOperation),
    };

    let Some((vec_body, remainder)) = remainder.split_at_checked(len as usize) else {
        return Err(AbridgedError::ParsingInvalidTLSVec);
    };

    trace!(
        "In length: {}, Output length: {}, Remainder Length: {}",
        value.len(),
        vec_body.len(),
        remainder.len()
    );
    Ok((vec_body, remainder))
}

/// Writes out an integer as defined in https://datatracker.ietf.org/doc/html/rfc8446#section-3.3
/// WIDTH must be between 1 and 4
fn write_tls_int<const WIDTH: u8>(writer: &mut impl Write, int: u32) -> Result<(), AbridgedError> {
    debug_assert!(WIDTH <= 4 && WIDTH > 0, "Invalid width specified");
    if u64::from(int) > 2_u64.pow(u32::from(WIDTH) * 8) - 1 {
        return Err(AbridgedError::InvalidOperation);
    }
    writer
        .write_uint::<BigEndian>(u64::from(int), usize::from(WIDTH))
        .map_err(|x| AbridgedError::WritingError(x))?;
    Ok(())
}

/// Writes out a TLS vector as defined in https://datatracker.ietf.org/doc/html/rfc8446#section-3.4
/// WIDTH must be between 1 and 4
fn write_tls_vec<const WIDTH: u8>(
    value: &[u8],
    writer: &mut impl Write,
) -> Result<(), AbridgedError> {
    debug_assert!(WIDTH <= 4 && WIDTH > 0, "Invalid width specified");

    let len: u32 = value
        .len()
        .try_into()
        .or(Err(AbridgedError::InvalidOperation))?;

    write_tls_int::<WIDTH>(writer, len)?;
    writer
        .write_all(value)
        .or_else(|x| Err(AbridgedError::WritingError(x)))?;
    Ok(())
}

/// These types represent the structure of a TLS 1.3 certificate message
///
/// RFC 8446: 4.4.2
/// enum {
///     X509(0),
///     RawPublicKey(2),
///     (255)
/// } CertificateType;
///
/// struct {
///     select (certificate_type) {
///         case RawPublicKey:
///           /* From RFC 7250 ASN.1_subjectPublicKeyInfo */
///           opaque ASN1_subjectPublicKeyInfo<1..2^24-1>;
///
///         case X509:
///           opaque cert_data<1..2^24-1>;
///     };
///     Extension extensions<0..2^16-1>;
/// } CertificateEntry;
///
/// struct {
///     opaque certificate_request_context<0..2^8-1>;
///     CertificateEntry certificate_list<0..2^24-1>;
/// } Certificate;

#[derive(Debug)]
pub struct CertificateEntry {
    pub data: Vec<u8>,
    pub extensions: Vec<u8>,
}

pub type UncompressedCertEntry = CertificateEntry;
pub type CompressedCertEntry = CertificateEntry;

#[derive(Debug)]
pub struct CertificateMessage {
    pub request_context: Vec<u8>,
    pub certificate_entries: Vec<CertificateEntry>,
}

impl CertificateEntry {
    pub fn read_from_bytes(value: &[u8]) -> Result<(CertificateEntry, &[u8]), AbridgedError> {
        let (data, remainder) = read_tls_vec::<3>(value)?;
        let (extensions, remainder) = read_tls_vec::<2>(remainder)?;
        Ok((
            CertificateEntry {
                data: data.to_vec(),
                extensions: extensions.to_vec(),
            },
            remainder,
        ))
    }

    pub fn write_to_bytes(&self, writer: &mut impl Write) -> Result<(), AbridgedError> {
        write_tls_vec::<3>(&self.data, writer)?;
        write_tls_vec::<2>(&self.extensions, writer)?;
        Ok(())
    }

    pub fn get_size(&self) -> usize {
        let calculated_size = 3 + self.data.len() + 2 + self.extensions.len();

        if cfg!(debug_assertions) {
            let mut output = Vec::with_capacity(calculated_size);
            self.write_to_bytes(&mut output).expect("Shouldn't error");
            debug_assert_eq!(calculated_size, output.len());
        }
        calculated_size
    }
}

impl CertificateMessage {
    pub fn read_from_bytes(value: &[u8]) -> Result<(CertificateMessage, &[u8]), AbridgedError> {
        trace!("Parsing certificate message from {} bytes", value.len());
        let (request_context, certificate_entries) = read_tls_vec::<1>(value)?;
        trace!(
            "Parsing request_context of size {}, {} remaining",
            request_context.len(),
            value.len()
        );
        let (certificate_entries, tail) = read_tls_vec::<3>(certificate_entries)?;
        trace!(
            "Parsing certificate_field of size {}, {} remaining",
            certificate_entries.len(),
            value.len()
        );

        let mut parsed_certificate_entries = Vec::with_capacity(5);

        let mut remaining_data = certificate_entries;
        while !remaining_data.is_empty() {
            let (entry, temp) = CertificateEntry::read_from_bytes(remaining_data)?;
            remaining_data = temp;
            parsed_certificate_entries.push(entry);
        }
        Ok((
            CertificateMessage {
                request_context: request_context.to_vec(),
                certificate_entries: parsed_certificate_entries,
            },
            tail,
        ))
    }

    pub fn write_to_bytes(&self, writer: &mut impl Write) -> Result<(), AbridgedError> {
        let ce_size: u32 = self
            .certificate_entries
            .iter()
            .map(CertificateEntry::get_size)
            .sum::<usize>()
            .try_into()
            .or(Err(AbridgedError::InvalidOperation))?;
        write_tls_vec::<1>(&self.request_context, writer)?;
        write_tls_int::<3>(writer, ce_size)?;
        for ce in &self.certificate_entries {
            ce.write_to_bytes(writer)?;
        }
        Ok(())
    }

    pub fn get_size(&self) -> usize {
        let calculated_size = 1
            + self.request_context.len()
            + 3
            + self
                .certificate_entries
                .iter()
                .map(|x| x.get_size())
                .sum::<usize>();

        if cfg!(debug_assertions) {
            let mut output = Vec::with_capacity(calculated_size);
            self.write_to_bytes(&mut output).expect("Shouldn't error");
            debug_assert_eq!(calculated_size, output.len());
        }
        calculated_size
    }
}
