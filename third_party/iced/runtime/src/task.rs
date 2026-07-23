use crate::Action;
use crate::core::widget;
use crate::futures::futures::channel::mpsc;
use crate::futures::futures::channel::oneshot;
use crate::futures::futures::future::{self, FutureExt};
use crate::futures::futures::stream::{self, Stream, StreamExt};
use crate::futures::{BoxStream, MaybeSend, boxed_stream};

use std::convert::Infallible;
use std::pin::Pin;
use std::sync::Arc;
use std::task;
use std::thread;

#[cfg(feature = "sipper")]
#[doc(no_inline)]
pub use sipper::{Never, Sender, Sipper, Straw, sipper, stream};

#[must_use = "`Task` must be returned to the runtime to take effect; normally in your `update` or `new` functions."]
pub struct Task<T> {
    stream: Option<BoxStream<Action<T>>>,
    units: usize,
}

impl<T> Task<T> {
        pub fn none() -> Self {
        Self {
            stream: None,
            units: 0,
        }
    }

        pub fn done(value: T) -> Self
    where
        T: MaybeSend + 'static,
    {
        Self {
            stream: Some(boxed_stream(stream::once(future::ready(Action::Output(
                value,
            ))))),
            units: 0,
        }
    }

            pub fn perform<A>(
        future: impl Future<Output = A> + MaybeSend + 'static,
        f: impl FnOnce(A) -> T + MaybeSend + 'static,
    ) -> Self
    where
        T: MaybeSend + 'static,
        A: MaybeSend + 'static,
    {
        Self::future(future.map(f))
    }

            pub fn run<A>(
        stream: impl Stream<Item = A> + MaybeSend + 'static,
        f: impl Fn(A) -> T + MaybeSend + 'static,
    ) -> Self
    where
        T: 'static,
    {
        Self::stream(stream.map(f))
    }

            #[cfg(feature = "sipper")]
    pub fn sip<S>(
        sipper: S,
        on_progress: impl FnMut(S::Progress) -> T + MaybeSend + 'static,
        on_output: impl FnOnce(<S as Future>::Output) -> T + MaybeSend + 'static,
    ) -> Self
    where
        S: sipper::Core + MaybeSend + 'static,
        T: MaybeSend + 'static,
    {
        Self::stream(stream(sipper::sipper(move |sender| async move {
            on_output(sipper.with(on_progress).run(sender).await)
        })))
    }

            pub fn batch(tasks: impl IntoIterator<Item = Self>) -> Self
    where
        T: 'static,
    {
        let mut select_all = stream::SelectAll::new();
        let mut units = 0;

        for task in tasks.into_iter() {
            if let Some(stream) = task.stream {
                select_all.push(stream);
            }

            units += task.units;
        }

        Self {
            stream: Some(boxed_stream(select_all)),
            units,
        }
    }

