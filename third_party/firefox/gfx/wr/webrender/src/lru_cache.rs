/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::freelist::{FreeList, FreeListHandle, WeakFreeListHandle};
use std::{mem, num};


/// Stores the data supplied by the user to be cached, and an index
/// into the LRU tracking freelist for this element.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(MallocSizeOf)]
struct LRUCacheEntry<T> {
    /// The LRU partition that tracks this entry.
    partition_index: u8,

    /// The location of the LRU tracking element for this cache entry in the
    /// right LRU partition.
    lru_index: ItemIndex,

    /// The cached data provided by the caller for this element.
    value: T,
}
/// The main public interface to the LRU cache
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(MallocSizeOf)]
pub struct LRUCache<T, M> {
    /// A free list of cache entries, and indices into the LRU tracking list
    entries: FreeList<LRUCacheEntry<T>, M>,
    /// The LRU tracking list, allowing O(1) access to the oldest element
    lru: Vec<LRUTracker<FreeListHandle<M>>>,
}

impl<T, M> LRUCache<T, M> {
    /// Construct a new LRU cache
    pub fn new(lru_partition_count: usize) -> Self {
        assert!(lru_partition_count <= u8::MAX as usize + 1);
        LRUCache {
            entries: FreeList::new(),
            lru: (0..lru_partition_count).map(|_| LRUTracker::new()).collect(),
        }
    }

    /// Insert a new element into the cache. Returns a weak handle for callers to
    /// access the data, since the lifetime is managed by the LRU algorithm and it
    /// may be evicted at any time.
    pub fn push_new(
        &mut self,
        partition_index: u8,
        value: T,
    ) -> WeakFreeListHandle<M> {

        let handle = self.entries.insert(LRUCacheEntry {
            partition_index: 0,
            lru_index: ItemIndex(num::NonZeroU32::new(1).unwrap()),
            value
        });

        let weak_handle = handle.weak();

        let entry = self.entries.get_mut(&handle);
        let lru_index = self.lru[partition_index as usize].push_new(handle);
        entry.partition_index = partition_index;
        entry.lru_index = lru_index;

        weak_handle
    }

    /// Get immutable access to the data at a given slot. Since this takes a weak
    /// handle, it may have been evicted, so returns an Option.
    pub fn get_opt(
        &self,
        handle: &WeakFreeListHandle<M>,
    ) -> Option<&T> {
        self.entries
            .get_opt(handle)
            .map(|entry| {
                &entry.value
            })
    }

    /// Get mutable access to the data at a given slot. Since this takes a weak
    /// handle, it may have been evicted, so returns an Option.
    pub fn get_opt_mut(
        &mut self,
        handle: &WeakFreeListHandle<M>,
    ) -> Option<&mut T> {
        self.entries
            .get_opt_mut(handle)
            .map(|entry| {
                &mut entry.value
            })
    }

    /// Return a reference to the oldest item in the cache, keeping it in the cache.
    /// If the cache is empty, this will return None.
    pub fn peek_oldest(&self, partition_index: u8) -> Option<&T> {
        self.lru[partition_index as usize]
            .peek_front()
            .map(|handle| {
                let entry = self.entries.get(handle);
                &entry.value
            })
    }

    /// Remove the oldest item from the cache. This is used to select elements to
    /// be evicted. If the cache is empty, this will return None.
    pub fn pop_oldest(
        &mut self,
        partition_index: u8,
    ) -> Option<T> {
        self.lru[partition_index as usize]
            .pop_front()
            .map(|handle| {
                let entry = self.entries.free(handle);
                entry.value
            })
    }

    /// This is a special case of `push_new`, which is a requirement for the texture
    /// cache. Sometimes, we want to replace the content of an existing handle if it
    /// exists, or insert a new element if the handle is invalid (for example, if an
    /// image is resized and it moves to a new location in the texture atlas). This
    /// method returns the old cache entry if it existed, so it can be freed by the caller.
    #[must_use]
    pub fn replace_or_insert(
        &mut self,
        handle: &mut WeakFreeListHandle<M>,
        partition_index: u8,
        data: T,
    ) -> Option<T> {
        match self.entries.get_opt_mut(handle) {
            Some(entry) => {
                if entry.partition_index != partition_index {
                    let strong_handle = self.lru[entry.partition_index as usize].remove(entry.lru_index);
                    let lru_index = self.lru[partition_index as usize].push_new(strong_handle);
                    entry.partition_index = partition_index;
                    entry.lru_index = lru_index;
                }
                Some(mem::replace(&mut entry.value, data))
            }
            None => {
                *handle = self.push_new(partition_index, data);
                None
            }
        }
    }

    /// Manually evict a specific item.
    pub fn remove(&mut self, handle: &WeakFreeListHandle<M>) -> Option<T> {
        if let Some(entry) = self.entries.get_opt_mut(handle) {
            let strong_handle = self.lru[entry.partition_index as usize].remove(entry.lru_index);
            return Some(self.entries.free(strong_handle).value);
        }

        None
    }

    /// This is used by the calling code to signal that the element that this handle
    /// references has been used on this frame. Internally, it updates the links in
    /// the LRU tracking element to move this item to the end of the LRU list. Returns
    /// the underlying data in case the client wants to mutate it.
    pub fn touch(
        &mut self,
        handle: &WeakFreeListHandle<M>,
    ) -> Option<&mut T> {
        let lru = &mut self.lru;

        self.entries
            .get_opt_mut(handle)
            .map(|entry| {
                lru[entry.partition_index as usize].mark_used(entry.lru_index);
                &mut entry.value
            })
    }

}

