use crate::core::image::Handle;
use crate::futures::futures::channel::oneshot;
use crate::task::{self, Task};

pub use crate::core::image::{Allocation, Error};

#[derive(Debug)]
pub enum Action {
        Allocate(Handle, oneshot::Sender<Result<Allocation, Error>>),
}

pub fn allocate(handle: impl Into<Handle>) -> Task<Result<Allocation, Error>> {
    task::oneshot(|sender| crate::Action::Image(Action::Allocate(handle.into(), sender)))
}
