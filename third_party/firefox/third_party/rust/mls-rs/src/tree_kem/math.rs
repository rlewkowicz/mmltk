// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use core::{fmt::Debug, hash::Hash};
use mls_rs_codec::{MlsDecode, MlsEncode};

use super::node::LeafIndex;

pub trait TreeIndex:
    Send + Sync + Eq + Clone + Debug + Default + MlsEncode + MlsDecode + Hash + Ord
{
    fn root(&self) -> Self;

    fn left_unchecked(&self) -> Self;
    fn right_unchecked(&self) -> Self;

    fn parent_sibling(&self, leaf_count: &Self) -> Option<ParentSibling<Self>>;
    fn is_leaf(&self) -> bool;
    fn is_in_tree(&self, root: &Self) -> bool;

    #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
    fn zero() -> Self;

#[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
fn left(&self) -> Option<Self> {
        (!self.is_leaf()).then(|| self.left_unchecked())
    }

#[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
fn right(&self) -> Option<Self> {
        (!self.is_leaf()).then(|| self.right_unchecked())
    }

    fn direct_copath(&self, leaf_count: &Self) -> Vec<CopathNode<Self>> {
        let root = leaf_count.root();

        if !self.is_in_tree(&root) {
            return Vec::new();
        }

        let mut path = Vec::new();
        let mut parent = self.clone();

        while let Some(ps) = parent.parent_sibling(leaf_count) {
            path.push(CopathNode::new(ps.parent.clone(), ps.sibling));
            parent = ps.parent;
        }

        path
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct CopathNode<T> {
    pub path: T,
    pub copath: T,
}

impl<T: Clone + PartialEq + Eq + core::fmt::Debug> CopathNode<T> {
    pub fn new(path: T, copath: T) -> CopathNode<T> {
        CopathNode { path, copath }
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ParentSibling<T> {
    pub parent: T,
    pub sibling: T,
}

impl<T: Clone + PartialEq + Eq + core::fmt::Debug> ParentSibling<T> {
    pub fn new(parent: T, sibling: T) -> ParentSibling<T> {
        ParentSibling { parent, sibling }
    }
}

macro_rules! impl_tree_stdint {
    ($t:ty) => {
        impl TreeIndex for $t {
            fn root(&self) -> $t {
                *self - 1
            }

            /// Panicks if `x` is even in debug, overflows in release.
            fn left_unchecked(&self) -> Self {
                *self ^ (0x01 << (self.trailing_ones() - 1))
            }

            /// Panicks if `x` is even in debug, overflows in release.
            fn right_unchecked(&self) -> Self {
                *self ^ (0x03 << (self.trailing_ones() - 1))
            }

            fn parent_sibling(&self, leaf_count: &Self) -> Option<ParentSibling<Self>> {
                if self == &leaf_count.root() {
                    return None;
                }

                let lvl = self.trailing_ones();
                let p = (self & !(1 << (lvl + 1))) | (1 << lvl);

                let s = if *self < p {
                    p.right_unchecked()
                } else {
                    p.left_unchecked()
                };

                Some(ParentSibling::new(p, s))
            }

            fn is_leaf(&self) -> bool {
                self & 1 == 0
            }

            fn is_in_tree(&self, root: &Self) -> bool {
                *self <= 2 * root
            }

            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            fn zero() -> Self {
                0
            }
        }
    };
}

impl_tree_stdint!(u32);

#[cfg(any())]









impl_tree_stdint!(u64);

pub fn leaf_lca_level(x: u32, y: u32) -> u32 {
    let mut xn = x;
    let mut yn = y;
    let mut k = 0;

    while xn != yn {
        xn >>= 1;
        yn >>= 1;
        k += 1;
    }

    k
}

#[derive(Clone, Debug)]
pub(crate) struct SubTree {
    pub left: LeafIndex,
    pub right: LeafIndex,
}

pub(crate) struct SubTreeIter {
    current: LeafIndex,
    end: LeafIndex,
}

impl Iterator for SubTreeIter {
    type Item = LeafIndex;

    fn next(&mut self) -> Option<Self::Item> {
        if self.current < self.end {
            let result = self.current;
            self.current = self.current.next_unchecked();
            Some(result)
        } else {
            None
        }
    }
}

impl IntoIterator for SubTree {
    type Item = LeafIndex;
    type IntoIter = SubTreeIter;

    fn into_iter(self) -> Self::IntoIter {
        SubTreeIter {
            current: self.left,
            end: self.right,
        }
    }
}

pub fn subtree(x: u32) -> SubTree {
    let breadth = 1 << x.trailing_ones();

    let left = LeafIndex::from_node_index_unchecked(x + 1 - breadth);
    let right = LeafIndex::from_node_index_unchecked(x + breadth).next_unchecked();

    SubTree { left, right }
}

pub struct BfsIterTopDown {
    level: usize,
    mask: usize,
    level_end: usize,
    ctr: usize,
}

impl BfsIterTopDown {
    pub fn new(num_leaves: usize) -> Self {
        let depth = num_leaves.trailing_zeros() as usize;
        Self {
            level: depth + 1,
            mask: (1 << depth) - 1,
            level_end: 1,
            ctr: 0,
        }
    }
}

impl Iterator for BfsIterTopDown {
    type Item = usize;

    fn next(&mut self) -> Option<Self::Item> {
        if self.ctr == self.level_end {
            if self.level == 1 {
                return None;
            }
            self.level_end = (((self.level_end - 1) << 1) | 1) + 1;
            self.level -= 1;
            self.ctr = 0;
            self.mask >>= 1;
        }
        let res = Some((self.ctr << self.level) | self.mask);
        self.ctr += 1;
        res
    }
}
