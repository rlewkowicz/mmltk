//! The browser's only CBOR wire vocabulary.
//!
//! Views never receive CBOR keys or decoder values.  They receive installed
//! document bindings; this module is the deliberately small boundary between
//! the native reflected envelope and browser presentation code.

pub(crate) mod cbor;
pub mod client_records;
mod records;

pub use cbor::{
    Envelope, ProtocolError, decode_envelope, encode_envelope, encoded_len, object,
    reject_unknown_fields,
};
pub(crate) use cbor::{validate_client_dynamic_value, validate_server_dynamic_value};
pub use records::{
    ApplicationError, Bootstrap, IntentReply, ServerRecord, SystemEvent, decode_server,
};
