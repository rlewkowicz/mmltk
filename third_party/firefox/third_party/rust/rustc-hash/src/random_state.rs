use std::collections::{HashMap, HashSet};

use crate::FxHasher;

/// Type alias for a hashmap using the `fx` hash algorithm with [`FxRandomState`].
pub type FxHashMapRand<K, V> = HashMap<K, V, FxRandomState>;

/// Type alias for a hashmap using the `fx` hash algorithm with [`FxRandomState`].
pub type FxHashSetRand<V> = HashSet<V, FxRandomState>;

/// `FxRandomState` is an alternative state for `HashMap` types.
///
/// A particular instance `FxRandomState` will create the same instances of
/// [`Hasher`], but the hashers created by two different `FxRandomState`
/// instances are unlikely to produce the same result for the same values.
#[derive(Clone)]
pub struct FxRandomState {
    seed: usize,
}

impl FxRandomState {
    /// Constructs a new `FxRandomState` that is initialized with random seed.
    pub fn new() -> FxRandomState {
        use rand::Rng;
        use std::{cell::Cell, thread_local};

        thread_local!(static SEED: Cell<usize> = {
            Cell::new(rand::thread_rng().gen())
        });

        SEED.with(|seed| {
            let s = seed.get();
            seed.set(s.wrapping_add(1));
            FxRandomState { seed: s }
        })
    }
}

impl core::hash::BuildHasher for FxRandomState {
    type Hasher = FxHasher;

    fn build_hasher(&self) -> Self::Hasher {
        FxHasher::with_seed(self.seed)
    }
}

impl Default for FxRandomState {
    fn default() -> Self {
        Self::new()
    }
}
