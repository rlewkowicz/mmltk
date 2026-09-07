use crate::MaybeSend;

pub trait Executor: Sized {
        fn new() -> Result<Self, futures::io::Error>
    where
        Self: Sized;

        fn spawn(&self, future: impl Future<Output = ()> + MaybeSend + 'static);

        #[cfg(not(target_arch = "wasm32"))]
    fn block_on<T>(&self, future: impl Future<Output = T>) -> T;

                            fn enter<R>(&self, f: impl FnOnce() -> R) -> R {
        f()
    }
}
