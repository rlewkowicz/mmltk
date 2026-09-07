// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::client::MlsError;
use crate::{group::PriorEpoch, key_package::KeyPackageRef};

use alloc::collections::VecDeque;
use alloc::vec::Vec;
use core::fmt::{self, Debug};
use mls_rs_codec::{MlsDecode, MlsEncode};
use mls_rs_core::group::{EpochRecord, GroupState};
use mls_rs_core::{error::IntoAnyError, group::GroupStateStorage, key_package::KeyPackageStorage};

use super::snapshot::Snapshot;

#[cfg(feature = "psk")]
use crate::group::ResumptionPsk;

#[cfg(feature = "psk")]
use mls_rs_core::psk::PreSharedKey;

/// A set of changes to apply to a GroupStateStorage implementation. These changes MUST
/// be made in a single transaction to avoid creating invalid states.
#[derive(Default, Clone, Debug)]
struct EpochStorageCommit {
    pub(crate) inserts: VecDeque<PriorEpoch>,
    pub(crate) updates: Vec<PriorEpoch>,
}

#[derive(Clone)]
pub(crate) struct GroupStateRepository<S, K>
where
    S: GroupStateStorage,
    K: KeyPackageStorage,
{
    pending_commit: EpochStorageCommit,
    pending_key_package_removal: Option<KeyPackageRef>,
    group_id: Vec<u8>,
    storage: S,
    key_package_repo: K,
}

impl<S, K> Debug for GroupStateRepository<S, K>
where
    S: GroupStateStorage + Debug,
    K: KeyPackageStorage + Debug,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("GroupStateRepository")
            .field("pending_commit", &self.pending_commit)
            .field(
                "pending_key_package_removal",
                &self.pending_key_package_removal,
            )
            .field(
                "group_id",
                &mls_rs_core::debug::pretty_group_id(&self.group_id),
            )
            .field("storage", &self.storage)
            .field("key_package_repo", &self.key_package_repo)
            .finish()
    }
}

impl<S, K> GroupStateRepository<S, K>
where
    S: GroupStateStorage,
    K: KeyPackageStorage,
{
    pub fn new(
        group_id: Vec<u8>,
        storage: S,
        key_package_repo: K,
        key_package_to_remove: Option<KeyPackageRef>,
    ) -> Result<GroupStateRepository<S, K>, MlsError> {
        Ok(GroupStateRepository {
            group_id,
            storage,
            pending_key_package_removal: key_package_to_remove,
            pending_commit: Default::default(),
            key_package_repo,
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn find_max_id(&self) -> Result<Option<u64>, MlsError> {
        if let Some(max) = self.pending_commit.inserts.back().map(|e| e.epoch_id()) {
            Ok(Some(max))
        } else {
            self.storage
                .max_epoch_id(&self.group_id)
                .await
                .map_err(|e| MlsError::GroupStorageError(e.into_any_error()))
        }
    }

    #[cfg(feature = "psk")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn resumption_secret(
        &self,
        psk_id: &ResumptionPsk,
    ) -> Result<Option<PreSharedKey>, MlsError> {
        if let Some(min) = self.pending_commit.inserts.front().map(|e| e.epoch_id()) {
            if psk_id.psk_epoch >= min {
                return Ok(self
                    .pending_commit
                    .inserts
                    .get((psk_id.psk_epoch - min) as usize)
                    .map(|e| e.secrets.resumption_secret.clone()));
            }
        }

        let maybe_pending = self.find_pending(psk_id.psk_epoch);

        if let Some(pending) = maybe_pending {
            return Ok(Some(
                self.pending_commit.updates[pending]
                    .secrets
                    .resumption_secret
                    .clone(),
            ));
        }

        self.storage
            .epoch(&psk_id.psk_group_id.0, psk_id.psk_epoch)
            .await
            .map_err(|e| MlsError::GroupStorageError(e.into_any_error()))?
            .map(|e| Ok(PriorEpoch::mls_decode(&mut &**e)?.secrets.resumption_secret))
            .transpose()
    }

    #[cfg(feature = "private_message")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn get_epoch_mut(
        &mut self,
        epoch_id: u64,
    ) -> Result<Option<&mut PriorEpoch>, MlsError> {
        if let Some(min) = self.pending_commit.inserts.front().map(|e| e.epoch_id()) {
            if epoch_id >= min {
                return Ok(self
                    .pending_commit
                    .inserts
                    .get_mut((epoch_id - min) as usize));
            }
        }

        match self.find_pending(epoch_id) {
            Some(i) => self.pending_commit.updates.get_mut(i).map(Ok),
            None => self
                .storage
                .epoch(&self.group_id, epoch_id)
                .await
                .map_err(|e| MlsError::GroupStorageError(e.into_any_error()))?
                .and_then(|epoch| {
                    PriorEpoch::mls_decode(&mut &**epoch)
                        .map(|epoch| {
                            self.pending_commit.updates.push(epoch);
                            self.pending_commit.updates.last_mut()
                        })
                        .transpose()
                }),
        }
        .transpose()
        .map_err(Into::into)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn insert(&mut self, epoch: PriorEpoch) -> Result<(), MlsError> {
        if epoch.group_id() != self.group_id {
            return Err(MlsError::GroupIdMismatch);
        }

        let epoch_id = epoch.epoch_id();

        if let Some(expected_id) = self.find_max_id().await?.map(|id| id + 1) {
            if epoch_id != expected_id {
                return Err(MlsError::InvalidEpoch);
            }
        }

        self.pending_commit.inserts.push_back(epoch);

        Ok(())
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn write_to_storage(&mut self, group_snapshot: Snapshot) -> Result<(), MlsError> {
        let inserts = self
            .pending_commit
            .inserts
            .iter()
            .map(|e| {
                Ok(EpochRecord::new(
                    e.epoch_id(),
                    e.mls_encode_to_vec()?.into(),
                ))
            })
            .collect::<Result<_, MlsError>>()?;

        let updates = self
            .pending_commit
            .updates
            .iter()
            .map(|e| {
                Ok(EpochRecord::new(
                    e.epoch_id(),
                    e.mls_encode_to_vec()?.into(),
                ))
            })
            .collect::<Result<_, MlsError>>()?;

        let group_state = GroupState {
            data: group_snapshot.mls_encode_to_vec()?.into(),
            id: group_snapshot.state.context.group_id,
        };

        self.storage
            .write(group_state, inserts, updates)
            .await
            .map_err(|e| MlsError::GroupStorageError(e.into_any_error()))?;

        if let Some(ref key_package_ref) = self.pending_key_package_removal {
            self.key_package_repo
                .delete(key_package_ref)
                .await
                .map_err(|e| MlsError::KeyPackageRepoError(e.into_any_error()))?;
        }

        self.pending_commit.inserts.clear();
        self.pending_commit.updates.clear();

        Ok(())
    }

    #[cfg(any(feature = "psk", feature = "private_message"))]
    fn find_pending(&self, epoch_id: u64) -> Option<usize> {
        self.pending_commit
            .updates
            .iter()
            .position(|ep| ep.context.epoch == epoch_id)
    }
}
