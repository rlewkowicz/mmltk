use crate::subscription::{Event, Hasher, Recipe};
use crate::{BoxFuture, MaybeSend};

use futures::channel::mpsc;
use futures::sink::{Sink, SinkExt};
use rustc_hash::FxHashMap;

use std::hash::Hasher as _;

#[derive(Debug, Default)]
pub struct Tracker {
    subscriptions: FxHashMap<u64, Execution>,
}

#[derive(Debug)]
pub struct Execution {
    _cancel: futures::channel::oneshot::Sender<()>,
    listener: Option<futures::channel::mpsc::Sender<Event>>,
}

impl Tracker {
        pub fn new() -> Self {
        Self {
            subscriptions: FxHashMap::default(),
        }
    }

                                                                                pub fn update<Message, Receiver>(
        &mut self,
        recipes: impl Iterator<Item = Box<dyn Recipe<Output = Message>>>,
        receiver: Receiver,
    ) -> Vec<BoxFuture<()>>
    where
        Message: 'static + MaybeSend,
        Receiver: 'static + Sink<Message, Error = mpsc::SendError> + Unpin + MaybeSend + Clone,
    {
        use futures::stream::StreamExt;

        let mut futures: Vec<BoxFuture<()>> = Vec::new();
        let mut alive = std::collections::HashSet::new();

        for recipe in recipes {
            let id = {
                let mut hasher = Hasher::default();
                recipe.hash(&mut hasher);

                hasher.finish()
            };

            let _ = alive.insert(id);

            if self.subscriptions.contains_key(&id) {
                continue;
            }

            let (cancel, mut canceled) = futures::channel::oneshot::channel();

            let (event_sender, event_receiver) = futures::channel::mpsc::channel(100);

            let mut receiver = receiver.clone();
            let mut stream = recipe.stream(event_receiver.boxed());

            let future = async move {
                loop {
                    let select = futures::future::select(&mut canceled, stream.next());

                    match select.await {
                        futures::future::Either::Left(_)
                        | futures::future::Either::Right((None, _)) => break,
                        futures::future::Either::Right((Some(message), _)) => {
                            let _ = receiver.send(message).await;
                        }
                    }
                }
            };

            let _ = self.subscriptions.insert(
                id,
                Execution {
                    _cancel: cancel,
                    listener: if event_sender.is_closed() {
                        None
                    } else {
                        Some(event_sender)
                    },
                },
            );

            futures.push(Box::pin(future));
        }

        self.subscriptions.retain(|id, _| alive.contains(id));

        futures
    }

                                            pub fn broadcast(&mut self, event: Event) {
        self.subscriptions
            .values_mut()
            .filter_map(|connection| connection.listener.as_mut())
            .for_each(|listener| {
                if let Err(error) = listener.try_send(event.clone()) {
                    log::warn!("Error sending event to subscription: {error:?}");
                }
            });
    }
}
