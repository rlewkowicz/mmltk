use super::super::{BitMask, Tag};
use core::{mem, ptr};


cfg_if! {
    if #[cfg(any(
        target_pointer_width = "64",
        target_arch = "aarch64",
        target_arch = "x86_64",
        target_arch = "wasm32",
    ))] {
        type GroupWord = u64;
        type NonZeroGroupWord = core::num::NonZeroU64;
    } else {
        type GroupWord = u32;
        type NonZeroGroupWord = core::num::NonZeroU32;
    }
}

pub(crate) type BitMaskWord = GroupWord;
pub(crate) type NonZeroBitMaskWord = NonZeroGroupWord;
pub(crate) const BITMASK_STRIDE: usize = 8;
const BITMASK_MASK: BitMaskWord = u64::from_ne_bytes([Tag::DELETED.0; 8]) as GroupWord;
pub(crate) const BITMASK_ITER_MASK: BitMaskWord = !0;

/// Helper function to replicate a tag across a `GroupWord`.
#[inline]
fn repeat(tag: Tag) -> GroupWord {
    GroupWord::from_ne_bytes([tag.0; Group::WIDTH])
}

/// Abstraction over a group of control tags which can be scanned in
/// parallel.
///
/// This implementation uses a word-sized integer.
#[derive(Copy, Clone)]
pub(crate) struct Group(GroupWord);

#[expect(clippy::use_self)]
impl Group {
    /// Number of bytes in the group.
    pub(crate) const WIDTH: usize = mem::size_of::<Self>();

    /// Returns a full group of empty tags, suitable for use as the initial
    /// value for an empty hash table.
    ///
    /// This is guaranteed to be aligned to the group size.
    #[inline]
    pub(crate) const fn static_empty() -> &'static [Tag; Group::WIDTH] {
        #[repr(C)]
        struct AlignedTags {
            _align: [Group; 0],
            tags: [Tag; Group::WIDTH],
        }
        const ALIGNED_TAGS: AlignedTags = AlignedTags {
            _align: [],
            tags: [Tag::EMPTY; Group::WIDTH],
        };
        &ALIGNED_TAGS.tags
    }

    /// Loads a group of tags starting at the given address.
    #[inline]
    pub(crate) unsafe fn load(ptr: *const Tag) -> Self {
        unsafe { Group(ptr::read_unaligned(ptr.cast())) }
    }

    /// Loads a group of tags starting at the given address, which must be
    /// aligned to `mem::align_of::<Group>()`.
    #[inline]
    pub(crate) unsafe fn load_aligned(ptr: *const Tag) -> Self {
        debug_assert_eq!(ptr.align_offset(mem::align_of::<Self>()), 0);
        unsafe { Group(ptr::read(ptr.cast())) }
    }

    /// Stores the group of tags to the given address, which must be
    /// aligned to `mem::align_of::<Group>()`.
    #[inline]
    pub(crate) unsafe fn store_aligned(self, ptr: *mut Tag) {
        debug_assert_eq!(ptr.align_offset(mem::align_of::<Self>()), 0);
        unsafe {
            ptr::write(ptr.cast(), self.0);
        }
    }

    /// Returns a `BitMask` indicating all tags in the group which *may*
    /// have the given value.
    ///
    /// This function may return a false positive in certain cases where
    /// the tag in the group differs from the searched value only in its
    /// lowest bit. This is fine because:
    /// - This never happens for `EMPTY` and `DELETED`, only full entries.
    /// - The check for key equality will catch these.
    /// - This only happens if there is at least 1 true match.
    /// - The chance of this happening is very low (< 1% chance per tag).
    #[inline]
    pub(crate) fn match_tag(self, tag: Tag) -> BitMask {
        let cmp = self.0 ^ repeat(tag);
        BitMask((cmp.wrapping_sub(repeat(Tag(0x01))) & !cmp & repeat(Tag::DELETED)).to_le())
    }

    /// Returns a `BitMask` indicating all tags in the group which are
    /// `EMPTY`.
    #[inline]
    pub(crate) fn match_empty(self) -> BitMask {
        BitMask((self.0 & (self.0 << 1) & repeat(Tag::DELETED)).to_le())
    }

    /// Returns a `BitMask` indicating all tags in the group which are
    /// `EMPTY` or `DELETED`.
    #[inline]
    pub(crate) fn match_empty_or_deleted(self) -> BitMask {
        BitMask((self.0 & repeat(Tag::DELETED)).to_le())
    }

    /// Returns a `BitMask` indicating all tags in the group which are full.
    #[inline]
    pub(crate) fn match_full(self) -> BitMask {
        BitMask(self.match_empty_or_deleted().0 ^ BITMASK_MASK)
    }

    /// Performs the following transformation on all tags in the group:
    /// - `EMPTY => EMPTY`
    /// - `DELETED => EMPTY`
    /// - `FULL => DELETED`
    #[inline]
    pub(crate) fn convert_special_to_empty_and_full_to_deleted(self) -> Self {
        let full = !self.0 & repeat(Tag::DELETED);
        Group(!full + (full >> 7))
    }
}
