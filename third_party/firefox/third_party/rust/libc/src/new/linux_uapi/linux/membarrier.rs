//! Header: `uapi/linux/membarrier.h`

use crate::prelude::*;

c_enum! {
    #[repr(c_int)]
    pub enum membarrier_cmd {
        pub MEMBARRIER_CMD_QUERY = 0,
        pub MEMBARRIER_CMD_GLOBAL = 1 << 0,
        pub MEMBARRIER_CMD_GLOBAL_EXPEDITED = 1 << 1,
        pub MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED = 1 << 2,
        pub MEMBARRIER_CMD_PRIVATE_EXPEDITED = 1 << 3,
        pub MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED = 1 << 4,
        pub MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE = 1 << 5,
        pub MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE = 1 << 6,
        pub MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ = 1 << 7,
        pub MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ = 1 << 8,
    }
}
