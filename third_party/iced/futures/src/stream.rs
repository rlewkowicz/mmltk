use futures::channel::mpsc;
use futures::stream::{self, Stream, StreamExt};

pub fn channel<T>(size: usize, f: impl AsyncFnOnce(mpsc::Sender<T>)) -> impl Stream<Item = T> {
    let (sender, receiver) = mpsc::channel(size);

    let runner = stream::once(f(sender)).filter_map(|_| async { None });

    stream::select(receiver, runner)
}

pub fn try_channel<T, E>(
    size: usize,
    f: impl AsyncFnOnce(mpsc::Sender<T>) -> Result<(), E>,
) -> impl Stream<Item = Result<T, E>> {
    let (sender, receiver) = mpsc::channel(size);

    let runner = stream::once(f(sender)).filter_map(|result| async {
        match result {
            Ok(()) => None,
            Err(error) => Some(Err(error)),
        }
    });

    stream::select(receiver.map(Ok), runner)
}
