// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)
use alloc::{vec, vec::Vec};

use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::crypto::HpkeSecretKey;

use crate::{client::MlsError, crypto::CipherSuiteProvider};

use super::{
    math::leaf_lca_level,
    node::LeafIndex,
    path_secret::{PathSecret, PathSecretGenerator},
    TreeKemPublic,
};

#[derive(Clone, Debug, MlsEncode, MlsDecode, MlsSize, Eq, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[non_exhaustive]
pub struct TreeKemPrivate {
    pub self_index: LeafIndex,
    pub secret_keys: Vec<Option<HpkeSecretKey>>,
}

impl TreeKemPrivate {
    pub fn new_self_leaf(self_index: LeafIndex, leaf_secret: HpkeSecretKey) -> Self {
        TreeKemPrivate {
            self_index,
            secret_keys: vec![Some(leaf_secret)],
        }
    }

    pub fn new_for_external() -> Self {
        TreeKemPrivate {
            self_index: LeafIndex::unchecked(0),
            secret_keys: Default::default(),
        }
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn update_secrets<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
        signer_index: LeafIndex,
        path_secret: PathSecret,
        public_tree: &TreeKemPublic,
    ) -> Result<(), MlsError> {
        let lca_index = leaf_lca_level(self.self_index.into(), signer_index.into()) as usize - 2;


        let mut node_secret_gen =
            PathSecretGenerator::starting_with(cipher_suite_provider, path_secret);

        let path = public_tree.nodes.direct_copath(self.self_index);
        let filtered = &public_tree.nodes.filtered(self.self_index)?;
        self.secret_keys.resize(path.len() + 1, None);

        for (i, (n, f)) in path.iter().zip(filtered).enumerate().skip(lca_index) {
            if *f {
                continue;
            }

            let secret = node_secret_gen.next_secret().await?;

            let expected_pub_key = public_tree
                .nodes
                .borrow_node(n.path)?
                .as_ref()
                .map(|n| n.public_key())
                .ok_or(MlsError::PubKeyMismatch)?;

            let (secret_key, public_key) = secret.to_hpke_key_pair(cipher_suite_provider).await?;

            if expected_pub_key != &public_key {
                return Err(MlsError::PubKeyMismatch);
            }

            self.secret_keys[i + 1] = Some(secret_key);
        }

        Ok(())
    }

    #[cfg(feature = "by_ref_proposal")]
    pub fn update_leaf(&mut self, new_leaf: HpkeSecretKey) {
        self.secret_keys = vec![None; self.secret_keys.len()];
        self.secret_keys[0] = Some(new_leaf);
    }
}
