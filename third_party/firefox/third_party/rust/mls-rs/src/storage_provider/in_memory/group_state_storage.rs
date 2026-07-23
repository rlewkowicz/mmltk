// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::collections::VecDeque;

#[cfg(target_has_atomic = "ptr")]
use alloc::sync::Arc;

#[cfg(mls_build_async)]
use alloc::boxed::Box;
use alloc::vec::Vec;
use core::{
    convert::Infallible,
    fmt::{self, Debug},
};
use mls_rs_core::group::{EpochRecord, GroupState, GroupStateStorage};
#[cfg(not(target_has_atomic = "ptr"))]
use portable_atomic_util::Arc;
use zeroize::Zeroizing;

use crate::{
    client::MlsError,
    map::{LargeMap, LargeMapEntry},
};

#[cfg(feature = "std")]
use std::sync::{Mutex, MutexGuard};

#[cfg(not(feature = "std"))]
use spin::{Mutex, MutexGuard};

pub(crate) const DEFAULT_EPOCH_RETENTION_LIMIT: usize = 3;

#[derive(Clone)]
pub(crate) struct InMemoryGroupData {
    pub(crate) state_data: Zeroizing<Vec<u8>>,
    pub(crate) epoch_data: VecDeque<EpochRecord>,
}

impl Debug for InMemoryGroupData {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("InMemoryGroupData")
            .field(
                "state_data",
                &mls_rs_core::debug::pretty_bytes(&self.state_data),
            )
            .field("epoch_data", &self.epoch_data)
            .finish()
    }
}

impl InMemoryGroupData {
    pub fn new(state_data: Zeroizing<Vec<u8>>) -> InMemoryGroupData {
        InMemoryGroupData {
            state_data,
            epoch_data: Default::default(),
        }
    }

    fn get_epoch_data_index(&self, epoch_id: u64) -> Option<u64> {
        self.epoch_data
            .front()
            .and_then(|e| epoch_id.checked_sub(e.id))
    }

    pub fn get_epoch(&self, epoch_id: u64) -> Option<&EpochRecord> {
        self.get_epoch_data_index(epoch_id)
            .and_then(|i| self.epoch_data.get(i as usize))
    }

    pub fn get_mut_epoch(&mut self, epoch_id: u64) -> Option<&mut EpochRecord> {
        self.get_epoch_data_index(epoch_id)
            .and_then(|i| self.epoch_data.get_mut(i as usize))
    }

    pub fn insert_epoch(&mut self, epoch: EpochRecord) {
        self.epoch_data.push_back(epoch)
    }

    pub fn update_epoch(&mut self, epoch: EpochRecord) {
        if let Some(existing_epoch) = self.get_mut_epoch(epoch.id) {
            *existing_epoch = epoch
        }
    }

    pub fn trim_epochs(&mut self, max_epoch_retention: usize) {
        while self.epoch_data.len() > max_epoch_retention {
            self.epoch_data.pop_front();
        }
    }
}

#[derive(Clone)]
/// In memory group state storage backed by a HashMap.
///
/// All clones of an instance of this type share the same underlying HashMap.
pub struct InMemoryGroupStateStorage {
    pub(crate) inner: Arc<Mutex<LargeMap<Vec<u8>, InMemoryGroupData>>>,
    pub(crate) max_epoch_retention: usize,
}

impl Debug for InMemoryGroupStateStorage {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("InMemoryGroupStateStorage")
            .field(
                "inner",
                &mls_rs_core::debug::pretty_with(|f| {
                    f.debug_map()
                        .entries(
                            self.lock()
                                .iter()
                                .map(|(k, v)| (mls_rs_core::debug::pretty_bytes(k), v)),
                        )
                        .finish()
                }),
            )
            .field("max_epoch_retention", &self.max_epoch_retention)
            .finish()
    }
}

impl InMemoryGroupStateStorage {
    /// Create an empty group state storage.
    pub fn new() -> Self {
        Self {
            inner: Default::default(),
            max_epoch_retention: DEFAULT_EPOCH_RETENTION_LIMIT,
        }
    }

    pub fn with_max_epoch_retention(self, max_epoch_retention: usize) -> Result<Self, MlsError> {
        (max_epoch_retention > 0)
            .then_some(())
            .ok_or(MlsError::NonZeroRetentionRequired)?;

        Ok(Self {
            inner: self.inner,
            max_epoch_retention,
        })
    }

    /// Get the set of unique group ids that have data stored.
    pub fn stored_groups(&self) -> Vec<Vec<u8>> {
        self.lock().keys().cloned().collect()
    }

    /// Delete all data corresponding to `group_id`.
    pub fn delete_group(&self, group_id: &[u8]) {
        self.lock().remove(group_id);
    }

    fn lock(&self) -> MutexGuard<'_, LargeMap<Vec<u8>, InMemoryGroupData>> {
        #[cfg(feature = "std")]
        return self.inner.lock().unwrap();

        #[cfg(not(feature = "std"))]
        return self.inner.lock();
    }
}

impl Default for InMemoryGroupStateStorage {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl GroupStateStorage for InMemoryGroupStateStorage {
    type Error = Infallible;

    async fn max_epoch_id(&self, group_id: &[u8]) -> Result<Option<u64>, Self::Error> {
        Ok(self
            .lock()
            .get(group_id)
            .and_then(|group_data| group_data.epoch_data.back().map(|e| e.id)))
    }

    async fn state(&self, group_id: &[u8]) -> Result<Option<Zeroizing<Vec<u8>>>, Self::Error> {
        Ok(self
            .lock()
            .get(group_id)
            .map(|data| data.state_data.clone()))
    }

    async fn epoch(
        &self,
        group_id: &[u8],
        epoch_id: u64,
    ) -> Result<Option<Zeroizing<Vec<u8>>>, Self::Error> {
        Ok(self
            .lock()
            .get(group_id)
            .and_then(|data| data.get_epoch(epoch_id).map(|ep| ep.data.clone())))
    }

    async fn write(
        &mut self,
        state: GroupState,
        epoch_inserts: Vec<EpochRecord>,
        epoch_updates: Vec<EpochRecord>,
    ) -> Result<(), Self::Error> {
        let mut group_map = self.lock();

        let group_data = match group_map.entry(state.id) {
            LargeMapEntry::Occupied(entry) => {
                let data = entry.into_mut();
                data.state_data = state.data;
                data
            }
            LargeMapEntry::Vacant(entry) => entry.insert(InMemoryGroupData::new(state.data)),
        };

        epoch_inserts
            .into_iter()
            .for_each(|e| group_data.insert_epoch(e));

        epoch_updates
            .into_iter()
            .for_each(|e| group_data.update_epoch(e));

        group_data.trim_epochs(self.max_epoch_retention);

        Ok(())
    }
}
