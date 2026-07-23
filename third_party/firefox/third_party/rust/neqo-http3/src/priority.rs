// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::fmt;

use neqo_transport::StreamId;
use sfv::{BareItem, Dictionary, Integer, Item, ListEntry, Parser};

use crate::{Error, Res, frames::HFrame};

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct Priority {
    urgency: u8,
    incremental: bool,
}

impl Default for Priority {
    fn default() -> Self {
        Self {
            urgency: 3,
            incremental: false,
        }
    }
}

impl Priority {
    /// # Panics
    ///
    /// If an invalid urgency (>7 is given)
    #[must_use]
    pub fn new(urgency: u8, incremental: bool) -> Self {
        assert!(urgency < 8);
        let priority = Self {
            urgency,
            incremental,
        };
#[cfg(any())]

        neqo_common::write_item_to_fuzzing_corpus("priority", priority.to_string().as_bytes());
        priority
    }

    /// Constructs a priority from raw bytes (either a field value of frame content).
    ///
    /// # Errors
    ///
    /// When the contained syntax is invalid.
    pub fn from_bytes(bytes: &[u8]) -> Res<Self> {
#[cfg(any())]

        neqo_common::write_item_to_fuzzing_corpus("priority", bytes);

        let dict: Dictionary = Parser::new(bytes).parse().map_err(|_| Error::HttpFrame)?;
        let urgency = match dict.get("u") {
            Some(ListEntry::Item(Item {
                bare_item: BareItem::Integer(u),
                ..
            })) if (Integer::constant(0)..=Integer::constant(7)).contains(u) => {
                u8::try_from(*u).map_err(|_| Error::Internal)?
            }
            _ => 3,
        };
        let incremental = match dict.get("i") {
            Some(ListEntry::Item(Item {
                bare_item: BareItem::Boolean(i),
                ..
            })) => *i,
            _ => false,
        };
        Ok(Self {
            urgency,
            incremental,
        })
    }
}

impl fmt::Display for Priority {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self {
                urgency: 3,
                incremental: false,
            } => Ok(()),
            Self {
                urgency: 3,
                incremental: true,
            } => write!(f, "i"),
            Self {
                urgency,
                incremental: false,
            } => write!(f, "u={urgency}"),
            Self {
                urgency,
                incremental: true,
            } => write!(f, "u={urgency},i"),
        }
    }
}

#[derive(Debug)]
pub struct PriorityHandler {
    push_stream: bool,
    priority: Priority,
    last_send_priority: Priority,
}

impl PriorityHandler {
    pub const fn new(push_stream: bool, priority: Priority) -> Self {
        Self {
            push_stream,
            priority,
            last_send_priority: priority,
        }
    }


    /// Returns if an priority update will be issued
    pub fn maybe_update_priority(&mut self, priority: Priority) -> bool {
        if priority == self.priority {
            false
        } else {
            self.priority = priority;
            true
        }
    }

    pub const fn priority_update_sent(&mut self) {
        self.last_send_priority = self.priority;
    }

    /// Returns `HFrame` if an priority update is outstanding
    pub fn maybe_encode_frame(&self, stream_id: StreamId) -> Option<HFrame> {
        if self.priority == self.last_send_priority {
            None
        } else if self.push_stream {
            Some(HFrame::PriorityUpdatePush {
                element_id: stream_id.as_u64(),
                priority: self.priority,
            })
        } else {
            Some(HFrame::PriorityUpdateRequest {
                element_id: stream_id.as_u64(),
                priority: self.priority,
            })
        }
    }
}
