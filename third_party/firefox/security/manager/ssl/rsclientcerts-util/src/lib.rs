/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

pub mod error;

use byteorder::{BigEndian, ReadBytesExt};

use crate::error::{Error, ErrorType};

pub const OID_BYTES_SHA_256: &[u8] = &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01];
pub const OID_BYTES_SHA_384: &[u8] = &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02];
pub const OID_BYTES_SHA_512: &[u8] = &[0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03];
pub const OID_BYTES_SHA_1: &[u8] = &[0x2b, 0x0e, 0x03, 0x02, 0x1a];

const OID_BYTES_RSA_ENCRYPTION: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01];
const OID_BYTES_EC_PUBLIC_KEY: &[u8] = &[0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01];
const OID_BYTES_SECP256R1: &[u8] = &[0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07];

/// Given a slice of DER bytes representing an RSA public key, extracts the bytes of the modulus
/// as an unsigned integer. Also verifies that the public exponent is present (again as an
/// unsigned integer). Finally verifies that reading these values consumes the entirety of the
/// slice.
/// RSAPublicKey ::= SEQUENCE {
///     modulus           INTEGER,  -- n
///     publicExponent    INTEGER   -- e
/// }
pub fn read_rsa_modulus(public_key: &[u8]) -> Result<Vec<u8>, Error> {
    let mut sequence = Sequence::new(public_key)?;
    let modulus_value = sequence.read_unsigned_integer()?;
    let _exponent = sequence.read_unsigned_integer()?;
    if !sequence.at_end() {
        return Err(error_here!(ErrorType::ExtraInput));
    }
    Ok(modulus_value.to_vec())
}

/// Given a slice of DER bytes representing a SubjectPublicKeyInfo, extracts
/// the bytes of the parameters of the algorithm. Does not verify that all
/// input is consumed.
/// PublicKeyInfo ::= SEQUENCE {
///   algorithm   AlgorithmIdentifier,
///   PublicKey   BIT STRING
/// }
///
/// AlgorithmIdentifier ::= SEQUENCE {
///   algorithm   OBJECT IDENTIFIER,
///   parameters  ANY DEFINED BY algorithm OPTIONAL
///  }
pub fn read_spki_algorithm_parameters(spki: &[u8]) -> Result<Vec<u8>, Error> {
    let mut public_key_info = Sequence::new(spki)?;
    let mut algorithm_identifier = public_key_info.read_sequence()?;
    let _algorithm = algorithm_identifier.read_oid()?;
    Ok(algorithm_identifier.read_rest().to_vec())
}

/// Given a slice of DER bytes representing a DigestInfo, extracts the bytes of
/// the OID of the hash algorithm and the digest.
/// DigestInfo ::= SEQUENCE {
///   digestAlgorithm DigestAlgorithmIdentifier,
///   digest Digest }
///
/// DigestAlgorithmIdentifier ::= AlgorithmIdentifier
///
/// AlgorithmIdentifier  ::=  SEQUENCE  {
///      algorithm               OBJECT IDENTIFIER,
///      parameters              ANY DEFINED BY algorithm OPTIONAL  }
///
/// Digest ::= OCTET STRING
pub fn read_digest_info(digest_info: &[u8]) -> Result<(&[u8], &[u8]), Error> {
    let mut sequence = Sequence::new(digest_info)?;
    let mut algorithm = sequence.read_sequence()?;
    let oid = algorithm.read_oid()?;
    algorithm.read_null()?;
    if !algorithm.at_end() {
        return Err(error_here!(ErrorType::ExtraInput));
    }
    let digest = sequence.read_octet_string()?;
    if !sequence.at_end() {
        return Err(error_here!(ErrorType::ExtraInput));
    }
    Ok((oid, digest))
}

/// Converts a slice of DER bytes representing an ECDSA signature to the concatenation of the bytes
/// of `r` and `s`, each 0-padded to `coordinate_width`. Also verifies that this consumes the
/// entirety of the slice.
///   Ecdsa-Sig-Value  ::=  SEQUENCE  {
///        r     INTEGER,
///        s     INTEGER  }
pub fn der_ec_sig_to_raw(encoded: &[u8], coordinate_width: usize) -> Result<Vec<u8>, Error> {
    let (r, s) = read_ec_sig_point(encoded)?;
    if r.len() > coordinate_width || s.len() > coordinate_width {
        return Err(error_here!(ErrorType::InvalidInput));
    }
    let mut raw_signature = Vec::with_capacity(2 * coordinate_width);
    raw_signature.resize(coordinate_width - r.len(), 0);
    raw_signature.extend_from_slice(r);
    raw_signature.resize((2 * coordinate_width) - s.len(), 0);
    raw_signature.extend_from_slice(s);
    Ok(raw_signature)
}

