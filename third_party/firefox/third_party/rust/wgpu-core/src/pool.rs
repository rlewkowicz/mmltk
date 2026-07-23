use alloc::sync::{Arc, Weak};
use core::hash::Hash;

use hashbrown::{hash_map::Entry, HashMap};
use once_cell::sync::OnceCell;

use crate::lock::{rank, Mutex};
use crate::FastHashMap;

type SlotInner<V> = Weak<V>;
type ResourcePoolSlot<V> = Arc<OnceCell<SlotInner<V>>>;

pub struct ResourcePool<K, V> {
    inner: Mutex<FastHashMap<K, ResourcePoolSlot<V>>>,
}

impl<K: Clone + Eq + Hash, V> ResourcePool<K, V> {
    pub fn new() -> Self {
        Self {
            inner: Mutex::new(rank::RESOURCE_POOL_INNER, HashMap::default()),
        }
    }

    /// Get a resource from the pool with the given entry map, or create a new
    /// one if it doesn't exist using the given constructor.
    ///
    /// Behaves such that only one resource will be created for each unique
    /// entry map at any one time.
    pub fn get_or_init<F, E>(&self, key: K, constructor: F) -> Result<Arc<V>, E>
    where
        F: FnOnce(K) -> Result<Arc<V>, E>,
    {
        let mut key = Some(key);
        let mut constructor = Some(constructor);

        'race: loop {
            let mut map_guard = self.inner.lock();

            let entry = match map_guard.entry(key.clone().unwrap()) {
                Entry::Occupied(entry) => Arc::clone(entry.get()),
                Entry::Vacant(entry) => Arc::clone(entry.insert(Arc::new(OnceCell::new()))),
            };

            drop(map_guard);

            let mut strong = None;
            let weak = entry.get_or_try_init(|| {
                let strong_inner = constructor.take().unwrap()(key.take().unwrap())?;
                let weak = Arc::downgrade(&strong_inner);
                strong = Some(strong_inner);
                Ok(weak)
            })?;

            if let Some(strong) = strong {
                return Ok(strong);
            }

            if let Some(strong) = weak.upgrade() {
                return Ok(strong);
            }

            continue 'race;
        }
    }

    /// Remove the given entry map from the pool.
    ///
    /// Must *only* be called in the Drop impl of [`BindGroupLayout`].
    ///
    /// [`BindGroupLayout`]: crate::binding_model::BindGroupLayout
    pub fn remove(&self, key: &K) {
        let mut map_guard = self.inner.lock();

        map_guard.remove(key);
    }
}