/// Index of an LRU tracking element
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, Copy, Clone, PartialEq, Eq, Hash, MallocSizeOf)]
struct ItemIndex(num::NonZeroU32);

impl ItemIndex {
    fn as_usize(&self) -> usize {
        self.0.get() as usize
    }
}

/// Stores a strong handle controlling the lifetime of the data in the LRU
/// cache, and a doubly-linked list node specifying where in the current LRU
/// order this element exists. These items are themselves backed by a freelist
/// to minimize heap allocations and improve cache access patterns.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, MallocSizeOf)]
struct Item<H> {
    prev: Option<ItemIndex>,
    next: Option<ItemIndex>,
    handle: Option<H>,
}

/// Internal implementation of the LRU tracking list
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(MallocSizeOf)]
struct LRUTracker<H> {
    /// Current head of the list - this is the oldest item that will be evicted next.
    head: Option<ItemIndex>,
    /// Current tail of the list - this is the most recently used element.
    tail: Option<ItemIndex>,
    /// As tracking items are removed, they are stored in a freelist, to minimize heap allocations
    free_list_head: Option<ItemIndex>,
    /// The freelist that stores all the LRU tracking items
    items: Vec<Item<H>>,
}

impl<H> LRUTracker<H> where H: std::fmt::Debug {
    /// Construct a new LRU tracker
    fn new() -> Self {
        let items = vec![
            Item {
                prev: None,
                next: None,
                handle: None,
            },
        ];

        LRUTracker {
            head: None,
            tail: None,
            free_list_head: None,
            items,
        }
    }

    /// Internal function that takes an item index, and links it to the
    /// end of the tracker list (makes it the newest item).
    fn link_as_new_tail(
        &mut self,
        item_index: ItemIndex,
    ) {
        match (self.head, self.tail) {
            (Some(..), Some(tail)) => {
                self.items[item_index.as_usize()].prev = Some(tail);
                self.items[item_index.as_usize()].next = None;

                self.items[tail.as_usize()].next = Some(item_index);
                self.tail = Some(item_index);
            }
            (None, None) => {
                self.items[item_index.as_usize()].prev = None;
                self.items[item_index.as_usize()].next = None;

                self.head = Some(item_index);
                self.tail = Some(item_index);
            }
            (Some(..), None) | (None, Some(..)) => {
                unreachable!();
            }
        }
    }

    /// Internal function that takes an LRU item index, and removes it from
    /// the current doubly linked list. Used during removal of items, and also
    /// when items are moved to the back of the list as they're touched.
    fn unlink(
        &mut self,
        item_index: ItemIndex,
    ) {
        let (next, prev) = {
            let item = &self.items[item_index.as_usize()];
            (item.next, item.prev)
        };

        match next {
            Some(next) => {
                self.items[next.as_usize()].prev = prev;
            }
            None => {
                debug_assert_eq!(self.tail, Some(item_index));
                self.tail = prev;
            }
        }

        match prev {
            Some(prev) => {
                self.items[prev.as_usize()].next = next;
            }
            None => {
                debug_assert_eq!(self.head, Some(item_index));
                self.head = next;
            }
        }
    }

    /// Push a new LRU tracking item on to the back of the list, marking
    /// it as the most recent item.
    fn push_new(
        &mut self,
        handle: H,
    ) -> ItemIndex {
        let item_index = match self.free_list_head {
            Some(index) => {
                let item = &mut self.items[index.as_usize()];

                assert!(item.handle.is_none());
                item.handle = Some(handle);

                self.free_list_head = item.next;

                index
            }
            None => {
                let index = ItemIndex(num::NonZeroU32::new(self.items.len() as u32).unwrap());

                self.items.push(Item {
                    prev: None,
                    next: None,
                    handle: Some(handle),
                });

                index
            }
        };

        self.link_as_new_tail(item_index);

        item_index
    }

    /// Returns a reference to the oldest element, or None if the list is empty.
    fn peek_front(&self) -> Option<&H> {
        self.head.map(|head| self.items[head.as_usize()].handle.as_ref().unwrap())
    }

    /// Remove the oldest element from the front of the LRU list. Returns None
    /// if the list is empty.
    fn pop_front(
        &mut self,
    ) -> Option<H> {
        let handle = match (self.head, self.tail) {
            (Some(head), Some(tail)) => {
                let item_index = head;

                if head == tail {
                    self.head = None;
                    self.tail = None;
                } else {
                    let new_head = self.items[head.as_usize()].next.unwrap();
                    self.head = Some(new_head);
                    self.items[new_head.as_usize()].prev = None;
                }

                self.items[item_index.as_usize()].next = self.free_list_head;
                self.free_list_head = Some(item_index);

                Some(self.items[item_index.as_usize()].handle.take().unwrap())
            }
            (None, None) => {
                None
            }
            (Some(..), None) | (None, Some(..)) => {
                unreachable!();
            }
        };

        handle
    }

    /// Manually remove an item from the LRU tracking list. This is used
    /// when an element switches from one LRU partition to a different one.
    fn remove(
        &mut self,
        index: ItemIndex,
    ) -> H {
        self.unlink(index);

        let handle = self.items[index.as_usize()].handle.take().unwrap();

        self.items[index.as_usize()].next = self.free_list_head;
        self.free_list_head = Some(index);

        handle
    }

    /// Called to mark that an item was used on this frame. It unlinks the
    /// tracking item, and then re-links it to the back of the list.
    fn mark_used(
        &mut self,
        index: ItemIndex,
    ) {
        self.unlink(index);
        self.link_as_new_tail(index);
    }

}
