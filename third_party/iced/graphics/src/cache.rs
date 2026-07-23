use std::cell::RefCell;
use std::fmt;
use std::mem;
use std::sync::atomic::{self, AtomicU64};

pub struct Cache<T> {
    group: Group,
    state: RefCell<State<T>>,
    version: RefCell<u64>,
}

static VERSION: AtomicU64 = AtomicU64::new(0);

impl<T> Cache<T> {
        pub fn new() -> Self {
        Cache {
            group: Group::singleton(),
            state: RefCell::new(State::Empty { previous: None }),
            version: RefCell::new(VERSION.load(atomic::Ordering::Relaxed)),
        }
    }

                            pub fn with_group(group: Group) -> Self {
        assert!(
            !group.is_singleton(),
            "The group {group:?} cannot be shared!"
        );

        Cache {
            group,
            state: RefCell::new(State::Empty { previous: None }),
            version: RefCell::new(VERSION.load(atomic::Ordering::Relaxed)),
        }
    }

        pub fn group(&self) -> Group {
        self.group
    }

                        pub fn put(&self, value: T) {
        *self.state.borrow_mut() = State::Filled { current: value };
    }

        pub fn state(&self) -> &RefCell<State<T>> {
        let version = VERSION.load(atomic::Ordering::Relaxed);

        if *self.version.borrow() != version {
            *self.state.borrow_mut() = State::Empty { previous: None };
            let _ = self.version.replace(version);
        }

        &self.state
    }

        pub fn clear(&self) {
        let mut state = self.state.borrow_mut();

        let previous = mem::replace(&mut *state, State::Empty { previous: None });

        let previous = match previous {
            State::Empty { previous } => previous,
            State::Filled { current } => Some(current),
        };

        *state = State::Empty { previous };
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Group {
    id: u64,
    is_singleton: bool,
}

impl Group {
        pub fn unique() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);

        Self {
            id: NEXT.fetch_add(1, atomic::Ordering::Relaxed),
            is_singleton: false,
        }
    }

                                        pub fn is_singleton(self) -> bool {
        self.is_singleton
    }

    fn singleton() -> Self {
        Self {
            is_singleton: true,
            ..Self::unique()
        }
    }
}

impl<T> fmt::Debug for Cache<T>
where
    T: fmt::Debug,
{
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        use std::ops::Deref;

        let state = self.state.borrow();

        match state.deref() {
            State::Empty { previous } => {
                write!(f, "Cache::Empty {{ previous: {previous:?} }}")
            }
            State::Filled { current } => {
                write!(f, "Cache::Filled {{ current: {current:?} }}")
            }
        }
    }
}

impl<T> Default for Cache<T> {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State<T> {
        Empty {
                previous: Option<T>,
    },
        Filled {
                current: T,
    },
}

pub trait Cached: Sized {
        type Cache: Clone;

                fn load(cache: &Self::Cache) -> Self;

                fn cache(self, group: Group, previous: Option<Self::Cache>) -> Self::Cache;
}

#[cfg(debug_assertions)]
impl Cached for () {
    type Cache = ();

    fn load(_cache: &Self::Cache) -> Self {}

    fn cache(self, _group: Group, _previous: Option<Self::Cache>) -> Self::Cache {}
}

pub fn invalidate_all() {
    let _ = VERSION.fetch_add(1, atomic::Ordering::Relaxed);
}
