// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::client::MlsError;
use crate::key_package::KeyPackageRef;

use alloc::vec::Vec;
use mls_rs_codec::MlsEncode;
use mls_rs_core::{
    error::IntoAnyError,
    group::{GroupState, GroupStateStorage},
    key_package::KeyPackageStorage,
};

use super::snapshot::Snapshot;

#[derive(Debug, Clone)]
pub(crate) struct GroupStateRepository<S, K>
where
    S: GroupStateStorage,
    K: KeyPackageStorage,
{
    pending_key_package_removal: Option<KeyPackageRef>,
    storage: S,
    key_package_repo: K,
}

impl<S, K> GroupStateRepository<S, K>
where
    S: GroupStateStorage,
    K: KeyPackageStorage,
{
    pub fn new(
        storage: S,
        key_package_repo: K,
        key_package_to_remove: Option<KeyPackageRef>,
    ) -> Result<GroupStateRepository<S, K>, MlsError> {
        Ok(GroupStateRepository {
            storage,
            pending_key_package_removal: key_package_to_remove,
            key_package_repo,
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn write_to_storage(&mut self, group_snapshot: Snapshot) -> Result<(), MlsError> {
        let group_state = GroupState {
            data: group_snapshot.mls_encode_to_vec()?.into(),
            id: group_snapshot.state.context.group_id,
        };

        self.storage
            .write(group_state, Vec::new(), Vec::new())
            .await
            .map_err(|e| MlsError::GroupStorageError(e.into_any_error()))?;

        if let Some(ref key_package_ref) = self.pending_key_package_removal {
            self.key_package_repo
                .delete(key_package_ref)
                .await
                .map_err(|e| MlsError::KeyPackageRepoError(e.into_any_error()))?;
        }

        Ok(())
    }
}
