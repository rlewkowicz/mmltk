mod tracker;

pub use tracker::Tracker;

use crate::core::event;
use crate::core::theme;
use crate::core::window;
use crate::futures::Stream;
use crate::{BoxStream, MaybeSend};

use std::any::TypeId;
use std::hash::Hash;

#[derive(Debug, Clone, PartialEq)]
pub enum Event {
        Interaction {
                window: window::Id,
                                event: event::Event,

                status: event::Status,
    },

        SystemThemeChanged(theme::Mode),

        PlatformSpecific(PlatformSpecific),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PlatformSpecific {
        MacOS(MacOS),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MacOS {
                        ReceivedUrl(String),
}

pub type EventStream = BoxStream<Event>;

pub type Hasher = rustc_hash::FxHasher;

#[must_use = "`Subscription` must be returned to the runtime to take effect; normally in your `subscription` function."]
pub struct Subscription<T> {
    recipes: Vec<Box<dyn Recipe<Output = T>>>,
}

impl<T> Subscription<T> {
        pub fn none() -> Self {
        Self {
            recipes: Vec::new(),
        }
    }

                                                                                                                                                                                                                                                                                            pub fn run<S>(builder: fn() -> S) -> Self
    where
        S: Stream<Item = T> + MaybeSend + 'static,
        T: 'static,
    {
        from_recipe(Runner {
            data: builder,
            spawn: |builder, _| builder(),
        })
    }

                        pub fn run_with<D, S>(data: D, builder: fn(&D) -> S) -> Self
    where
        D: Hash + 'static,
        S: Stream<Item = T> + MaybeSend + 'static,
        T: 'static,
    {
        from_recipe(Runner {
            data: (data, builder),
            spawn: |(data, builder), _| builder(data),
        })
    }

            pub fn batch(subscriptions: impl IntoIterator<Item = Subscription<T>>) -> Self {
        Self {
            recipes: subscriptions
                .into_iter()
                .flat_map(|subscription| subscription.recipes)
                .collect(),
        }
    }

                pub fn with<A>(self, value: A) -> Subscription<(A, T)>
    where
        T: 'static,
        A: std::hash::Hash + Clone + Send + Sync + 'static,
    {
        struct With<A, B> {
            recipe: Box<dyn Recipe<Output = A>>,
            value: B,
        }

        impl<A, B> Recipe for With<A, B>
        where
            A: 'static,
            B: 'static + std::hash::Hash + Clone + Send + Sync,
        {
            type Output = (B, A);

            fn hash(&self, state: &mut Hasher) {
                std::any::TypeId::of::<B>().hash(state);
                self.value.hash(state);
                self.recipe.hash(state);
            }

            fn stream(self: Box<Self>, input: EventStream) -> BoxStream<Self::Output> {
                use futures::StreamExt;

                let value = self.value;

                Box::pin(
                    self.recipe
                        .stream(input)
                        .map(move |element| (value.clone(), element)),
                )
            }
        }

        Subscription {
            recipes: self
                .recipes
                .into_iter()
                .map(|recipe| {
                    Box::new(With {
                        recipe,
                        value: value.clone(),
                    }) as Box<dyn Recipe<Output = (A, T)>>
                })
                .collect(),
        }
    }

                pub fn map<F, A>(self, f: F) -> Subscription<A>
    where
        T: 'static,
        F: Fn(T) -> A + MaybeSend + Clone + 'static,
        A: 'static,
    {
        const {
            check_zero_sized::<F>();
        }

        struct Map<A, B, F>
        where
            F: Fn(A) -> B + 'static,
        {
            recipe: Box<dyn Recipe<Output = A>>,
            mapper: F,
        }

        impl<A, B, F> Recipe for Map<A, B, F>
        where
            A: 'static,
            B: 'static,
            F: Fn(A) -> B + 'static + MaybeSend,
        {
            type Output = B;

            fn hash(&self, state: &mut Hasher) {
                TypeId::of::<F>().hash(state);
                self.recipe.hash(state);
            }

            fn stream(self: Box<Self>, input: EventStream) -> BoxStream<Self::Output> {
                use futures::StreamExt;

                Box::pin(self.recipe.stream(input).map(self.mapper))
            }
        }

        Subscription {
            recipes: self
                .recipes
                .into_iter()
                .map(|recipe| {
                    Box::new(Map {
                        recipe,
                        mapper: f.clone(),
                    }) as Box<dyn Recipe<Output = A>>
                })
                .collect(),
        }
    }

                    pub fn filter_map<F, A>(mut self, f: F) -> Subscription<A>
    where
        T: MaybeSend + 'static,
        F: Fn(T) -> Option<A> + MaybeSend + Clone + 'static,
        A: MaybeSend + 'static,
    {
        const {
            check_zero_sized::<F>();
        }

        struct FilterMap<A, B, F>
        where
            F: Fn(A) -> Option<B> + 'static,
        {
            recipe: Box<dyn Recipe<Output = A>>,
            mapper: F,
        }

        impl<A, B, F> Recipe for FilterMap<A, B, F>
        where
            A: 'static,
            B: 'static + MaybeSend,
            F: Fn(A) -> Option<B> + MaybeSend,
        {
            type Output = B;

            fn hash(&self, state: &mut Hasher) {
                TypeId::of::<F>().hash(state);
                self.recipe.hash(state);
            }

            fn stream(self: Box<Self>, input: EventStream) -> BoxStream<Self::Output> {
                use futures::StreamExt;
                use futures::future;

                let mapper = self.mapper;

                Box::pin(
                    self.recipe
                        .stream(input)
                        .filter_map(move |a| future::ready(mapper(a))),
                )
            }
        }

        Subscription {
            recipes: self
                .recipes
                .drain(..)
                .map(|recipe| {
                    Box::new(FilterMap {
                        recipe,
                        mapper: f.clone(),
                    }) as Box<dyn Recipe<Output = A>>
                })
                .collect(),
        }
    }

        pub fn units(&self) -> usize {
        self.recipes.len()
    }
}

pub fn from_recipe<T>(recipe: impl Recipe<Output = T> + 'static) -> Subscription<T> {
    Subscription {
        recipes: vec![Box::new(recipe)],
    }
}

pub fn into_recipes<T>(subscription: Subscription<T>) -> Vec<Box<dyn Recipe<Output = T>>> {
    subscription.recipes
}

impl<T> std::fmt::Debug for Subscription<T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Subscription").finish()
    }
}

