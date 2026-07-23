#[cfg(not(target_arch = "wasm32"))]
mod platform {
    #[cfg(feature = "tokio")]
    pub use crate::backend::native::tokio::*;

    #[cfg(all(feature = "smol", not(feature = "tokio"),))]
    pub use crate::backend::native::smol::*;

    #[cfg(all(feature = "thread-pool", not(any(feature = "tokio", feature = "smol"))))]
    pub use crate::backend::native::thread_pool::*;

    #[cfg(not(any(feature = "tokio", feature = "smol", feature = "thread-pool")))]
    pub use crate::backend::null::*;
}

#[cfg(target_arch = "wasm32")]
mod platform {
    pub use crate::backend::wasm::wasm_bindgen::*;
}

pub use platform::*;
