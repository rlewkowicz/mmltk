use crate::core::backend;
use crate::futures::futures;

#[derive(Debug, thiserror::Error)]
pub enum Error {
        #[error("the futures executor could not be created")]
    ExecutorCreationFailed(futures::io::Error),

        #[error("the application window could not be created")]
    WindowCreationFailed(winit::error::OsError),

        #[error("the application graphics context could not be created")]
    GraphicsCreationFailed(backend::Error),
}

impl From<backend::Error> for Error {
    fn from(error: backend::Error) -> Error {
        Error::GraphicsCreationFailed(error)
    }
}
