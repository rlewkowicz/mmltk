macro_rules! os_required {
    () => {
        panic!("mio must be compiled with `os-poll` to run.")
    };
}

mod selector;
pub(crate) use self::selector::{event, Event, Events, Selector};

#[cfg(not(target_os = "wasi"))]
mod waker;
#[cfg(not(target_os = "wasi"))]
pub(crate) use self::waker::Waker;

cfg_net! {
    pub(crate) mod tcp;
    pub(crate) mod udp;
pub(crate) mod uds;
}

cfg_io_source! {
    use std::io;
use std::os::fd::RawFd;
    #[cfg(target_os = "hermit")]
    use std::os::hermit::io::RawFd;

use crate::{Registry, Token, Interest};

    pub(crate) struct IoSourceState;

    impl IoSourceState {
        pub fn new() -> IoSourceState {
            IoSourceState
        }

        pub fn do_io<T, F, R>(&self, f: F, io: &T) -> io::Result<R>
        where
            F: FnOnce(&T) -> io::Result<R>,
        {
            f(io)
        }
    }

impl IoSourceState {
        pub fn register(
            &mut self,
            _: &Registry,
            _: Token,
            _: Interest,
            _: RawFd,
        ) -> io::Result<()> {
            os_required!()
        }

        pub fn reregister(
            &mut self,
            _: &Registry,
            _: Token,
            _: Interest,
            _: RawFd,
        ) -> io::Result<()> {
           os_required!()
        }

        pub fn deregister(&mut self, _: &Registry, _: RawFd) -> io::Result<()> {
            os_required!()
        }
    }

}
