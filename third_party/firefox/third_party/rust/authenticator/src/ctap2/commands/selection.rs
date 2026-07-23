use super::{Command, CommandError, RequestCtap2, StatusCode};
use crate::transport::errors::HIDError;
use crate::transport::{FidoDevice, VirtualFidoDevice};
use serde_cbor::{de::from_slice, Value};

#[derive(Debug, Default)]
pub struct Selection {}

impl RequestCtap2 for Selection {
    type Output = ();

    fn command(&self) -> Command {
        Command::Selection
    }

    fn wire_format(&self) -> Result<Vec<u8>, HIDError> {
        Ok(Vec::new())
    }

    fn handle_response_ctap2<Dev: FidoDevice>(
        &self,
        _dev: &mut Dev,
        input: &[u8],
    ) -> Result<Self::Output, HIDError> {
        if input.is_empty() {
            return Err(CommandError::InputTooSmall.into());
        }

        let status: StatusCode = input[0].into();

        if status.is_ok() {
            Ok(())
        } else {
            let msg = if input.len() > 1 {
                let data: Value = from_slice(&input[1..]).map_err(CommandError::Deserializing)?;
                Some(data)
            } else {
                None
            };
            Err(CommandError::StatusCode(status, msg).into())
        }
    }

    fn send_to_virtual_device<Dev: VirtualFidoDevice>(
        &self,
        dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        dev.selection(self)
    }
}
