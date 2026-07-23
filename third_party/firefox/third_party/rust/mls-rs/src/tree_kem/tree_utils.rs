// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::string::String;
use alloc::{format, vec};
use core::borrow::BorrowMut;

use debug_tree::TreeBuilder;

use super::node::{NodeIndex, NodeVec};
use crate::{client::MlsError, tree_kem::math::TreeIndex};

pub(crate) fn build_tree(
    tree: &mut TreeBuilder,
    nodes: &NodeVec,
    idx: NodeIndex,
) -> Result<(), MlsError> {
    let blank_tag = if nodes.is_blank(idx)? { "Blank " } else { "" };

    if nodes.is_leaf(idx) {
        let leaf_tag = format!("{blank_tag}Leaf ({idx})");
        tree.add_leaf(&leaf_tag);
        return Ok(());
    }

    let mut parent_tag = format!("{blank_tag}Parent ({idx})");

    if nodes.total_leaf_count().root() == idx {
        parent_tag = format!("{blank_tag}Root ({idx})");
    }

    let unmerged_leaves_idxs = match nodes.borrow_as_parent(idx) {
        Ok(parent) => parent
            .unmerged_leaves
            .iter()
            .map(|leaf_idx| format!("{}", **leaf_idx))
            .collect(),
        Err(_) => {
            vec![]
        }
    };

    if !unmerged_leaves_idxs.is_empty() {
        let unmerged_leaves_tag =
            format!(" unmerged leaves idxs: {}", unmerged_leaves_idxs.join(","));
        parent_tag.push_str(&unmerged_leaves_tag);
    }

    let mut branch = tree.add_branch(&parent_tag);

    build_tree(tree, nodes, idx.left_unchecked())?;
    build_tree(tree, nodes, idx.right_unchecked())?;

    branch.release();

    Ok(())
}

pub(crate) fn build_ascii_tree(nodes: &NodeVec) -> String {
    let leaves_count: u32 = nodes.total_leaf_count();
    let mut tree = TreeBuilder::new();
    build_tree(tree.borrow_mut(), nodes, leaves_count.root()).unwrap();
    tree.string()
}