/// Given a slice of DER bytes representing an ECDSA signature, extracts the bytes of `r` and `s`
/// as unsigned integers. Also verifies that this consumes the entirety of the slice.
///   Ecdsa-Sig-Value  ::=  SEQUENCE  {
///        r     INTEGER,
///        s     INTEGER  }
fn read_ec_sig_point(signature: &[u8]) -> Result<(&[u8], &[u8]), Error> {
    let mut sequence = Sequence::new(signature)?;
    let r = sequence.read_unsigned_integer()?;
    let s = sequence.read_unsigned_integer()?;
    if !sequence.at_end() {
        return Err(error_here!(ErrorType::ExtraInput));
    }
    Ok((r, s))
}

/// Given a slice of DER bytes representing an X.509 certificate, extracts the encoded serial
/// number, issuer, and subject. Does not verify that the remainder of the certificate is in any
/// way well-formed.
///   Certificate  ::=  SEQUENCE  {
///           tbsCertificate       TBSCertificate,
///           signatureAlgorithm   AlgorithmIdentifier,
///           signatureValue       BIT STRING  }
///
///   TBSCertificate  ::=  SEQUENCE  {
///           version         [0]  EXPLICIT Version DEFAULT v1,
///           serialNumber         CertificateSerialNumber,
///           signature            AlgorithmIdentifier,
///           issuer               Name,
///           validity             Validity,
///           subject              Name,
///           ...
///
///   CertificateSerialNumber  ::=  INTEGER
///
///   Name ::= CHOICE { -- only one possibility for now --
///     rdnSequence  RDNSequence }
///
///   RDNSequence ::= SEQUENCE OF RelativeDistinguishedName
///
///   Validity ::= SEQUENCE {
///        notBefore      Time,
///        notAfter       Time  }
#[allow(clippy::type_complexity)]
pub fn read_encoded_certificate_identifiers(
    certificate: &[u8],
) -> Result<(Vec<u8>, Vec<u8>, Vec<u8>), Error> {
    let mut certificate_sequence = Sequence::new(certificate)?;
    let mut tbs_certificate_sequence = certificate_sequence.read_sequence()?;
    let _version = tbs_certificate_sequence.read_optional_tagged_value(0)?;
    let serial_number = tbs_certificate_sequence.read_encoded_sequence_component(INTEGER)?;
    let _signature = tbs_certificate_sequence.read_sequence()?;
    let issuer =
        tbs_certificate_sequence.read_encoded_sequence_component(SEQUENCE | CONSTRUCTED)?;
    let _validity = tbs_certificate_sequence.read_sequence()?;
    let subject =
        tbs_certificate_sequence.read_encoded_sequence_component(SEQUENCE | CONSTRUCTED)?;
    Ok((serial_number, issuer, subject))
}

pub struct RSAPrivateKey {
    pub modulus: Vec<u8>,
    pub private_exponent: Vec<u8>,
}

pub struct ECPrivateKey {
    pub private_key: Vec<u8>,
}

pub enum PrivateKeyInfo {
    RSA(RSAPrivateKey),
    EC(ECPrivateKey),
}

