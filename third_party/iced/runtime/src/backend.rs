use crate::Task;
use crate::core::backend;
use crate::futures::futures::channel::oneshot;
use crate::task;

#[derive(Debug)]
pub enum Action {
        Configure(
        backend::Settings,
        oneshot::Sender<Result<(), backend::Error>>,
    ),
}

pub fn configure(settings: backend::Settings) -> Task<Result<(), backend::Error>> {
    task::oneshot(|sender| crate::Action::Backend(Action::Configure(settings, sender)))
}
