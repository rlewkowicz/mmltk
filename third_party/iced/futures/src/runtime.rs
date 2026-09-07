use crate::subscription;
use crate::{BoxStream, Executor, MaybeSend};

use futures::{Sink, SinkExt, channel::mpsc};
use std::marker::PhantomData;

#[derive(Debug)]
pub struct Runtime<Executor, Sender, Message> {
    executor: Executor,
    sender: Sender,
    subscriptions: subscription::Tracker,
    _message: PhantomData<Message>,
}

impl<Executor, Sender, Message> Runtime<Executor, Sender, Message>
where
    Executor: self::Executor,
    Sender: Sink<Message, Error = mpsc::SendError> + Unpin + MaybeSend + Clone + 'static,
    Message: MaybeSend + 'static,
{
                        pub fn new(executor: Executor, sender: Sender) -> Self {
        Self {
            executor,
            sender,
            subscriptions: subscription::Tracker::new(),
            _message: PhantomData,
        }
    }

                pub fn enter<R>(&self, f: impl FnOnce() -> R) -> R {
        self.executor.enter(f)
    }

        #[cfg(not(target_arch = "wasm32"))]
    pub fn block_on<T>(&mut self, future: impl Future<Output = T>) -> T {
        self.executor.block_on(future)
    }

                            pub fn run(&mut self, stream: BoxStream<Message>) {
        use futures::{FutureExt, StreamExt};

        let sender = self.sender.clone();
        let future = stream.map(Ok).forward(sender).map(|result| match result {
            Ok(()) => (),
            Err(error) => {
                log::warn!("Stream could not run until completion: {error}");
            }
        });

        self.executor.spawn(future);
    }

        pub fn send(&mut self, message: Message) {
        let mut sender = self.sender.clone();

        self.executor.spawn(async move {
            let _ = sender.send(message).await;
        });
    }

                                pub fn track(
        &mut self,
        recipes: impl IntoIterator<Item = Box<dyn subscription::Recipe<Output = Message>>>,
    ) {
        let Runtime {
            executor,
            subscriptions,
            sender,
            ..
        } = self;

        let futures = executor.enter(|| subscriptions.update(recipes.into_iter(), sender.clone()));

        for future in futures {
            executor.spawn(future);
        }
    }

                            pub fn broadcast(&mut self, event: subscription::Event) {
        self.subscriptions.broadcast(event);
    }
}
