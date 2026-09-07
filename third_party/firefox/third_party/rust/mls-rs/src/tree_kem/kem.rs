// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::client::MlsError;
use crate::crypto::{CipherSuiteProvider, SignatureSecretKey};
use crate::group::GroupContext;
use crate::identity::SigningIdentity;
use crate::iter::wrap_iter;
use crate::tree_kem::math as tree_math;
use alloc::vec;
use alloc::vec::Vec;
use itertools::Itertools;
use mls_rs_codec::MlsEncode;
use tree_math::{CopathNode, TreeIndex};

#[cfg(all(not(mls_build_async), feature = "rayon"))]
use {crate::iter::ParallelIteratorExt, rayon::prelude::*};

#[cfg(mls_build_async)]
use futures::{StreamExt, TryStreamExt};

#[cfg(feature = "std")]
use std::collections::HashSet;

use super::hpke_encryption::HpkeEncryptable;
use super::leaf_node::ConfigProperties;
use super::node::NodeTypeResolver;
use super::{
    node::{LeafIndex, NodeIndex},
    path_secret::{PathSecret, PathSecretGenerator},
    TreeKemPrivate, TreeKemPublic, UpdatePath, UpdatePathNode, ValidatedUpdatePath,
};


pub struct TreeKem<'a> {
    tree_kem_public: &'a mut TreeKemPublic,
    private_key: &'a mut TreeKemPrivate,
}

pub struct EncapGeneration {
    pub update_path: UpdatePath,
    pub path_secrets: Vec<Option<PathSecret>>,
    pub commit_secret: PathSecret,
}

impl<'a> TreeKem<'a> {
    pub fn new(
        tree_kem_public: &'a mut TreeKemPublic,
        private_key: &'a mut TreeKemPrivate,
    ) -> Self {
        TreeKem {
            tree_kem_public,
            private_key,
        }
    }

    #[allow(clippy::too_many_arguments)]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn encap<P>(
        self,
        context: &mut GroupContext,
        excluding: &[LeafIndex],
        signer: &SignatureSecretKey,
        update_leaf_properties: Option<ConfigProperties>,
        signing_identity: Option<SigningIdentity>,
        cipher_suite_provider: &P,
#[cfg(any())]