/// PrivateKeyInfo ::= SEQUENCE {
///  version                   Version,
///  privateKeyAlgorithm       PrivateKeyAlgorithmIdentifier,
///  privateKey                PrivateKey,
///  attributes           [0]  IMPLICIT Attributes OPTIONAL }
///
/// Version ::= INTEGER
///
/// PrivateKeyAlgorithmIdentifier ::= AlgorithmIdentifier
///
/// PrivateKey ::= OCTET STRING
///
/// Attributes ::= SET OF Attribute
///
/// RSAPrivateKey ::= SEQUENCE {
///     version           Version,
///     modulus           INTEGER,  -- n
///     publicExponent    INTEGER,  -- e
///     privateExponent   INTEGER,  -- d
///     prime1            INTEGER,  -- p
///     prime2            INTEGER,  -- q
///     exponent1         INTEGER,  -- d mod (p-1)
///     exponent2         INTEGER,  -- d mod (q-1)
///     coefficient       INTEGER,  -- (inverse of q) mod p
///     otherPrimeInfos   OtherPrimeInfos OPTIONAL
/// }
///
///  ECPrivateKey ::= SEQUENCE {
///    version        INTEGER { ecPrivkeyVer1(1) } (ecPrivkeyVer1),
///    privateKey     OCTET STRING,
///    parameters [0] ECParameters {{ NamedCurve }} OPTIONAL,
///    publicKey  [1] BIT STRING OPTIONAL
/// }
pub fn read_private_key_info(private_key_info: &[u8]) -> Result<PrivateKeyInfo, Error> {
    let mut private_key_info = Sequence::new(private_key_info)?;
    let _version = private_key_info.read_unsigned_integer()?;
    let mut algorithm_identifier = private_key_info.read_sequence()?;
    let algorithm = algorithm_identifier.read_oid()?;
    let private_key_bytes = private_key_info.read_octet_string()?;
    let mut private_key = Sequence::new(private_key_bytes)?;
    if algorithm == OID_BYTES_RSA_ENCRYPTION {
        let _version = private_key.read_unsigned_integer()?;
        let modulus = private_key.read_unsigned_integer()?;
        let _public_exponent = private_key.read_unsigned_integer()?;
        let private_exponent = private_key.read_unsigned_integer()?;
        return Ok(PrivateKeyInfo::RSA(RSAPrivateKey {
            modulus: modulus.to_vec(),
            private_exponent: private_exponent.to_vec(),
        }));
    }
    if algorithm == OID_BYTES_EC_PUBLIC_KEY {
        let algorithm_parameters = algorithm_identifier.read_oid()?;
        if algorithm_parameters != OID_BYTES_SECP256R1 {
            return Err(error_here!(ErrorType::UnsupportedInput));
        }
        let _version = private_key.read_unsigned_integer()?;
        let private_key_bytes = private_key.read_octet_string()?;
        return Ok(PrivateKeyInfo::EC(ECPrivateKey {
            private_key: private_key_bytes.to_vec(),
        }));
    }
    Err(error_here!(ErrorType::UnsupportedInput))
}

/// Helper macro for reading some bytes from a slice while checking the slice is long enough.
/// Returns a pair consisting of a slice of the bytes read and a slice of the rest of the bytes
/// from the original slice.
macro_rules! try_read_bytes {
    ($data:ident, $len:expr) => {{
        if $data.len() < $len {
            return Err(error_here!(ErrorType::TruncatedInput));
        }
        $data.split_at($len)
    }};
}

/// ASN.1 tag identifying an integer.
const INTEGER: u8 = 0x02;
/// ASN.1 tag identifying an octet string.
const OCTET_STRING: u8 = 0x04;
/// ASN.1 tag identifying a null value.
const NULL: u8 = 0x05;
/// ASN.1 tag identifying an object identifier (OID).
const OBJECT_IDENTIFIER: u8 = 0x06;
/// ASN.1 tag identifying a sequence.
const SEQUENCE: u8 = 0x10;
/// ASN.1 tag modifier identifying an item as constructed.
const CONSTRUCTED: u8 = 0x20;
/// ASN.1 tag modifier identifying an item as context-specific.
const CONTEXT_SPECIFIC: u8 = 0x80;

/// A helper struct for reading items from a DER SEQUENCE (in this case, all sequences are
/// assumed to be CONSTRUCTED).
struct Sequence<'a> {
    /// The contents of the SEQUENCE.
    contents: Der<'a>,
}

impl<'a> Sequence<'a> {
    fn new(input: &'a [u8]) -> Result<Sequence<'a>, Error> {
        let mut der = Der::new(input);
        let (_, _, sequence_bytes) = der.read_tlv(SEQUENCE | CONSTRUCTED)?;
        if !der.at_end() {
            return Err(error_here!(ErrorType::ExtraInput));
        }
        Ok(Sequence {
            contents: Der::new(sequence_bytes),
        })
    }

    fn read_unsigned_integer(&mut self) -> Result<&'a [u8], Error> {
        let (_, _, bytes) = self.contents.read_tlv(INTEGER)?;
        if bytes.is_empty() {
            return Err(error_here!(ErrorType::InvalidInput));
        }
        if bytes[0] == 0 && bytes.len() > 1 {
            let (_, integer) = bytes.split_at(1);
            Ok(integer)
        } else {
            Ok(bytes)
        }
    }

