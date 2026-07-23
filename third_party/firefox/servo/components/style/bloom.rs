/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! The style bloom filter is used as an optimization when matching deep
//! descendant selectors.

#![deny(missing_docs)]

use crate::dom::{SendElement, TElement};
use crate::LocalName;
use atomic_refcell::{AtomicRefCell, AtomicRefMut};
use selectors::bloom::BloomFilter;
use smallvec::SmallVec;

thread_local! {
    /// Bloom filters are large allocations, so we store them in thread-local storage
    /// such that they can be reused across style traversals. StyleBloom is responsible
    /// for ensuring that the bloom filter is zeroed when it is dropped.
    ///
    /// We intentionally leak this from TLS because we don't have the guarantee
    /// of TLS destructors to run in worker threads.
    ///
    /// Also, leaking it guarantees that we can borrow it indefinitely.
    ///
    /// We could change this once https://github.com/rayon-rs/rayon/issues/688
    /// is fixed, hopefully, which point we'd need to change the filter member below to be an
    /// arc and carry an owning reference around or so.
    static BLOOM_KEY: &'static AtomicRefCell<BloomFilter> = Box::leak(Default::default());
}

/// A struct that allows us to fast-reject deep descendant selectors avoiding
/// selector-matching.
///
/// This is implemented using a counting bloom filter, and it's a standard
/// optimization. See Gecko's `AncestorFilter`, and Blink's and WebKit's
/// `SelectorFilter`.
///
/// The constraints for Servo's style system are a bit different compared to
/// traditional style systems given Servo does a parallel breadth-first
/// traversal instead of a sequential depth-first traversal.
///
/// This implies that we need to track a bit more state than other browsers to
/// ensure we're doing the correct thing during the traversal, and being able to
/// apply this optimization effectively.
///
/// Concretely, we have a bloom filter instance per worker thread, and we track
/// the current DOM depth in order to find a common ancestor when it doesn't
/// match the previous element we've styled.
///
/// This is usually a pretty fast operation (we use to be one level deeper than
/// the previous one), but in the case of work-stealing, we may needed to push
/// and pop multiple elements.
///
/// See the `insert_parents_recovering`, where most of the magic happens.
///
/// Regarding thread-safety, this struct is safe because:
///
///  * We clear this after a restyle.
///  * The DOM shape and attributes (and every other thing we access here) are
///    immutable during a restyle.
///
pub struct StyleBloom<E: TElement> {
    /// A handle to the bloom filter from the thread upon which this StyleBloom
    /// was created. We use AtomicRefCell so that this is all |Send|, which allows
    /// StyleBloom to live in ThreadLocalStyleContext, which is dropped from the
    /// parent thread.
    filter: AtomicRefMut<'static, BloomFilter>,

    /// The stack of elements that this bloom filter contains, along with the
    /// number of hashes pushed for each element.
    elements: SmallVec<[PushedElement<E>; 16]>,

    /// Stack of hashes that have been pushed onto this filter.
    pushed_hashes: SmallVec<[u32; 64]>,
}

/// The very rough benchmarks in the selectors crate show clear()
/// costing about 25 times more than remove_hash(). We use this to implement
/// clear() more efficiently when only a small number of hashes have been
/// pushed.
///
/// One subtly to note is that remove_hash() will not touch the value
/// if the filter overflowed. However, overflow can only occur if we
/// get 255 collisions on the same hash value, and 25 < 255.
const MEMSET_CLEAR_THRESHOLD: usize = 25;

struct PushedElement<E: TElement> {
    /// The element that was pushed.
    element: SendElement<E>,

    /// The number of hashes pushed for the element.
    num_hashes: usize,
}

impl<E: TElement> PushedElement<E> {
    fn new(el: E, num_hashes: usize) -> Self {
        PushedElement {
            element: unsafe { SendElement::new(el) },
            num_hashes,
        }
    }
}

/// Returns whether the attribute name is excluded from the bloom filter.
///
/// We do this for attributes that are very common but not commonly used in
/// selectors.
#[inline]
pub fn is_attr_name_excluded_from_filter(name: &LocalName) -> bool {
    *name == local_name!("class") || *name == local_name!("id") || *name == local_name!("style")
}

/// Gather all relevant hash for fast-reject filters from an element.
pub fn each_relevant_element_hash<E, F>(element: E, mut f: F)
where
    E: TElement,
    F: FnMut(u32),
{
    f(element.local_name().get_hash());
    f(element.namespace().get_hash());

    if let Some(id) = element.id() {
        f(id.get_hash());
    }

    element.each_class(|class| f(class.get_hash()));

    element.each_attr_name(|name| {
        if !is_attr_name_excluded_from_filter(name) {
            f(name.get_hash())
        }
    });
}

impl<E: TElement> Drop for StyleBloom<E> {
    fn drop(&mut self) {
        self.clear();
    }
}

impl<E: TElement> StyleBloom<E> {
    /// Create an empty `StyleBloom`. Because StyleBloom acquires the thread-
    /// local filter buffer, creating multiple live StyleBloom instances at
    /// the same time on the same thread will panic.

    #[inline(never)]
    pub fn new() -> Self {
        let filter = BLOOM_KEY.with(|b| b.borrow_mut());
        debug_assert!(
            filter.is_zeroed(),
            "Forgot to zero the bloom filter last time"
        );
        StyleBloom {
            filter,
            elements: Default::default(),
            pushed_hashes: Default::default(),
        }
    }