 commit_modifiers: &CommitModifiers,
    ) -> Result<EncapGeneration, MlsError>
    where
        P: CipherSuiteProvider + Send + Sync,
    {
        let self_index = self.private_key.self_index;
        let path = self.tree_kem_public.nodes.direct_copath(self_index);
        let filtered = self.tree_kem_public.nodes.filtered(self_index)?;

        self.private_key.secret_keys.resize(path.len() + 1, None);

        let mut secret_generator = PathSecretGenerator::new(cipher_suite_provider);
        let mut path_secrets = vec![];

        for (i, (node, f)) in path.iter().zip(&filtered).enumerate() {
            if !f {
                let secret = secret_generator.next_secret().await?;

                let (secret_key, public_key) =
                    secret.to_hpke_key_pair(cipher_suite_provider).await?;

                self.private_key.secret_keys[i + 1] = Some(secret_key);
                self.tree_kem_public.update_node(public_key, node.path)?;
                path_secrets.push(Some(secret));
            } else {
                self.private_key.secret_keys[i + 1] = None;
                path_secrets.push(None);
            }
        }

#[cfg(any())]









        (commit_modifiers.modify_tree)(self.tree_kem_public);

        self.tree_kem_public
            .update_parent_hashes(self_index, false, cipher_suite_provider)
            .await?;

        let update_path_leaf = {
            let own_leaf = self.tree_kem_public.nodes.borrow_as_leaf_mut(self_index)?;

            self.private_key.secret_keys[0] = Some(
                own_leaf
                    .commit(
                        cipher_suite_provider,
                        &context.group_id,
                        *self_index,
                        update_leaf_properties,
                        signing_identity,
                        signer,
                    )
                    .await?,
            );

#[cfg(any())]









            if let Some(signer) = (commit_modifiers.modify_leaf)(own_leaf, signer) {
                let context = &(context.group_id.as_slice(), *self_index).into();

                own_leaf
                    .sign(cipher_suite_provider, &signer, context)
                    .await
                    .unwrap();
            }

            own_leaf.clone()
        };

        self.tree_kem_public
            .update_hashes(&[self_index], cipher_suite_provider)
            .await?;

        context.tree_hash = self
            .tree_kem_public
            .tree_hash(cipher_suite_provider)
            .await?;

        let context_bytes = context.mls_encode_to_vec()?;

        let node_updates = self
            .encrypt_path_secrets(
                path,
                &path_secrets,
                &context_bytes,
                cipher_suite_provider,
                excluding,
            )
            .await?;

#[cfg(any())]









        let node_updates = (commit_modifiers.modify_path)(node_updates);

        let update_path = UpdatePath {
            leaf_node: update_path_leaf,
            nodes: node_updates,
        };

        Ok(EncapGeneration {
            update_path,
            path_secrets,
            commit_secret: secret_generator.next_secret().await?,
        })
    }

    #[cfg(any(mls_build_async, not(feature = "rayon")))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn encrypt_path_secrets<P: CipherSuiteProvider>(
        &self,
        path: Vec<CopathNode<NodeIndex>>,
        path_secrets: &[Option<PathSecret>],
        context_bytes: &[u8],
        cipher_suite: &P,
        excluding: &[LeafIndex],
    ) -> Result<Vec<UpdatePathNode>, MlsError> {
        let excluding = excluding.iter().copied().map(NodeIndex::from);

        #[cfg(feature = "std")]
        let excluding = excluding.collect::<HashSet<NodeIndex>>();
        #[cfg(not(feature = "std"))]
        let excluding = excluding.collect::<Vec<NodeIndex>>();

        let mut node_updates = Vec::new();

        for (index, path_secret) in path.into_iter().zip(path_secrets.iter()) {
            if let Some(path_secret) = path_secret {
                node_updates.push(
                    self.encrypt_copath_node_resolution(
                        cipher_suite,
                        path_secret,
                        index.copath,
                        context_bytes,
                        &excluding,
                    )
                    .await?,
                );
            }
        }

        Ok(node_updates)
    }

    #[cfg(all(not(mls_build_async), feature = "rayon"))]
    fn encrypt_path_secrets<P: CipherSuiteProvider>(
        &self,
        path: Vec<CopathNode<NodeIndex>>,
        path_secrets: &[Option<PathSecret>],
        context_bytes: &[u8],
        cipher_suite: &P,
        excluding: &[LeafIndex],
    ) -> Result<Vec<UpdatePathNode>, MlsError> {
        let excluding = excluding.iter().copied().map(NodeIndex::from);

        #[cfg(feature = "std")]
        let excluding = excluding.collect::<HashSet<NodeIndex>>();
        #[cfg(not(feature = "std"))]
        let excluding = excluding.collect::<Vec<NodeIndex>>();

        path.into_par_iter()
            .zip(path_secrets.par_iter())
            .filter_map(|(node, path_secret)| {
                path_secret.as_ref().map(|path_secret| {
                    self.encrypt_copath_node_resolution(
                        cipher_suite,
                        path_secret,
                        node.copath,
                        context_bytes,
                        &excluding,
                    )
                })
            })
            .collect()
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn decap<CP>(
        self,
        sender_index: LeafIndex,
        update_path: &ValidatedUpdatePath,
        added_leaves: &[LeafIndex],
        context_bytes: &[u8],
        cipher_suite_provider: &CP,
    ) -> Result<PathSecret, MlsError>
    where
        CP: CipherSuiteProvider,
    {
        let self_index = self.private_key.self_index;

        let lca_index =
            tree_math::leaf_lca_level(self_index.into(), sender_index.into()) as usize - 2;

        let mut path = self.tree_kem_public.nodes.direct_copath(self_index);
        let leaf = CopathNode::new(self_index.into(), 0);
        path.insert(0, leaf);
        let resolved_pos = self.find_resolved_pos(&path, lca_index)?;

        let ct_pos =
            self.find_ciphertext_pos(path[lca_index].path, path[resolved_pos].path, added_leaves)?;

        let lca_node = update_path.nodes[lca_index]
            .as_ref()
            .ok_or(MlsError::LcaNotFoundInDirectPath)?;

        let ct = lca_node
            .encrypted_path_secret
            .get(ct_pos)
            .ok_or(MlsError::LcaNotFoundInDirectPath)?;

        let secret = self.private_key.secret_keys[resolved_pos]
            .as_ref()
            .ok_or(MlsError::UpdateErrorNoSecretKey)?;

        let public = self
            .tree_kem_public
            .nodes
            .borrow_node(path[resolved_pos].path)?
            .as_ref()
            .ok_or(MlsError::UpdateErrorNoSecretKey)?
            .public_key();

        let lca_path_secret =
            PathSecret::decrypt(cipher_suite_provider, secret, public, context_bytes, ct).await?;

        let mut node_secret_gen =
            PathSecretGenerator::starting_with(cipher_suite_provider, lca_path_secret);

        self.private_key.secret_keys.resize(path.len() + 1, None);

        for (i, update) in update_path.nodes.iter().enumerate().skip(lca_index) {
            if let Some(update) = update {
                let secret = node_secret_gen.next_secret().await?;

                let (hpke_private, hpke_public) =
                    secret.to_hpke_key_pair(cipher_suite_provider).await?;

                if hpke_public != update.public_key {
                    return Err(MlsError::PubKeyMismatch);
                }

                self.private_key.secret_keys[i + 1] = Some(hpke_private);
            } else {
                self.private_key.secret_keys[i + 1] = None;
            }
        }

        node_secret_gen.next_secret().await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn encrypt_copath_node_resolution<P: CipherSuiteProvider>(
        &self,
        cipher_suite_provider: &P,
        path_secret: &PathSecret,
        copath_index: NodeIndex,
        context: &[u8],
        #[cfg(feature = "std")] excluding: &HashSet<NodeIndex>,
        #[cfg(not(feature = "std"))] excluding: &[NodeIndex],
    ) -> Result<UpdatePathNode, MlsError> {
        let reso = self
            .tree_kem_public
            .nodes
            .get_resolution_index(copath_index)?;

        let make_ctxt = |idx| async move {
            let node = self
                .tree_kem_public
                .nodes
                .borrow_node(idx)?
                .as_non_empty()?;

            path_secret
                .encrypt(cipher_suite_provider, node.public_key(), context)
                .await
        };

        let ctxts = wrap_iter(reso).filter(|&idx| async move { !excluding.contains(&idx) });

        #[cfg(not(mls_build_async))]
        let ctxts = ctxts.map(make_ctxt);

        #[cfg(mls_build_async)]
        let ctxts = ctxts.then(make_ctxt);

        let ctxts = ctxts.try_collect().await?;

        let path_index = copath_index
            .parent_sibling(&self.tree_kem_public.total_leaf_count())
            .ok_or(MlsError::ExpectedNode)?
            .parent;

        Ok(UpdatePathNode {
            public_key: self
                .tree_kem_public
                .nodes
                .borrow_as_parent(path_index)?
                .public_key
                .clone(),
            encrypted_path_secret: ctxts,
        })
    }

    #[inline]
    fn find_resolved_pos(
        &self,
        path: &[CopathNode<NodeIndex>],
        mut lca_index: usize,
    ) -> Result<usize, MlsError> {
        while self.tree_kem_public.nodes.is_blank(path[lca_index].path)? {
            lca_index -= 1;
        }

        if self.private_key.secret_keys[lca_index].is_none() {
            lca_index = 0;
        }

        Ok(lca_index)
    }

    #[inline]
    fn find_ciphertext_pos(
        &self,
        lca: NodeIndex,
        resolved: NodeIndex,
        excluding: &[LeafIndex],
    ) -> Result<usize, MlsError> {
        let reso = self.tree_kem_public.nodes.get_resolution_index(lca)?;

        let (ct_pos, _) = reso
            .iter()
            .filter(|idx| {
                **idx % 2 == 1 || !excluding.contains(&LeafIndex::from_node_index_unchecked(**idx))
            })
            .find_position(|idx| idx == &&resolved)
            .ok_or(MlsError::UpdateErrorNoSecretKey)?;

        Ok(ct_pos)
    }
}