        pub fn map<O>(self, mut f: impl FnMut(T) -> O + MaybeSend + 'static) -> Task<O>
    where
        T: MaybeSend + 'static,
        O: MaybeSend + 'static,
    {
        self.then(move |output| Task::done(f(output)))
    }

                        pub fn then<O>(self, mut f: impl FnMut(T) -> Task<O> + MaybeSend + 'static) -> Task<O>
    where
        T: MaybeSend + 'static,
        O: MaybeSend + 'static,
    {
        Task {
            stream: match self.stream {
                None => None,
                Some(stream) => Some(boxed_stream(stream.flat_map(move |action| {
                    match action.output() {
                        Ok(output) => f(output)
                            .stream
                            .unwrap_or_else(|| boxed_stream(stream::empty())),
                        Err(action) => boxed_stream(stream::once(async move { action })),
                    }
                }))),
            },
            units: self.units,
        }
    }

        pub fn chain(self, task: Self) -> Self
    where
        T: 'static,
    {
        match self.stream {
            None => task,
            Some(first) => match task.stream {
                None => Self {
                    stream: Some(first),
                    units: self.units,
                },
                Some(second) => Self {
                    stream: Some(boxed_stream(first.chain(second))),
                    units: self.units + task.units,
                },
            },
        }
    }

        pub fn collect(self) -> Task<Vec<T>>
    where
        T: MaybeSend + 'static,
    {
        match self.stream {
            None => Task::done(Vec::new()),
            Some(stream) => Task {
                stream: Some(boxed_stream(
                    stream::unfold(
                        (stream, Some(Vec::new())),
                        move |(mut stream, outputs)| async move {
                            let mut outputs = outputs?;

                            let Some(action) = stream.next().await else {
                                return Some((Some(Action::Output(outputs)), (stream, None)));
                            };

                            match action.output() {
                                Ok(output) => {
                                    outputs.push(output);

                                    Some((None, (stream, Some(outputs))))
                                }
                                Err(action) => Some((Some(action), (stream, Some(outputs)))),
                            }
                        },
                    )
                    .filter_map(future::ready),
                )),
                units: self.units,
            },
        }
    }

                pub fn discard<O>(self) -> Task<O>
    where
        T: MaybeSend + 'static,
        O: MaybeSend + 'static,
    {
        self.then(|_| Task::none())
    }

        pub fn abortable(self) -> (Self, Handle)
    where
        T: 'static,
    {
        let (stream, handle) = match self.stream {
            Some(stream) => {
                let (stream, handle) = stream::abortable(stream);

                (Some(boxed_stream(stream)), InternalHandle::Manual(handle))
            }
            None => (
                None,
                InternalHandle::Manual(stream::AbortHandle::new_pair().0),
            ),
        };

        (
            Self {
                stream,
                units: self.units,
            },
            Handle { internal: handle },
        )
    }

            pub fn future(future: impl Future<Output = T> + MaybeSend + 'static) -> Self
    where
        T: 'static,
    {
        Self::stream(stream::once(future))
    }

            pub fn stream(stream: impl Stream<Item = T> + MaybeSend + 'static) -> Self
    where
        T: 'static,
    {
        Self {
            stream: Some(boxed_stream(
                stream::once(yield_now())
                    .filter_map(|_| async { None })
                    .chain(stream.map(Action::Output)),
            )),
            units: 1,
        }
    }

        pub fn units(&self) -> usize {
        self.units
    }
}

impl<T> std::fmt::Debug for Task<T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct(&format!("Task<{}>", std::any::type_name::<T>()))
            .field("units", &self.units)
            .finish()
    }
}

#[derive(Debug, Clone)]
pub struct Handle {
    internal: InternalHandle,
}

#[derive(Debug, Clone)]
enum InternalHandle {
    Manual(stream::AbortHandle),
    AbortOnDrop(Arc<stream::AbortHandle>),
}

impl InternalHandle {
    pub fn as_ref(&self) -> &stream::AbortHandle {
        match self {
            InternalHandle::Manual(handle) => handle,
            InternalHandle::AbortOnDrop(handle) => handle.as_ref(),
        }
    }
}

impl Handle {
        pub fn abort(&self) {
        self.internal.as_ref().abort();
    }

                                    pub fn abort_on_drop(self) -> Self {
        match &self.internal {
            InternalHandle::Manual(handle) => Self {
                internal: InternalHandle::AbortOnDrop(Arc::new(handle.clone())),
            },
            InternalHandle::AbortOnDrop(_) => self,
        }
    }

        pub fn is_aborted(&self) -> bool {
        self.internal.as_ref().is_aborted()
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        if let InternalHandle::AbortOnDrop(handle) = &mut self.internal {
            let handle = std::mem::replace(handle, Arc::new(stream::AbortHandle::new_pair().0));

            if let Some(handle) = Arc::into_inner(handle) {
                handle.abort();
            }
        }
    }
}

impl<T> Task<Option<T>> {
                pub fn and_then<A>(self, f: impl Fn(T) -> Task<A> + MaybeSend + 'static) -> Task<A>
    where
        T: MaybeSend + 'static,
        A: MaybeSend + 'static,
    {
        self.then(move |option| option.map_or_else(Task::none, &f))
    }
}

impl<T, E> Task<Result<T, E>> {
                pub fn and_then<A>(
        self,
        f: impl Fn(T) -> Task<Result<A, E>> + MaybeSend + 'static,
    ) -> Task<Result<A, E>>
    where
        T: MaybeSend + 'static,
        E: MaybeSend + 'static,
        A: MaybeSend + 'static,
    {
        self.then(move |result| result.map_or_else(|error| Task::done(Err(error)), &f))
    }

            pub fn map_err<E2>(self, f: impl Fn(E) -> E2 + MaybeSend + 'static) -> Task<Result<T, E2>>
    where
        T: MaybeSend + 'static,
        E: MaybeSend + 'static,
        E2: MaybeSend + 'static,
    {
        self.map(move |result| result.map_err(&f))
    }
}

impl<T> Default for Task<T> {
    fn default() -> Self {
        Self::none()
    }
}

impl<T> From<()> for Task<T> {
    fn from(_value: ()) -> Self {
        Self::none()
    }
}

pub fn widget<T>(operation: impl widget::Operation<T> + 'static) -> Task<T>
where
    T: Send + 'static,
{
    channel(move |sender| {
        let operation = widget::operation::map(Box::new(operation), move |value| {
            let _ = sender.clone().try_send(value);
        });

        Action::Widget(Box::new(operation))
    })
}

pub fn oneshot<T>(f: impl FnOnce(oneshot::Sender<T>) -> Action<T>) -> Task<T>
where
    T: MaybeSend + 'static,
{
    let (sender, receiver) = oneshot::channel();

    let action = f(sender);

    Task {
        stream: Some(boxed_stream(
            stream::once(async move { action }).chain(
                receiver
                    .into_stream()
                    .filter_map(|result| async move { Some(Action::Output(result.ok()?)) }),
            ),
        )),
        units: 1,
    }
}

pub fn channel<T>(f: impl FnOnce(mpsc::Sender<T>) -> Action<T>) -> Task<T>
where
    T: MaybeSend + 'static,
{
    let (sender, receiver) = mpsc::channel(1);

    let action = f(sender);

    Task {
        stream: Some(boxed_stream(
            stream::once(async move { action })
                .chain(receiver.map(|result| Action::Output(result))),
        )),
        units: 1,
    }
}

pub fn effect<T>(action: impl Into<Action<Infallible>>) -> Task<T> {
    let action = action.into();

    Task {
        stream: Some(boxed_stream(stream::once(async move {
            action.output().expect_err("no output")
        }))),
        units: 1,
    }
}

pub fn into_stream<T>(task: Task<T>) -> Option<BoxStream<Action<T>>> {
    task.stream
}

pub fn blocking<T>(f: impl FnOnce(mpsc::Sender<T>) + Send + 'static) -> Task<T>
where
    T: Send + 'static,
{
    let (sender, receiver) = mpsc::channel(1);

    let _ = thread::spawn(move || {
        f(sender);
    });

    Task::stream(receiver)
}

pub fn try_blocking<T, E>(
    f: impl FnOnce(mpsc::Sender<T>) -> Result<(), E> + Send + 'static,
) -> Task<Result<T, E>>
where
    T: Send + 'static,
    E: Send + 'static,
{
    let (sender, receiver) = mpsc::channel(1);
    let (error_sender, error_receiver) = oneshot::channel();

    let _ = thread::spawn(move || {
        if let Err(error) = f(sender) {
            let _ = error_sender.send(Err(error));
        }
    });

    Task::stream(stream::select(
        receiver.map(Ok),
        stream::once(error_receiver).filter_map(async |result| result.ok()),
    ))
}

async fn yield_now() {
    struct YieldNow {
        yielded: bool,
    }

    impl Future for YieldNow {
        type Output = ();

        fn poll(mut self: Pin<&mut Self>, cx: &mut task::Context<'_>) -> task::Poll<()> {
            if self.yielded {
                return task::Poll::Ready(());
            }

            self.yielded = true;

            cx.waker().wake_by_ref();

            task::Poll::Pending
        }
    }

    YieldNow { yielded: false }.await;
}
