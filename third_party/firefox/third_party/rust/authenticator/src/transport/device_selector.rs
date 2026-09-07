use crate::transport::hid::HIDDevice;

pub use crate::transport::platform::device::Device;

use runloop::RunLoop;
use std::collections::{HashMap, HashSet};
use std::sync::mpsc::{channel, RecvTimeoutError, Sender};
use std::time::Duration;

pub type DeviceID = <Device as HIDDevice>::Id;
pub type DeviceBuildParameters = <Device as HIDDevice>::BuildParameters;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlinkResult {
    DeviceSelected,
    Cancelled,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceCommand {
    Blink,
    Cancel,
    Continue,
    Removed,
}

#[derive(Debug)]
pub enum DeviceSelectorEvent {
    Cancel,
    Timeout,
    DevicesAdded(Vec<DeviceID>),
    DeviceRemoved(DeviceID),
    NotAToken(DeviceID),
    ImAToken((DeviceID, Sender<DeviceCommand>)),
    SelectedToken(DeviceID),
}

pub struct DeviceSelector {
    /// How to send a message to the event loop
    sender: Sender<DeviceSelectorEvent>,
    /// Thread of the event loop
    runloop: RunLoop,
}

impl DeviceSelector {
    pub fn run() -> Self {
        let (selector_send, selector_rec) = channel();
        let runloop = RunLoop::new(move |alive| {
            let mut blinking = false;
            let mut waiting_for_response = HashSet::new();
            let mut tokens = HashMap::new();
            while alive() {
                let d = Duration::from_secs(100);
                let res = match selector_rec.recv_timeout(d) {
                    Err(RecvTimeoutError::Disconnected) => {
                        break;
                    }
                    Err(RecvTimeoutError::Timeout) => DeviceSelectorEvent::Timeout,
                    Ok(res) => res,
                };

                match res {
                    DeviceSelectorEvent::Timeout | DeviceSelectorEvent::Cancel => {
                        Self::cancel_all(tokens, None);
                        break;
                    }
                    DeviceSelectorEvent::SelectedToken(ref id) => {
                        Self::cancel_all(tokens, Some(id));
                        break; 
                    }
                    DeviceSelectorEvent::DevicesAdded(ids) => {
                        for id in ids {
                            debug!("Device added event: {:?}", id);
                            waiting_for_response.insert(id);
                        }
                        continue;
                    }
                    DeviceSelectorEvent::DeviceRemoved(ref id) => {
                        debug!("Device removed event: {:?}", id);
                        if !waiting_for_response.remove(id) {
                            tokens.iter().for_each(|(dev_id, tx)| {
                                if dev_id == id {
                                    let _ = tx.send(DeviceCommand::Removed);
                                }
                            });
                            tokens.retain(|dev_id, _| dev_id != id);
                            if tokens.is_empty() {
                                blinking = false;
                                continue;
                            }
                        }
                        if blinking {
                            continue;
                        }
                    }
                    DeviceSelectorEvent::NotAToken(ref id) => {
                        debug!("Device not a token event: {:?}", id);
                        waiting_for_response.remove(id);
                    }
                    DeviceSelectorEvent::ImAToken((id, tx)) => {
                        let _ = waiting_for_response.remove(&id);
                        if blinking {
                            if tx.send(DeviceCommand::Blink).is_ok() {
                                tokens.insert(id, tx.clone());
                            }
                            continue;
                        } else {
                            tokens.insert(id, tx.clone());
                        }
                    }
                }

                if waiting_for_response.is_empty() && !tokens.is_empty() {
                    if tokens.len() == 1 {
                        let (dev_id, tx) = tokens.drain().next().unwrap(); 
                        if tx.send(DeviceCommand::Continue).is_err() {
                            continue;
                        }
                        Self::cancel_all(tokens, Some(&dev_id));
                        break; 
                    } else {
                        blinking = true;

                        tokens.iter().for_each(|(_dev, tx)| {
                            let _ = tx.send(DeviceCommand::Blink);
                        });
                    }
                }
            }
        });
        Self {
            runloop: runloop.unwrap(), 
            sender: selector_send,
        }
    }

    pub fn clone_sender(&self) -> Sender<DeviceSelectorEvent> {
        self.sender.clone()
    }

    fn cancel_all(tokens: HashMap<DeviceID, Sender<DeviceCommand>>, exclude: Option<&DeviceID>) {
        for (dev_id, tx) in tokens.iter() {
            if Some(dev_id) != exclude {
                let _ = tx.send(DeviceCommand::Cancel);
            }
        }
    }

    pub fn stop(&mut self) {
        let _ = self.sender.send(DeviceSelectorEvent::Cancel);
        self.runloop.cancel();
    }
}
