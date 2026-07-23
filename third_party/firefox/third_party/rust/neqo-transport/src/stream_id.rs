// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::fmt::{self, Display, Formatter};

use enum_map::Enum;
use neqo_common::Role;

/// The type of stream, either Bi-Directional or Uni-Directional.
/// The discriminant values match the QUIC stream type bits.
#[derive(PartialEq, Debug, Copy, Clone, PartialOrd, Eq, Ord, Hash, Enum)]
#[repr(u64)]
pub enum StreamType {
    BiDi = 0,
    UniDi = 2,
}

#[derive(Debug, Eq, PartialEq, Clone, Copy, Ord, PartialOrd, Hash, Default)]
pub struct StreamId(u64);

impl StreamId {
    #[must_use]
    pub const fn new(id: u64) -> Self {
        Self(id)
    }

    #[must_use]
    pub const fn init(stream_type: StreamType, role: Role) -> Self {
        Self(stream_type as u64 + Self::role_bit(role))
    }

    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }

    #[must_use]
    pub const fn is_bidi(self) -> bool {
        self.as_u64() & 0x02 == 0
    }

    #[must_use]
    pub const fn is_uni(self) -> bool {
        !self.is_bidi()
    }

    #[must_use]
    pub const fn stream_type(self) -> StreamType {
        if self.is_bidi() {
            StreamType::BiDi
        } else {
            StreamType::UniDi
        }
    }

    #[must_use]
    pub const fn is_client_initiated(self) -> bool {
        self.as_u64() & 0x01 == 0
    }

    #[must_use]
    pub const fn is_server_initiated(self) -> bool {
        !self.is_client_initiated()
    }

    #[must_use]
    pub const fn role(self) -> Role {
        if self.is_client_initiated() {
            Role::Client
        } else {
            Role::Server
        }
    }

    #[must_use]
    pub const fn is_self_initiated(self, my_role: Role) -> bool {
        match my_role {
            Role::Client if self.is_client_initiated() => true,
            Role::Server if self.is_server_initiated() => true,
            _ => false,
        }
    }

    #[must_use]
    pub const fn is_remote_initiated(self, my_role: Role) -> bool {
        !self.is_self_initiated(my_role)
    }

    #[must_use]
    pub const fn is_send_only(self, my_role: Role) -> bool {
        self.is_uni() && self.is_self_initiated(my_role)
    }

    #[must_use]
    pub const fn is_recv_only(self, my_role: Role) -> bool {
        self.is_uni() && self.is_remote_initiated(my_role)
    }

    pub const fn next(&mut self) {
        self.0 += 4;
    }

    /// Return the stream index for this stream ID.
    #[must_use]
    pub const fn index(self) -> u64 {
        self.0 >> 2
    }

    /// This returns a bit that is shared by all streams created by this role.
    #[must_use]
    pub const fn role_bit(role: Role) -> u64 {
        match role {
            Role::Server => 1,
            Role::Client => 0,
        }
    }
}

impl From<u64> for StreamId {
    fn from(val: u64) -> Self {
        Self::new(val)
    }
}

impl PartialEq<u64> for StreamId {
    fn eq(&self, other: &u64) -> bool {
        self.as_u64() == *other
    }
}

impl Display for StreamId {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "{}", self.as_u64())
    }
}
