use alloc::sync::Arc;

use crate::{
    id::Id,
    identity::IdentityManager,
    lock::{
        rank::{self, LockRank},
        RwLock, RwLockReadGuard,
    },
    storage::{Element, Storage, StorageItem},
};

#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct RegistryReport {
    /// Count of IDs allocated by [`IdentityManager`]
    ///
    /// This may be inconsistent with other fields of the report if
    /// IDs are allocated or released concurrently with report
    /// generation.
    pub num_allocated: usize,
    pub num_kept_from_user: usize,
    pub num_released_from_user: usize,
    pub element_size: usize,
}

impl RegistryReport {
    pub fn is_empty(&self) -> bool {
        self.num_allocated + self.num_kept_from_user == 0
    }
}

/// Registry is the primary holder of each resource type
/// Every resource is now arcanized so the last arc released
/// will in the end free the memory and release the inner raw resource
///
/// Registry act as the main entry point to keep resource alive
/// when created and released from user land code
///
/// A resource may still be alive when released from user land code
/// if it's used in active submission or anyway kept alive from
/// any other dependent resource
///
#[derive(Debug)]
pub(crate) struct Registry<T: StorageItem> {
    identity: Arc<IdentityManager<T::Marker>>,
    storage: RwLock<Storage<T>>,
}

impl<T: StorageItem> Registry<T> {
    pub(crate) fn new() -> Self {
        Self::with_rank(rank::HUB_OTHER)
    }

    pub(crate) fn with_rank(rank: LockRank) -> Self {
        Self {
            identity: Arc::new(IdentityManager::new()),
            storage: RwLock::new(rank, Storage::new()),
        }
    }
}

#[must_use]
pub(crate) struct FutureId<'a, T: StorageItem> {
    id: Id<T::Marker>,
    data: &'a RwLock<Storage<T>>,
}

impl<T: StorageItem> FutureId<'_, T> {
    /// Assign a new resource to this ID.
    ///
    /// Registers it with the registry.
    pub fn assign(self, value: T) -> Id<T::Marker> {
        let mut data = self.data.write();
        data.insert(self.id, value);
        self.id
    }
}

impl<T: StorageItem> Registry<T> {
    pub(crate) fn prepare(&self, id_in: Option<Id<T::Marker>>) -> FutureId<'_, T> {
        FutureId {
            id: match id_in {
                Some(id_in) => {
                    self.identity.mark_as_used(id_in);
                    id_in
                }
                None => self.identity.process(),
            },
            data: &self.storage,
        }
    }

    #[track_caller]
    pub(crate) fn read<'a>(&'a self) -> RwLockReadGuard<'a, Storage<T>> {
        self.storage.read()
    }
    pub(crate) fn remove(&self, id: Id<T::Marker>) -> T {
        let value = self.storage.write().remove(id);
        self.identity.free(id);
        value
    }

    pub(crate) fn generate_report(&self) -> RegistryReport {
        let mut report = RegistryReport {
            element_size: size_of::<T>(),
            ..Default::default()
        };

        report.num_allocated = self.identity.values.lock().count();

        let storage = self.storage.read();
        for element in storage.map.iter() {
            match *element {
                Element::Occupied(..) => report.num_kept_from_user += 1,
                Element::Vacant => report.num_released_from_user += 1,
            }
        }
        report
    }
}

impl<T: StorageItem + Clone> Registry<T> {
    pub(crate) fn get(&self, id: Id<T::Marker>) -> T {
        self.read().get(id)
    }
}