pub trait Recipe {
            type Output;

                fn hash(&self, state: &mut Hasher);

            fn stream(self: Box<Self>, input: EventStream) -> BoxStream<Self::Output>;
}

pub fn filter_map<I, F, T>(id: I, f: F) -> Subscription<T>
where
    I: Hash + 'static,
    F: Fn(Event) -> Option<T> + MaybeSend + 'static,
    T: 'static + MaybeSend,
{
    from_recipe(Runner {
        data: id,
        spawn: |_, events| {
            use futures::future;
            use futures::stream::StreamExt;

            events.filter_map(move |event| future::ready(f(event)))
        },
    })
}

struct Runner<I, F, S, T>
where
    F: FnOnce(&I, EventStream) -> S,
    S: Stream<Item = T>,
{
    data: I,
    spawn: F,
}

impl<I, F, S, T> Recipe for Runner<I, F, S, T>
where
    I: Hash + 'static,
    F: FnOnce(&I, EventStream) -> S,
    S: Stream<Item = T> + MaybeSend + 'static,
{
    type Output = T;

    fn hash(&self, state: &mut Hasher) {
        std::any::TypeId::of::<I>().hash(state);
        self.data.hash(state);
    }

    fn stream(self: Box<Self>, input: EventStream) -> BoxStream<Self::Output> {
        crate::boxed_stream((self.spawn)(&self.data, input))
    }
}

const fn check_zero_sized<T>() {
    if std::mem::size_of::<T>() != 0 {
        panic!(
            "The Subscription closure provided is not non-capturing. \
            Closures given to Subscription::map or filter_map cannot \
            capture external variables. If you need to capture state, \
            consider using Subscription::with."
        );
    }
}