    fn read_octet_string(&mut self) -> Result<&'a [u8], Error> {
        let (_, _, bytes) = self.contents.read_tlv(OCTET_STRING)?;
        Ok(bytes)
    }

    fn read_oid(&mut self) -> Result<&'a [u8], Error> {
        let (_, _, bytes) = self.contents.read_tlv(OBJECT_IDENTIFIER)?;
        Ok(bytes)
    }

    fn read_null(&mut self) -> Result<(), Error> {
        let (_, _, bytes) = self.contents.read_tlv(NULL)?;
        if bytes.is_empty() {
            Ok(())
        } else {
            Err(error_here!(ErrorType::InvalidInput))
        }
    }

    fn read_sequence(&mut self) -> Result<Sequence<'a>, Error> {
        let (_, _, sequence_bytes) = self.contents.read_tlv(SEQUENCE | CONSTRUCTED)?;
        Ok(Sequence {
            contents: Der::new(sequence_bytes),
        })
    }

    fn read_optional_tagged_value(&mut self, tag: u8) -> Result<Option<&'a [u8]>, Error> {
        let expected = CONTEXT_SPECIFIC | CONSTRUCTED | tag;
        if self.contents.peek(expected) {
            let (_, _, tagged_value_bytes) = self.contents.read_tlv(expected)?;
            Ok(Some(tagged_value_bytes))
        } else {
            Ok(None)
        }
    }

    fn read_encoded_sequence_component(&mut self, tag: u8) -> Result<Vec<u8>, Error> {
        let (tag, length, value) = self.contents.read_tlv(tag)?;
        let mut encoded_component_bytes = length;
        encoded_component_bytes.insert(0, tag);
        encoded_component_bytes.extend_from_slice(value);
        Ok(encoded_component_bytes)
    }

    fn at_end(&self) -> bool {
        self.contents.at_end()
    }

    fn read_rest(&mut self) -> &[u8] {
        self.contents.read_rest()
    }
}

/// A helper struct for reading DER data. The contents are treated like a cursor, so its position
/// is updated as data is read.
struct Der<'a> {
    contents: &'a [u8],
}

impl<'a> Der<'a> {
    fn new(contents: &'a [u8]) -> Der<'a> {
        Der { contents }
    }

    /// Given an expected tag, reads the next (tag, lengh, value) from the contents. Most
    /// consumers will only be interested in the value, but some may want the entire encoded
    /// contents, in which case the returned tuple can be concatenated.
    fn read_tlv(&mut self, tag: u8) -> Result<(u8, Vec<u8>, &'a [u8]), Error> {
        let contents = self.contents;
        let (tag_read, rest) = try_read_bytes!(contents, 1);
        if tag_read[0] != tag {
            return Err(error_here!(ErrorType::InvalidInput));
        }
        let mut accumulated_length_bytes = Vec::with_capacity(4);
        let (length1, rest) = try_read_bytes!(rest, 1);
        accumulated_length_bytes.extend_from_slice(length1);
        let (length, to_read_from) = if length1[0] < 0x80 {
            (length1[0] as usize, rest)
        } else if length1[0] == 0x81 {
            let (length, rest) = try_read_bytes!(rest, 1);
            accumulated_length_bytes.extend_from_slice(length);
            if length[0] < 0x80 {
                return Err(error_here!(ErrorType::InvalidInput));
            }
            (length[0] as usize, rest)
        } else if length1[0] == 0x82 {
            let (mut lengths, rest) = try_read_bytes!(rest, 2);
            accumulated_length_bytes.extend_from_slice(lengths);
            let length = lengths
                .read_u16::<BigEndian>()
                .map_err(|_| error_here!(ErrorType::LibraryFailure))?;
            if length < 256 {
                return Err(error_here!(ErrorType::InvalidInput));
            }
            (length as usize, rest)
        } else {
            return Err(error_here!(ErrorType::UnsupportedInput));
        };
        let (contents, rest) = try_read_bytes!(to_read_from, length);
        self.contents = rest;
        Ok((tag, accumulated_length_bytes, contents))
    }

    fn at_end(&self) -> bool {
        self.contents.is_empty()
    }

    fn peek(&self, expected: u8) -> bool {
        Some(&expected) == self.contents.first()
    }

    fn read_rest(&mut self) -> &'a [u8] {
        let contents = self.contents;
        self.contents = &[];
        contents
    }
}
