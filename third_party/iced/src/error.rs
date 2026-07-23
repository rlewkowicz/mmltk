use crate::backend;
use crate::futures;
use crate::shell;

#[derive(Debug, thiserror::Error)]
pub enum Error {
        #[error("the futures executor could not be created")]
    ExecutorCreationFailed(futures::io::Error),

        #[error("the application window could not be created")]
    WindowCreationFailed(Box<dyn std::error::Error + Send + Sync>),

        #[error("the application graphics context could not be created")]
    GraphicsCreationFailed(backend::Error),
}

impl From<shell::Error> for Error {
    fn from(error: shell::Error) -> Error {
        match error {
            shell::Error::ExecutorCreationFailed(error) => Error::ExecutorCreationFailed(error),
            shell::Error::WindowCreationFailed(error) => {
                Error::WindowCreationFailed(Box::new(error))
            }
            shell::Error::GraphicsCreationFailed(error) => Error::GraphicsCreationFailed(error),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn assert_send_sync() {
        fn _assert<T: Send + Sync>() {}
        _assert::<Error>();
    }
}
