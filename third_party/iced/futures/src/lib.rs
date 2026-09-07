#![doc(
    html_logo_url = "https://raw.githubusercontent.com/iced-rs/iced/9ab6923e943f784985e9ef9ca28b10278297225d/docs/logo.svg"
)]
#![cfg_attr(docsrs, feature(doc_cfg))]
pub use futures;
pub use iced_core as core;

mod maybe;
mod runtime;

pub mod backend;
pub mod event;
pub mod executor;
pub mod keyboard;
pub mod stream;
pub mod subscription;

pub use executor::Executor;
pub use maybe::{MaybeSend, MaybeSync};
pub use platform::*;
pub use runtime::Runtime;
pub use subscription::Subscription;

#[cfg(not(target_arch = "wasm32"))]
mod platform {
                    pub type BoxFuture<T> = futures::future::BoxFuture<'static, T>;

                    pub type BoxStream<T> = futures::stream::BoxStream<'static, T>;

                    pub fn boxed_stream<T, S>(stream: S) -> BoxStream<T>
    where
        S: futures::Stream<Item = T> + Send + 'static,
    {
        futures::stream::StreamExt::boxed(stream)
    }
}

#[cfg(target_arch = "wasm32")]
mod platform {
                    pub type BoxFuture<T> = futures::future::LocalBoxFuture<'static, T>;

                    pub type BoxStream<T> = futures::stream::LocalBoxStream<'static, T>;

                    pub fn boxed_stream<T, S>(stream: S) -> BoxStream<T>
    where
        S: futures::Stream<Item = T> + 'static,
    {
        futures::stream::StreamExt::boxed_local(stream)
    }
}
