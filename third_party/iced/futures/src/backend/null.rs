use crate::MaybeSend;

#[derive(Debug)]
pub struct Executor;

impl crate::Executor for Executor {
    fn new() -> Result<Self, futures::io::Error> {
        Ok(Self)
    }

    fn spawn(&self, _future: impl Future<Output = ()> + MaybeSend + 'static) {}

    #[cfg(not(target_arch = "wasm32"))]
    fn block_on<T>(&self, _future: impl Future<Output = T>) -> T {
        unimplemented!()
    }
}

pub mod time {
    }