    /// Return the bloom filter used properly by the `selectors` crate.
    pub fn filter(&self) -> &BloomFilter {
        &*self.filter
    }

    /// Push an element to the bloom filter, knowing that it's a child of the
    /// last element parent.
    pub fn push(&mut self, element: E) {
        if cfg!(debug_assertions) {
            if self.elements.is_empty() {
                assert!(element.traversal_parent().is_none());
            }
        }
        self.push_internal(element);
    }

    /// Same as `push`, but without asserting, in order to use it from
    /// `rebuild`.
    fn push_internal(&mut self, element: E) {
        let mut count = 0;
        each_relevant_element_hash(element, |hash| {
            count += 1;
            self.filter.insert_hash(hash);
            self.pushed_hashes.push(hash);
        });
        self.elements.push(PushedElement::new(element, count));
    }

    /// Pop the last element in the bloom filter and return it.
    #[inline]
    fn pop(&mut self) -> Option<E> {
        let PushedElement {
            element,
            num_hashes,
        } = self.elements.pop()?;
        let popped_element = *element;

        let mut expected_hashes = vec![];
        if cfg!(debug_assertions) {
            each_relevant_element_hash(popped_element, |hash| expected_hashes.push(hash));
        }

        for _ in 0..num_hashes {
            let hash = self.pushed_hashes.pop().unwrap();
            debug_assert_eq!(expected_hashes.pop().unwrap(), hash);
            self.filter.remove_hash(hash);
        }

        Some(popped_element)
    }

    /// Returns the DOM depth of elements that can be correctly
    /// matched against the bloom filter (that is, the number of
    /// elements in our list).
    pub fn matching_depth(&self) -> usize {
        self.elements.len()
    }

    /// Clears the bloom filter.
    pub fn clear(&mut self) {
        self.elements.clear();

        if self.pushed_hashes.len() > MEMSET_CLEAR_THRESHOLD {
            self.filter.clear();
            self.pushed_hashes.clear();
        } else {
            for hash in self.pushed_hashes.drain(..) {
                self.filter.remove_hash(hash);
            }
            debug_assert!(self.filter.is_zeroed());
        }
    }

    /// Rebuilds the bloom filter up to the parent of the given element.
    pub fn rebuild(&mut self, mut element: E) {
        self.clear();

        let mut parents_to_insert = SmallVec::<[E; 16]>::new();
        while let Some(parent) = element.traversal_parent() {
            parents_to_insert.push(parent);
            element = parent;
        }

        for parent in parents_to_insert.drain(..).rev() {
            self.push(parent);
        }
    }

    /// In debug builds, asserts that all the parents of `element` are in the
    /// bloom filter.
    ///
    /// Goes away in release builds.
    pub fn assert_complete(&self, mut element: E) {
        if cfg!(debug_assertions) {
            let mut checked = 0;
            while let Some(parent) = element.traversal_parent() {
                assert_eq!(
                    parent,
                    *(self.elements[self.elements.len() - 1 - checked].element)
                );
                element = parent;
                checked += 1;
            }
            assert_eq!(checked, self.elements.len());
        }
    }

    /// Get the element that represents the chain of things inserted
    /// into the filter right now.  That chain is the given element
    /// (if any) and its ancestors.
    #[inline]
    pub fn current_parent(&self) -> Option<E> {
        self.elements.last().map(|ref el| *el.element)
    }

    /// Insert the parents of an element in the bloom filter, trying to recover
    /// the filter if the last element inserted doesn't match.
    ///
    /// Gets the element depth in the dom, to make it efficient, or if not
    /// provided always rebuilds the filter from scratch.
    ///
    /// Returns the new bloom filter depth, that the traversal code is
    /// responsible to keep around if it wants to get an effective filter.
    pub fn insert_parents_recovering(&mut self, element: E, element_depth: usize) {
        if self.elements.is_empty() {
            self.rebuild(element);
            return;
        }

        let traversal_parent = match element.traversal_parent() {
            Some(parent) => parent,
            None => {
                self.clear();
                return;
            },
        };

        if self.current_parent() == Some(traversal_parent) {
            return;
        }

        if element_depth == 0 {
            self.clear();
            return;
        }

        debug_assert!(
            element_depth != 0,
            "We should have already cleared the bloom filter"
        );
        debug_assert!(!self.elements.is_empty(), "How! We should've just rebuilt!");

        let mut current_depth = self.elements.len() - 1;

        while current_depth > element_depth - 1 {
            self.pop().expect("Emilio is bad at math");
            current_depth -= 1;
        }

        let mut common_parent = traversal_parent;
        let mut common_parent_depth = element_depth - 1;

        let mut parents_to_insert = SmallVec::<[E; 16]>::new();

        while common_parent_depth > current_depth {
            parents_to_insert.push(common_parent);
            common_parent = common_parent.traversal_parent().expect("We were lied to");
            common_parent_depth -= 1;
        }

        debug_assert_eq!(common_parent_depth, current_depth);

        while *(self.elements.last().unwrap().element) != common_parent {
            parents_to_insert.push(common_parent);
            self.pop().unwrap();
            common_parent = match common_parent.traversal_parent() {
                Some(parent) => parent,
                None => {
                    debug_assert!(self.elements.is_empty());
                    if cfg!(feature = "gecko") {
                        break;
                    } else {
                        panic!("should have found a common ancestor");
                    }
                },
            }
        }

        for parent in parents_to_insert.drain(..).rev() {
            self.push(parent);
        }

        debug_assert_eq!(self.elements.len(), element_depth);

    }
}
