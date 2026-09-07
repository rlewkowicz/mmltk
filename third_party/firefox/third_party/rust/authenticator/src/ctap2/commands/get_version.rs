use super::{CommandError, CtapResponse, RequestCtap1, Retryable};
use crate::consts::U2F_VERSION;
use crate::transport::errors::{ApduErrorStatus, HIDError};
use crate::transport::{FidoDevice, VirtualFidoDevice};
use crate::u2ftypes::CTAP1RequestAPDU;

#[allow(non_camel_case_types)]
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum U2FInfo {
    U2F_V2,
}

impl CtapResponse for U2FInfo {}

#[derive(Debug, Default)]
pub struct GetVersion {}

impl RequestCtap1 for GetVersion {
    type Output = U2FInfo;
    type AdditionalInfo = ();

    fn handle_response_ctap1<Dev: FidoDevice>(
        &self,
        _dev: &mut Dev,
        _status: Result<(), ApduErrorStatus>,
        input: &[u8],
        _add_info: &(),
    ) -> Result<Self::Output, Retryable<HIDError>> {
        if input.is_empty() {
            return Err(Retryable::Error(HIDError::Command(
                CommandError::InputTooSmall,
            )));
        }

        let expected = String::from("U2F_V2");
        let result = String::from_utf8_lossy(input);
        match result {
            ref data if data == &expected => Ok(U2FInfo::U2F_V2),
            _ => Err(Retryable::Error(HIDError::UnexpectedVersion)),
        }
    }

    fn ctap1_format(&self) -> Result<(Vec<u8>, ()), HIDError> {
        let flags = 0;

        let cmd = U2F_VERSION;
        let data = CTAP1RequestAPDU::serialize(cmd, flags, &[])?;
        Ok((data, ()))
    }

    fn send_to_virtual_device<Dev: VirtualFidoDevice>(
        &self,
        dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        dev.get_version(self)
    }
}
