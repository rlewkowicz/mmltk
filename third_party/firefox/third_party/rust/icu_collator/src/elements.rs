// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

// adapted from ICU4C and, therefore, are subject to the ICU license as
// described in LICENSE.

//! This module holds the 64-bit `CollationElement` struct used for
//! the actual comparison, the 32-bit `CollationElement32` struct
//! that's used for storage. (Strictly speaking, the storage is
//! `RawBytesULE<4>`.) And the `CollationElements` iterator adapter
//! that turns an iterator over `char` into an iterator over
//! `CollationElement`. (To match the structure of ICU4C, this isn't
//! a real Rust `Iterator`. Instead of signaling end by returning
//! `None`, it signals end by returning `NO_CE`.)
//!
//! This module also declares various constants that are also used
//! by the `comparison` module.

use core::char::REPLACEMENT_CHARACTER;
use core::marker::PhantomData;
use icu_collections::char16trie::TrieResult;
use icu_collections::codepointtrie::AbstractCodePointTrie;
use icu_collections::codepointtrie::WithTrie;
use icu_normalizer::provider::DecompositionTables;
use icu_properties::props::CanonicalCombiningClass;
use smallvec::SmallVec;
use zerovec::ule::AsULE;
use zerovec::ule::RawBytesULE;
use zerovec::{zeroslice, ZeroSlice};

use crate::provider::CollationData;

/// `true` iff `ce32`, when interpreted as `CollationElement32`,
/// is self-contained.
#[cfg(feature = "datagen")]
pub fn is_self_contained(ce32: u32) -> bool {
    CollationElement32::new(ce32)
        .to_ce_self_contained()
        .is_some()
}


/// The number of full 64-bit collation units that get buffered
/// in the primary comparison loop so that they can be examined
/// by the subsequent comparison stregths.
///
/// Note 1: If a primary difference is found, the comparison
/// returns early, so these buffers end up holding all the
/// collation elements only if there is no primary difference.
///
/// Note 2: Unfortunately for now, a sentinel value signaling
/// the end of input gets written into the buffer in addition
/// to the real collation elements.
///
/// This should probably either be halved to 4 on the logic
/// that especially in the presence of the identical prefix
/// optimization, most comparisons return after a couple of
/// primary comparisons or increased to 32 on the logic that
/// such a buffer could better hold a file or human name that
/// differs on secordary or higher level.
pub(crate) const CE_BUFFER_SIZE: usize = 8;

/// The number of extra full 64-bit collation units that have
/// already been computed as part of an operation that yields
/// multiple collation units at a time.
const PENDING_CE_BUFFER_SIZE: usize = 6;

/// Either the identical prefix or the lookahead plus the next
/// upcoming character.
///
/// The longest contraction suffix in CLDR 40 is 7 characters long.
const UPCOMING_CHARACTER_BUFFER_SIZE: usize = 10;

/// The contiguous sequence of combining characters.
const COMBINING_CHARACTER_BUFFER_SIZE: usize = 7;

/// The sequence of digits in the numeric mode.
const DIGIT_BUFFER_SIZE: usize = 8;

/// The number of combining characters that a contraction has
/// matched.
const PENDING_REMOVALS_SIZE: usize = 1;


/// Marker that the decomposition does not round trip via NFC.
///
/// See components/normalizer/trie-value-format.md
pub(crate) const NON_ROUND_TRIP_MARKER: u32 = 1 << 30;

/// Marker that the first character of the decomposition
/// can combine backwards.
///
/// See components/normalizer/trie-value-format.md
pub(crate) const BACKWARD_COMBINING_MARKER: u32 = 1 << 31;

/// Mask for the bits have to be zero for this to be a BMP
/// singleton decomposition, or value baked into the surrogate
/// range.
///
/// See components/normalizer/trie-value-format.md
pub(crate) const HIGH_ZEROS_MASK: u32 = 0x3FFF0000;

/// Mask for the bits have to be zero for this to be a complex
/// decomposition.
///
/// See components/normalizer/trie-value-format.md
pub(crate) const LOW_ZEROS_MASK: u32 = 0xFFE0;

/// Marker value for U+FDFA in NFKD. (Unified with
/// `HANGUL_SYLLABLE_MARKER`, but they differ by
/// `NON_ROUND_TRIP_MARKER`.)
///
/// See components/normalizer/trie-value-format.md
const FDFA_MARKER: u16 = 1;

/// Marker value for Hangul syllables. (Unified with `FDFA_MARKER`,
/// but they differ by `NON_ROUND_TRIP_MARKER`.)
///
/// See components/normalizer/trie-value-format.md
pub(crate) const HANGUL_SYLLABLE_MARKER: u32 = 1;

/// Checks if a trie value carries a (non-zero) canonical
/// combining class.
///
/// See components/normalizer/trie-value-format.md
fn trie_value_has_ccc(trie_value: u32) -> bool {
    (trie_value & 0x3FFFFE00) == 0xD800
}

/// Checks if the trie signifies a special non-starter decomposition.
///
/// See components/normalizer/trie-value-format.md
fn trie_value_indicates_special_non_starter_decomposition(trie_value: u32) -> bool {
    (trie_value & 0x3FFFFF00) == 0xD900
}

/// Checks if a trie value signifies a character whose decomposition
/// starts with a non-starter.
///
/// See components/normalizer/trie-value-format.md
fn decomposition_starts_with_non_starter(trie_value: u32) -> bool {
    trie_value_has_ccc(trie_value)
}

/// Extracts a canonical combining class (possibly zero) from a trie value.
///
/// See components/normalizer/trie-value-format.md
fn ccc_from_trie_value(trie_value: u32) -> CanonicalCombiningClass {
    if trie_value_has_ccc(trie_value) {
        CanonicalCombiningClass::from_icu4c_value(trie_value as u8)
    } else {
        CanonicalCombiningClass::NotReordered
    }
}

pub(crate) const HANGUL_S_BASE: u32 = 0xAC00;
pub(crate) const HANGUL_L_BASE: u32 = 0x1100;
pub(crate) const HANGUL_V_BASE: u32 = 0x1161;
pub(crate) const HANGUL_T_BASE: u32 = 0x11A7;
pub(crate) const HANGUL_T_COUNT: u32 = 28;
pub(crate) const HANGUL_N_COUNT: u32 = 588;
pub(crate) const HANGUL_S_COUNT: u32 = 11172;

pub(crate) const JAMO_COUNT: usize = 256; 

const COMBINING_DIACRITICS_BASE: usize = 0x0300;
const OPTIMIZED_DIACRITICS_LIMIT: usize = 0x034F;
pub(crate) const OPTIMIZED_DIACRITICS_MAX_COUNT: usize =
    OPTIMIZED_DIACRITICS_LIMIT - COMBINING_DIACRITICS_BASE;

pub(crate) const CASE_MASK: u16 = 0xC000;
pub(crate) const TERTIARY_MASK: u16 = 0x3F3F; 
pub(crate) const QUATERNARY_MASK: u16 = 0xC0;

const SPECIAL_CE32_LOW_BYTE: u8 = 0xC0;
pub(crate) const FALLBACK_CE32: CollationElement32 =
    CollationElement32(SPECIAL_CE32_LOW_BYTE as u32);
const LONG_PRIMARY_CE32_LOW_BYTE: u8 = 0xC1; 
/// Used only as a placeholder on the indentical prefix path.
/// The requirement is that this CE32 fails the quick mapping to a primary,
/// which is does, because the tag byte is higher than
/// `LONG_PRIMARY_CE32_LOW_BYTE`.
pub(crate) const IDENTICAL_PREFIX_HANGUL_MARKER_CE32: CollationElement32 = CollationElement32(0xC2);
const COMMON_SECONDARY_CE: u64 = 0x05000000;
const COMMON_TERTIARY_CE: u64 = 0x0500;
const COMMON_SEC_AND_TER_CE: u64 = COMMON_SECONDARY_CE | COMMON_TERTIARY_CE;

const UNASSIGNED_IMPLICIT_BYTE: u8 = 0xFE;


/// Set if the first character of every contraction suffix has lccc!=0.
const CONTRACT_NEXT_CCC: u32 = 0x200;
/// Set if any contraction suffix ends with lccc!=0.
const CONTRACT_TRAILING_CCC: u32 = 0x400;
/// Set if at least one contraction suffix contains a starter
const CONTRACT_HAS_STARTER: u32 = 0x800;

pub(crate) const NO_CE: CollationElement = CollationElement::default();
pub(crate) const NO_CE_PRIMARY: u32 = 1; 
pub(crate) const NO_CE_SECONDARY: u16 = 0x0100;
pub(crate) const NO_CE_TERTIARY: u16 = 0x0100;
pub(crate) const NO_CE_QUATERNARY: u16 = 0x0100;
const NO_CE_VALUE: u64 =
    ((NO_CE_PRIMARY as u64) << 32) | ((NO_CE_SECONDARY as u64) << 16) | (NO_CE_TERTIARY as u64); 

pub(crate) const FFFD_PRIMARY: u32 = 0xFFFD0000; 
pub(crate) const FFFD_CE_VALUE: u64 = ((FFFD_PRIMARY as u64) << 32) | COMMON_SEC_AND_TER_CE;
pub(crate) const FFFD_CE: CollationElement = CollationElement(FFFD_CE_VALUE);
pub(crate) const FFFD_CE32_VALUE: u32 = 0xFFFD0505;
pub(crate) const FFFD_CE32: CollationElement32 = CollationElement32(FFFD_CE32_VALUE);

pub(crate) const EMPTY_U16: &ZeroSlice<u16> = zeroslice![];
const SINGLE_REPLACEMENT_CHARACTER_U16: &ZeroSlice<u16> =
    zeroslice!(u16; <u16 as AsULE>::ULE::from_unsigned; [REPLACEMENT_CHARACTER as u16]);

pub(crate) const EMPTY_CHAR: &ZeroSlice<char> = zeroslice![];
const SINGLE_REPLACEMENT_CHARACTER_CHAR: &ZeroSlice<char> =
    zeroslice!(char; <char as AsULE>::ULE::from_aligned; [REPLACEMENT_CHARACTER]);

/// If `opt` is `Some`, unwrap it. If `None`, panic if debug assertions
/// are enabled and return `default` if debug assertions are not enabled.
///
/// Use this only if the only reason why `opt` could be `None` is bogus
/// data from the provider.
#[inline(always)]
pub(crate) fn unwrap_or_gigo<T>(opt: Option<T>, default: T) -> T {
    if let Some(val) = opt {
        val
    } else {
        debug_assert!(false);
        default
    }
}

/// Convert a `u32` _obtained from data provider data_ to `char`.
#[inline(always)]
pub(crate) fn char_from_u32(u: u32) -> char {
    unwrap_or_gigo(core::char::from_u32(u), REPLACEMENT_CHARACTER)
}

/// Convert a `u16` _obtained from data provider data_ to `char`.
#[inline(always)]
fn char_from_u16(u: u16) -> char {
    char_from_u32(u32::from(u))
}

#[inline(always)]
fn in_inclusive_range(c: char, start: char, end: char) -> bool {
    u32::from(c).wrapping_sub(u32::from(start)) <= (u32::from(end) - u32::from(start))
}

/// Special-CE32 tags, from bits 3..0 of a special 32-bit CE.
/// Bits 31..8 are available for tag-specific data.
/// Bits  5..4: Reserved. May be used in the future to indicate lccc!=0 and tccc!=0.
#[derive(Eq, PartialEq, Debug)]
#[allow(dead_code)]
#[repr(u8)] 
pub(crate) enum Tag {
    /// Fall back to the base collator.
    /// This is the tag value in [`SPECIAL_CE32_LOW_BYTE`] and [`FALLBACK_CE32`].
    /// Bits 31..8: Unused, 0.
    Fallback = 0,
    /// Long-primary CE with [`COMMON_SEC_AND_TER_CE`].
    /// Bits 31..8: Three-byte primary.
    LongPrimary = 1,
    /// Long-secondary CE with zero primary.
    /// Bits 31..16: Secondary weight.
    /// Bits 15.. 8: Tertiary weight.
    LongSecondary = 2,
    /// Unused.
    /// May be used in the future for single-byte secondary CEs (`SHORT_SECONDARY_TAG`),
    /// storing the secondary in bits 31..24, the ccc in bits 23..16,
    /// and the tertiary in bits 15..8.
    Reserved3 = 3,
    /// Latin mini expansions of two simple CEs [pp, 05, tt] [00, ss, 05].
    /// Bits 31..24: Single-byte primary weight pp of the first CE.
    /// Bits 23..16: Tertiary weight tt of the first CE.
    /// Bits 15.. 8: Secondary weight ss of the second CE.
    /// Unused by ICU4X, may get repurposed for jamo expansions is Korean search.
    LatinExpansion = 4,
    /// Points to one or more simple/long-primary/long-secondary 32-bit CE32s.
    /// Bits 31..13: Index into `uint32_t` table.
    /// Bits 12.. 8: Length=1..31.
    Expansion32 = 5,
    /// Points to one or more 64-bit CEs.
    /// Bits 31..13: Index into CE table.
    /// Bits 12.. 8: Length=1..31.
    Expansion = 6,
    /// Builder data, used only in the `CollationDataBuilder`, not in runtime data.
    ///
    /// If bit 8 is 0: Builder context, points to a list of context-sensitive mappings.
    /// Bits 31..13: Index to the builder's list of `ConditionalCE32` for this character.
    /// Bits 12.. 9: Unused, 0.
    ///
    /// If bit 8 is 1 (`IS_BUILDER_JAMO_CE32`): Builder-only jamoCE32 value.
    /// The builder fetches the Jamo CE32 from the trie.
    /// Bits 31..13: Jamo code point.
    /// Bits 12.. 9: Unused, 0.
    BuilderData = 7,
    /// Points to prefix trie.
    /// Bits 31..13: Index into prefix/contraction data.
    /// Bits 12.. 8: Unused, 0.
    Prefix = 8,
    /// Points to contraction data.
    /// Bits 31..13: Index into prefix/contraction data.
    /// Bits 12..11: Unused, 0.
    /// Bit      10: `CONTRACT_TRAILING_CCC` flag.
    /// Bit       9: `CONTRACT_NEXT_CCC` flag.
    /// Bit       8: `CONTRACT_SINGLE_CP_NO_MATCH` flag.
    Contraction = 9,
    /// Decimal digit.
    /// Bits 31..13: Index into `uint32_t` table for non-numeric-collation CE32.
    /// Bit      12: Unused, 0.
    /// Bits 11.. 8: Digit value 0..9.
    Digit = 10,
    /// Tag for U+0000, for moving the NUL-termination handling
    /// from the regular fastpath into specials-handling code.
    /// Bits 31..8: Unused, 0.
    /// Not used by ICU4X.
    U0000 = 11,
    /// Tag for a Hangul syllable.
    /// Bits 31..9: Unused, 0.
    /// Bit      8: `HANGUL_NO_SPECIAL_JAMO` flag.
    /// Not used by ICU4X, may get reused for compressing Hanja expansions.
    Hangul = 12,
    /// Tag for a lead surrogate code unit.
    /// Optional optimization for UTF-16 string processing.
    /// Bits 31..10: Unused, 0.
    ///       9.. 8: =0: All associated supplementary code points are unassigned-implicit.
    ///              =1: All associated supplementary code points fall back to the base data.
    ///              else: (Normally 2) Look up the data for the supplementary code point.
    /// Not used by ICU4X.
    LeadSurrogate = 13,
    /// Tag for CEs with primary weights in code point order.
    /// Bits 31..13: Index into CE table, for one data "CE".
    /// Bits 12.. 8: Unused, 0.
    ///
    /// This data "CE" has the following bit fields:
    /// Bits 63..32: Three-byte primary pppppp00.
    ///      31.. 8: Start/base code point of the in-order range.
    ///           7: Flag isCompressible primary.
    ///       6.. 0: Per-code point primary-weight increment.
    Offset = 14,
    /// Implicit CE tag. Compute an unassigned-implicit CE.
    /// All bits are set (`UNASSIGNED_CE32=0xffffffff`).
    Implicit = 15,
}

/// A compressed form of a collation element as stored in the collation
/// data.
///
/// A `CollationElement32` can be "normal" or "special".
/// Bits 7 and 6 are case bits for the "normal" case and setting
/// both is an impossible case bit combination. Hence, "special"
/// `CollationElement32`s are marked by setting both case bits
/// to 1. This is equivalent with the low byte being less than
/// `SPECIAL_CE32_LOW_BYTE` (0xC0, i.e. 0b11000000) in the "normal"
/// case and equal to or greater in the "special" case.
///
/// For the normal case:
/// Bits: 31..16: Primary weight
/// Bits: 15..8: Secondary weight
/// Bits:  7..6: Case bits (cannot both be 1 simultaneously)
/// Bits:  5..0: The high part of the discontiguous tertiary weight
/// (The quaternary weight and the low part of the discontiguous
/// tertiary weight are zero.)
///
/// For the special case:
/// Bits 31..8: tag-specific; see the documentation for `Tag`.
/// Bits  7..6: The specialness marker; both bits set to 1
/// Bits  5..4: Reserved. May be used in the future to indicate lccc!=0 and tccc!=0.
/// Bits  3..0: the tag (bit-compatible with `Tag`)
#[derive(Copy, Clone, PartialEq, Debug)]
pub(crate) struct CollationElement32(u32);

impl CollationElement32 {
    #[inline(always)]
    pub fn new(bits: u32) -> Self {
        CollationElement32(bits)
    }

    #[inline(always)]
    pub fn new_from_ule(ule: RawBytesULE<4>) -> Self {
        CollationElement32(u32::from_unaligned(ule))
    }

    #[inline(always)]
    fn low_byte(self) -> u8 {
        self.0 as u8
    }

    #[inline(always)]
    pub(crate) fn tag_checked(self) -> Option<Tag> {
        let t = self.low_byte();
        if t < SPECIAL_CE32_LOW_BYTE {
            None
        } else {
            Some(self.tag())
        }
    }

    /// Returns the tag if this element is special.
    /// Non-specialness should first be checked by seeing if either
    /// `to_ce_simple_or_long_primary()` or `to_ce_self_contained()`
    /// returns non-`None`.
    ///
    /// # Panics
    ///
    /// Panics in debug mode if called on a non-special element.
    #[inline(always)]
    pub(crate) fn tag(self) -> Tag {
        debug_assert!(self.low_byte() >= SPECIAL_CE32_LOW_BYTE);
        unsafe { core::mem::transmute(self.low_byte() & 0xF) }
    }

    /// Simplest possible check for the Latin1 fast path.
    #[cfg(feature = "latin1")]
    #[inline(always)]
    pub fn to_primary_simple(self) -> Option<u32> {
        let t = self.low_byte();
        if t < SPECIAL_CE32_LOW_BYTE {
            Some(self.0 & 0xFFFF0000)
        } else {
            None
        }
    }

    /// Extract only the first primary in the quick check without identical
    /// prefix.
    #[inline(always)]
    pub fn to_primary_in_quick_check(self, data: &CollationData) -> Option<u32> {
        let t = self.low_byte();
        if t < SPECIAL_CE32_LOW_BYTE {
            Some(self.0 & 0xFFFF0000)
        } else if t == LONG_PRIMARY_CE32_LOW_BYTE {
            Some(self.0 - u32::from(t))
        } else {
            let tag = self.tag();
            if tag == Tag::Expansion {
                Some(data.get_primary_from_ces(self.index()))
            } else {
                None
            }
        }
    }

    /// Extract only the first primary in the quick check after the identical
    /// prefix. Unlike `to_primary_in_quick_check`, this method variant can
    /// handle `Tag::Digit` if the numeric mode is not enabled. (The numeric
    /// mode requires looking ahead.)
    #[inline(always)]
    pub fn to_primary_in_quick_check_numeric(
        self,
        data: &CollationData,
        numeric: bool,
    ) -> Option<u32> {
        let mut ce32 = self;
        loop {
            let t = ce32.low_byte();
            if t < SPECIAL_CE32_LOW_BYTE {
                return Some(ce32.0 & 0xFFFF0000);
            }
            if t == LONG_PRIMARY_CE32_LOW_BYTE {
                return Some(ce32.0 - u32::from(t));
            }
            let tag = ce32.tag();
            if tag == Tag::Expansion {
                return Some(data.get_primary_from_ces(ce32.index()));
            }
            if tag == Tag::Digit && !numeric {
                ce32 = data.get_ce32(ce32.index());
                continue;
            }
            return None;
        }
    }

    /// Expands to 64 bits if the expansion is to a single 64-bit collation
    /// element and is not a long-secondary expansion.
    #[inline(always)]
    pub fn to_ce_simple_or_long_primary(self) -> Option<CollationElement> {
        let t = self.low_byte();
        if t < SPECIAL_CE32_LOW_BYTE {
            let as64 = u64::from(self.0);
            Some(CollationElement::new(
                ((as64 & 0xFFFF0000) << 32) | ((as64 & 0xFF00) << 16) | (u64::from(t) << 8),
            ))
        } else if t == LONG_PRIMARY_CE32_LOW_BYTE {
            let as64 = u64::from(self.0);
            Some(CollationElement::new(
                ((as64 - u64::from(t)) << 32) | COMMON_SEC_AND_TER_CE,
            ))
        } else {
            None
        }
    }

    /// Expands to 64 bits if the expansion is to a single 64-bit collation
    /// element.
    #[inline(always)]
    pub fn to_ce_self_contained(self) -> Option<CollationElement> {
        if let Some(ce) = self.to_ce_simple_or_long_primary() {
            return Some(ce);
        }
        if self.tag() == Tag::LongSecondary {
            Some(CollationElement::new(u64::from(self.0 & 0xffffff00)))
        } else {
            None
        }
    }

    /// Expands to 64 bits if the expansion is to a single 64-bit collation
    /// element or otherwise returns the collation element for U+FFFD.
    #[inline(always)]
    pub fn to_ce_self_contained_or_gigo(self) -> CollationElement {
        unwrap_or_gigo(self.to_ce_self_contained(), FFFD_CE)
    }

    /// Gets the length from this element.
    ///
    /// # Panics
    ///
    /// In debug builds if this element doesn't have a length.
    #[inline(always)]
    pub fn len(self) -> usize {
        debug_assert!(self.tag() == Tag::Expansion32 || self.tag() == Tag::Expansion);
        ((self.0 >> 8) & 31) as usize
    }

    /// Gets the index from this element.
    ///
    /// # Panics
    ///
    /// In debug builds if this element doesn't have an index.
    #[inline(always)]
    pub fn index(self) -> usize {
        debug_assert!(
            self.tag() == Tag::Expansion32
                || self.tag() == Tag::Expansion
                || self.tag() == Tag::Contraction
                || self.tag() == Tag::Digit
                || self.tag() == Tag::Prefix
                || self.tag() == Tag::Offset
        );
        (self.0 >> 13) as usize
    }

    #[inline(always)]
    pub fn digit(self) -> u8 {
        debug_assert!(self.tag() == Tag::Digit);
        ((self.0 >> 8) & 0xF) as u8
    }

    #[inline(always)]
    pub fn every_suffix_starts_with_combining(self) -> bool {
        debug_assert!(self.tag() == Tag::Contraction);
        (self.0 & CONTRACT_NEXT_CCC) != 0
    }
    #[inline(always)]
    pub fn at_least_one_suffix_contains_starter(self) -> bool {
        debug_assert!(self.tag() == Tag::Contraction);
        (self.0 & CONTRACT_HAS_STARTER) != 0
    }
    #[inline(always)]
    pub fn at_least_one_suffix_ends_with_non_starter(self) -> bool {
        debug_assert!(self.tag() == Tag::Contraction);
        (self.0 & CONTRACT_TRAILING_CCC) != 0
    }
}

impl Default for CollationElement32 {
    fn default() -> Self {
        CollationElement32(1) 
    }
}

/// A collation element is a 64-bit value.
///
/// Bits 63..32 are the primary weight.
/// Bits 31..16 are the secondary weight.
/// Bits 15..14 are the case bits.
/// Bits 13..8 and 5..0 are the (bitwise discontiguous) tertiary weight.
/// Bits 7..6 the quaternary weight.
#[derive(Copy, Clone, Debug, PartialEq)]
pub(crate) struct CollationElement(u64);

impl CollationElement {
    #[inline(always)]
    pub fn new(bits: u64) -> Self {
        CollationElement(bits)
    }

    #[inline(always)]
    pub fn new_from_primary(primary: u32) -> Self {
        CollationElement((u64::from(primary) << 32) | COMMON_SEC_AND_TER_CE)
    }

    #[inline(always)]
    pub fn new_from_secondary(secondary: u16) -> Self {
        CollationElement((u64::from(secondary) << 16) | COMMON_TERTIARY_CE)
    }

    #[inline(always)]
    pub fn new_implicit_from_char(c: char) -> Self {
        let mut c_with_offset = u32::from(c) + 1;
        let mut primary: u32 = 2 + (c_with_offset % 18) * 14;
        c_with_offset /= 18;
        primary |= (2 + (c_with_offset % 254)) << 8;
        c_with_offset /= 254;
        primary |= (4 + (c_with_offset % 251)) << 16;
        primary |= u32::from(UNASSIGNED_IMPLICIT_BYTE) << 24;
        CollationElement::new_from_primary(primary)
    }

    #[inline(always)]
    pub fn clone_with_non_primary_zeroed(self) -> Self {
        CollationElement(self.0 & 0xFFFFFFFF00000000)
    }

    /// Get the primary weight
    #[inline(always)]
    pub fn primary(self) -> u32 {
        (self.0 >> 32) as u32
    }

    /// Get the non-primary weights
    #[inline(always)]
    pub fn non_primary(self) -> NonPrimary {
        NonPrimary::new(self.0 as u32)
    }

    /// Get the secondary weight
    #[inline(always)]
    pub fn secondary(self) -> u16 {
        self.non_primary().secondary()
    }
    #[inline(always)]
    pub fn quaternary(self) -> u32 {
        self.non_primary().quaternary()
    }
    #[inline(always)]
    pub fn tertiary_ignorable(self) -> bool {
        self.non_primary().tertiary_ignorable()
    }
    #[inline(always)]
    pub fn either_half_zero(self) -> bool {
        self.primary() == 0 || (self.0 as u32) == 0
    }

    #[inline(always)]
    pub const fn default() -> CollationElement {
        CollationElement(NO_CE_VALUE) 
    }
}

impl Default for CollationElement {
    #[inline(always)]
    fn default() -> Self {
        CollationElement(NO_CE_VALUE) 
    }
}

impl Default for &CollationElement {
    #[inline(always)]
    fn default() -> Self {
        &CollationElement(NO_CE_VALUE) 
    }
}

/// The purpose of grouping the non-primary bits
/// into a struct is to allow for a future optimization
/// that specializes code over whether storage for primary
/// weights is needed or not. (I.e. whether to specialize
/// on `CollationElement` or `NonPrimary`.)
#[derive(Copy, Clone, PartialEq, Debug)]
pub(crate) struct NonPrimary(u32);

impl NonPrimary {
    /// Constructor
    pub fn new(bits: u32) -> Self {
        NonPrimary(bits)
    }
    /// Get the bits
    pub fn bits(self) -> u32 {
        self.0
    }
    /// Get the secondary weight
    #[inline(always)]
    pub fn secondary(self) -> u16 {
        (self.0 >> 16) as u16
    }
    /// Get the case bits as the high two bits of a u16
    #[inline(always)]
    pub fn case(self) -> u16 {
        (self.0 as u16) & CASE_MASK
    }
    /// Get the tertiary weight as u16 with the high
    /// two bits of each half zeroed.
    #[inline(always)]
    pub fn tertiary(self) -> u16 {
        (self.0 as u16) & TERTIARY_MASK
    }
    #[inline(always)]
    pub fn tertiary_ignorable(self) -> bool {
        (self.0 as u16) <= NO_CE_TERTIARY
    }
    /// Get the quaternary weight in the original
    /// storage bit positions with the other bits
    /// set to one.
    #[inline(always)]
    pub fn quaternary(self) -> u32 {
        self.0 | !(QUATERNARY_MASK as u32)
    }
    /// Get any combination of tertiary, case, and quaternary
    /// by mask.
    #[inline(always)]
    pub fn tertiary_case_quarternary(self, mask: u16) -> u16 {
        debug_assert!((mask & CASE_MASK) == CASE_MASK || (mask & CASE_MASK) == 0);
        debug_assert!((mask & TERTIARY_MASK) == TERTIARY_MASK || (mask & TERTIARY_MASK) == 0);
        debug_assert!((mask & QUATERNARY_MASK) == QUATERNARY_MASK || (mask & QUATERNARY_MASK) == 0);
        (self.0 as u16) & mask
    }

    #[inline(always)]
    pub fn case_quaternary(self) -> u16 {
        (self.0 as u16) & (CASE_MASK | QUATERNARY_MASK)
    }

    #[inline(always)]
    pub fn ignorable(self) -> bool {
        self.0 == 0
    }
}

impl Default for NonPrimary {
    #[inline(always)]
    fn default() -> Self {
        NonPrimary(0x01000100) 
    }
}

/// This struct makes the handling of the `upcoming` buffer
/// easily so that trie lookups are done at most once. However,
/// when `upcoming[0]` is an undecomposed starter, we don't
/// need the ccc yet, and when lookahead has already done the
/// trie lookups, we don't need `trie_value`, as it is implied
/// by ccc.
#[derive(Debug, Clone)]
pub(crate) struct CharacterAndClassAndTrieValue {
    c_and_c: CharacterAndClass,
    pub trie_val: u32,
}

impl CharacterAndClassAndTrieValue {
    pub fn new_with_non_decomposing_starter(c: char) -> Self {
        CharacterAndClassAndTrieValue {
            c_and_c: CharacterAndClass::new(c, CanonicalCombiningClass::NotReordered),
            trie_val: 0,
        }
    }
    pub fn new_with_non_zero_ccc(c: char, ccc: CanonicalCombiningClass) -> Self {
        CharacterAndClassAndTrieValue {
            c_and_c: CharacterAndClass::new(c, ccc),
            trie_val: 0xD800 | u32::from(ccc.to_icu4c_value()),
        }
    }
    pub fn new_with_non_special_decomposition_trie_val(c: char, trie_val: u32) -> Self {
        debug_assert!(!trie_value_indicates_special_non_starter_decomposition(
            trie_val
        ));
        CharacterAndClassAndTrieValue {
            c_and_c: CharacterAndClass::new_with_trie_value(c, trie_val),
            trie_val,
        }
    }
    pub fn new_with_trie_val(c: char, trie_val: u32) -> Self {
        if !trie_value_indicates_special_non_starter_decomposition(trie_val) {
            CharacterAndClassAndTrieValue {
                c_and_c: CharacterAndClass::new_with_trie_value(c, trie_val),
                trie_val,
            }
        } else {
            CharacterAndClassAndTrieValue {
                c_and_c: CharacterAndClass::new(c, CanonicalCombiningClass::from_icu4c_value(0xFF)),
                trie_val,
            }
        }
    }

    pub fn decomposition_starts_with_non_starter(&self) -> bool {
        decomposition_starts_with_non_starter(self.trie_val)
    }

    pub fn character(&self) -> char {
        self.c_and_c.character()
    }

    fn ccc(&self) -> CanonicalCombiningClass {
        let ret = self.c_and_c.ccc();
        debug_assert_ne!(ret, CanonicalCombiningClass::from_icu4c_value(0xFF));
        ret
    }
}

/// Pack a `char` and a `CanonicalCombiningClass` in
/// 32 bits (the former in the lower 24 bits and the
/// latter in the high 8 bits). The latter can be
/// initialized to 0xFF upon creation, in which case
/// it can be actually set later by calling
/// `set_ccc_from_trie_if_not_already_set`. This is
/// a micro optimization to avoid the Canonical
/// Combining Class trie lookup when there is only
/// one combining character in a sequence. This type
/// is intentionally non-`Copy` to get compiler help
/// in making sure that the class is set on the
/// instance on which it is intended to be set
/// and not on a temporary copy.
///
/// Note that 0xFF is won't be assigned to an actual
/// canonical combining class per definition D104
/// in The Unicode Standard.
#[derive(Debug, Clone)]
struct CharacterAndClass(u32);

impl CharacterAndClass {
    pub fn new(c: char, ccc: CanonicalCombiningClass) -> Self {
        CharacterAndClass(u32::from(c) | (u32::from(ccc.to_icu4c_value()) << 24))
    }
    pub fn new_with_placeholder(c: char) -> Self {
        CharacterAndClass(u32::from(c) | ((0xFF) << 24))
    }
    pub fn new_with_trie_value(c: char, trie_value: u32) -> Self {
        Self::new(c, ccc_from_trie_value(trie_value))
    }
    pub fn character(&self) -> char {
        unsafe { char::from_u32_unchecked(self.0 & 0xFF_FFFF) }
    }
    pub fn ccc(&self) -> CanonicalCombiningClass {
        CanonicalCombiningClass::from_icu4c_value((self.0 >> 24) as u8)
    }
    pub fn character_and_ccc(&self) -> (char, CanonicalCombiningClass) {
        (self.character(), self.ccc())
    }
    pub fn set_ccc_from_trie_if_not_already_set<'data, T: AbstractCodePointTrie<'data, u32>>(
        &mut self,
        trie: &T,
    ) {
        if self.0 >> 24 != 0xFF {
            return;
        }
        let scalar = self.character();
        self.0 = ((ccc_from_trie_value(trie.scalar(scalar)).to_icu4c_value() as u32) << 24)
            | u32::from(scalar);
    }
}

/// Iterator that transforms an iterator over `char` into an iterator
/// over `CollationElement` with a tailoring.
/// Not a real Rust iterator: Instead of `None` uses `NO_CE` to indicate
/// end of iteration to optimize comparison.
///
/// It is _extremely_ important for performance that `SmallVec`s not be
/// moved. To facilitate move-avoidance, this struct has the following
/// life cycle where `new` returns the struct in a state that is not
/// yet valid for a `next` call until `init` is called:
///
/// 1. `new`.
/// 2. Some number of calls to `iter_next_before_init` and
///    `prepend_upcoming_before_init`.
/// 3. `init`.
/// 4. Some number of calls to `next`.
pub(crate) struct CollationElements<'data, I, T>
where
    I: Iterator<Item = (char, u32)> + WithTrie<'data, T, u32>,
    T: AbstractCodePointTrie<'data, u32>,
{
    /// See components/normalizer/trie-value-format.md for the trie wrapped in `iter`.
    iter: I,
    /// Already computed but not yet returned `CollationElement`s.
    pending: SmallVec<[CollationElement; PENDING_CE_BUFFER_SIZE]>, 
    /// The index of the next item to be returned from `pending`. The purpose
    /// of this index is to avoid moving the rest of the items.
    pending_pos: usize,
    /// The characters most previously seen (or never-matching placeholders)
    /// CLDR, as of 40, has two kinds of prefixes:
    /// Prefixes that contain a single starter
    /// Prefixes that contain a starter followed by either U+3099 or U+309A
    /// Last-pushed is at index 0 and previously-pushed at index 1
    prefix: [char; 2],
    /// `upcoming` holds the characters that have already been read from
    /// `iter` but haven't yet been mapped to `CollationElement`s.
    ///
    /// Typically, `upcoming` holds one character and corresponds semantically
    /// to `pending_unnormalized_starter` in `icu::normalizer::Decomposition`.
    /// This is why there isn't a move avoidance optimization similar to
    /// `pending_pos` above for this buffer. A complex decomposition, a
    /// Hangul syllable followed by a non-starter, or lookahead can cause
    /// `pending` to hold more than one `char`.
    ///
    /// Invariant: `upcoming` is allowed to become empty only after `iter`
    /// has been exhausted.
    ///
    /// Invariant: (Checked by `debug_assert!`) At the start of `next()` call,
    /// if `upcoming` isn't empty (with `iter` having been exhausted), the
    /// first `char` in `upcoming` must have its decomposition start with a
    /// starter.
    ///
    /// TODO: Reverse the order, since now `insert(0, x)` and `remove(0)`
    /// are used more often than `push()` and `pop()`.
    upcoming: SmallVec<[CharacterAndClassAndTrieValue; UPCOMING_CHARACTER_BUFFER_SIZE]>,
    /// The root collation data.
    root: &'data CollationData<'data>,
    /// Tailoring if applicable.
    tailoring: &'data CollationData<'data>,
    /// The `CollationElement32` mapping for the Hangul Jamo block.
    ///
    /// Note: in ICU4C the jamo table contains only modern jamo. Here, the jamo table contains the whole Unicode block.
    jamo: &'data [<u32 as AsULE>::ULE; JAMO_COUNT],
    /// The `CollationElement32` mapping for the Combining Diacritical Marks block.
    diacritics: &'data ZeroSlice<u16>,
    /// NFD complex decompositions on the BMP
    scalars16: &'data ZeroSlice<u16>,
    /// NFD complex decompositions on supplementary planes
    scalars32: &'data ZeroSlice<char>,
    /// If numeric mode is enabled, the 8 high bits of the numeric primary.
    /// `None` if disabled.
    numeric_primary: Option<u8>,
    /// Whether the Lithuanian combining dot above handling is enabled.
    lithuanian_dot_above: bool,
    /// Whether `upcoming` (except the last item) has been normalized already
    upcoming_normalized: bool,
    #[cfg(debug_assertions)]
    /// Whether `iter` has been exhausted
    iter_exhausted: bool,
    #[cfg(debug_assertions)]
    /// Whether `init` has been called
    initialized: bool,
    _phantom: PhantomData<T>,
}

impl<'data, I, T> CollationElements<'data, I, T>
where
    I: Iterator<Item = (char, u32)> + WithTrie<'data, T, u32> + 'data,
    T: AbstractCodePointTrie<'data, u32> + 'data,
{
    #[expect(clippy::too_many_arguments)]
    pub fn new(
        delegate: I,
        root: &'data CollationData,
        tailoring: &'data CollationData,
        jamo: &'data [<u32 as AsULE>::ULE; JAMO_COUNT],
        diacritics: &'data ZeroSlice<u16>,
        tables: &'data DecompositionTables,
        numeric_primary: Option<u8>,
        lithuanian_dot_above: bool,
    ) -> Self {
        CollationElements::<I, T> {
            iter: delegate,
            pending: SmallVec::new(),
            pending_pos: 0,
            prefix: ['\u{FFFF}'; 2],
            upcoming: SmallVec::new(),
            root,
            tailoring,
            jamo,
            diacritics,
            scalars16: &tables.scalars16,
            scalars32: &tables.scalars24,
            numeric_primary,
            lithuanian_dot_above,
            upcoming_normalized: false,
            #[cfg(debug_assertions)]
            iter_exhausted: false,
            #[cfg(debug_assertions)]
            initialized: false,
            _phantom: PhantomData,
        }
    }

    pub fn iter_next_before_init(&mut self) -> Option<CharacterAndClassAndTrieValue> {
        #[cfg(debug_assertions)]
        debug_assert!(!self.initialized);
        self.iter_next()
    }

    pub fn prepend_upcoming_before_init(&mut self, c: CharacterAndClassAndTrieValue) {
        #[cfg(debug_assertions)]
        debug_assert!(!self.initialized);
        self.upcoming.insert(0, c);
    }

    pub fn init(&mut self) {
        #[cfg(debug_assertions)]
        {
            debug_assert!(!self.initialized);
            self.initialized = true;
        }

        loop {
            if let Some(last) = self.upcoming.last() {
                if last.decomposition_starts_with_non_starter() {
                    loop {
                        if let Some(ch) = self.iter_next() {
                            let starter = !ch.decomposition_starts_with_non_starter();
                            self.upcoming.push(ch);
                            if starter {
                                break;
                            }
                        } else {
                            #[cfg(debug_assertions)]
                            {
                                self.iter_exhausted = true;
                            }
                            break;
                        }
                    }
                }
                if let Some(first) = self.upcoming.first() {
                    if !first.decomposition_starts_with_non_starter() {
                        return;
                    }
                }
            } else {
                if let Some(ch) = self.iter_next() {
                    let starter = !ch.decomposition_starts_with_non_starter();
                    self.upcoming.push(ch);
                    if starter {
                        return;
                    }
                    continue;
                } else {
                    #[cfg(debug_assertions)]
                    {
                        self.iter_exhausted = true;
                    }
                    return;
                }
            }
            break;
        }

        self.upcoming.insert(
            0,
            CharacterAndClassAndTrieValue::new_with_non_decomposing_starter('\u{0}'),
        ); 
        let _ = self.next(); 
    }

    fn iter_next(&mut self) -> Option<CharacterAndClassAndTrieValue> {
        let (c, trie_val) = self.iter.next()?;
        Some(CharacterAndClassAndTrieValue::new_with_trie_val(
            c, trie_val,
        ))
    }

    fn next_internal(&mut self) -> Option<CharacterAndClassAndTrieValue> {
        if self.upcoming.is_empty() {
            return None;
        }
        let ret = self.upcoming.remove(0);
        if self.upcoming.is_empty() {
            if let Some(c) = self.iter_next() {
                self.upcoming.push(c);
            } else {
                #[cfg(debug_assertions)]
                {
                    self.iter_exhausted = true;
                }
            }
        }
        Some(ret)
    }

    fn maybe_gather_combining(&mut self) {
        if self.upcoming.len() != 1 {
            return;
        }
        #[expect(clippy::indexing_slicing)]
        if !self.upcoming[0].decomposition_starts_with_non_starter() {
            return;
        }
        let first = self.upcoming.remove(0);
        self.push_decomposed_combining(first);
        loop {
            if let Some(ch) = self.iter_next() {
                if ch.decomposition_starts_with_non_starter() {
                    self.push_decomposed_combining(ch);
                } else {
                    self.upcoming.push(ch);
                    break;
                }
            } else {
                #[cfg(debug_assertions)]
                {
                    self.iter_exhausted = true;
                }
                break;
            }
        }
    }

    /// Ensures that `upcoming` is normalized to NFD, except:
    /// 1. When the last item is a starter, it isn't necessarily normalized.
    /// 2. Hangul syllable are unnormalized.
    fn ensure_upcoming_normalized(&mut self) {
        if self.upcoming_normalized {
            return;
        }
        self.upcoming_normalized = true;
        let without_trailing_starter = if let Some((last, head)) = self.upcoming.split_last() {
            if !last.decomposition_starts_with_non_starter() {
                if head.is_empty() {
                    return;
                } else {
                    head
                }
            } else {
                &self.upcoming[..]
            }
        } else {
            #[cfg(debug_assertions)]
            debug_assert!(self.iter_exhausted);
            return;
        };

        if without_trailing_starter.iter().all(|c| {
            (c.trie_val
                & !(BACKWARD_COMBINING_MARKER | NON_ROUND_TRIP_MARKER | HANGUL_SYLLABLE_MARKER))
                == 0
        }) {
            return;
        }

        let mut unnormalized = core::mem::take(&mut self.upcoming);
        let last_index = unnormalized.len() - 1;
        #[expect(clippy::indexing_slicing)]
        {
            debug_assert!(!unnormalized[0].decomposition_starts_with_non_starter());
        }
        let mut start_combining = 0;
        for (i, c) in unnormalized.drain(..).enumerate() {
            if c.decomposition_starts_with_non_starter() {
                self.push_decomposed_combining(c);
            } else if i == last_index {
                #[expect(clippy::indexing_slicing)]
                self.upcoming[start_combining..].sort_by_key(|c| c.ccc());
                self.upcoming.push(c);
                return;
            } else {
                #[expect(clippy::indexing_slicing)]
                self.upcoming[start_combining..].sort_by_key(|c| c.ccc());
                start_combining = self.push_decomposed_starter(c);
            }
        }
        #[cfg(debug_assertions)]
        debug_assert!(self.iter_exhausted);
        #[expect(clippy::indexing_slicing)]
        self.upcoming[start_combining..].sort_by_key(|c| c.ccc());
    }

    fn push_decomposed_combining(&mut self, c: CharacterAndClassAndTrieValue) {
        if !trie_value_indicates_special_non_starter_decomposition(c.trie_val) {
            debug_assert!(trie_value_has_ccc(c.trie_val));
            self.upcoming.push(c);
            return;
        }

        match c.character() {
            '\u{0340}' => {
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0300}',
                        CanonicalCombiningClass::Above,
                    ));
            }
            '\u{0341}' => {
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0301}',
                        CanonicalCombiningClass::Above,
                    ));
            }
            '\u{0343}' => {
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0313}',
                        CanonicalCombiningClass::Above,
                    ));
            }
            '\u{0344}' => {
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0308}',
                        CanonicalCombiningClass::Above,
                    ));
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0301}',
                        CanonicalCombiningClass::Above,
                    ));
            }
            '\u{0F73}' => {
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0F71}',
                        CanonicalCombiningClass::CCC129,
                    ));
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0F72}',
                        CanonicalCombiningClass::CCC130,
                    ));
            }
            '\u{0F75}' => {
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0F71}',
                        CanonicalCombiningClass::CCC129,
                    ));
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0F74}',
                        CanonicalCombiningClass::CCC132,
                    ));
            }
            '\u{0F81}' => {
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0F71}',
                        CanonicalCombiningClass::CCC129,
                    ));
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_zero_ccc(
                        '\u{0F80}',
                        CanonicalCombiningClass::CCC130,
                    ));
            }
            _ => {
                debug_assert!(false);
            }
        }
    }

    fn push_decomposed_starter(&mut self, c: CharacterAndClassAndTrieValue) -> usize {
        let mut search_start_combining = false;
        let old_len = self.upcoming.len();


        let decomposition = c.trie_val;
        if (decomposition & !(BACKWARD_COMBINING_MARKER | NON_ROUND_TRIP_MARKER))
            <= HANGUL_SYLLABLE_MARKER
        {
            self.upcoming.push(
                CharacterAndClassAndTrieValue::new_with_non_decomposing_starter(c.character()),
            );
        } else {
            let high_zeros = (decomposition & HIGH_ZEROS_MASK) == 0;
            let low_zeros = (decomposition & LOW_ZEROS_MASK) == 0;
            if !high_zeros && !low_zeros {
                let starter = char_from_u32(decomposition & 0x7FFF);
                let low_c = char_from_u32((decomposition >> 15) & 0x7FFF);
                self.upcoming
                    .push(CharacterAndClassAndTrieValue::new_with_non_decomposing_starter(starter));
                let trie_value = self.iter.trie().bmp(low_c as u16);
                self.upcoming.push(
                    CharacterAndClassAndTrieValue::new_with_non_special_decomposition_trie_val(
                        low_c, trie_value,
                    ),
                );
            } else if high_zeros {
                let singleton = decomposition as u16;
                debug_assert_ne!(
                    singleton, FDFA_MARKER,
                    "How come U+FDFA NFKD marker seen in NFD?"
                );
                if (singleton & 0xFF00) == 0xD800 {
                    self.upcoming.push(c);
                    #[cfg(debug_assertions)]
                    debug_assert!(self.iter_exhausted);
                } else {
                    self.upcoming.push(
                        CharacterAndClassAndTrieValue::new_with_non_decomposing_starter(
                            char_from_u16(singleton),
                        ),
                    );
                }
            } else {
                debug_assert!(low_zeros);
                let offset = (((decomposition & !(0b11 << 30)) >> 16) as usize) - 1;
                let len_bits = decomposition & 0b1111;
                let only_non_starters_in_trail = (decomposition & 0b10000) != 0;
                if offset < self.scalars16.len() {
                    let len = (len_bits + 2) as usize;
                    for u in unwrap_or_gigo(
                        self.scalars16.get_subslice(offset..offset + len),
                        SINGLE_REPLACEMENT_CHARACTER_U16, 
                    )
                    .iter()
                    {
                        let ch = char_from_u16(u);
                        let trie_value = self.iter.trie().bmp(u);
                        self.upcoming
                            .push(CharacterAndClassAndTrieValue::new_with_non_special_decomposition_trie_val(ch, trie_value));
                    }
                } else {
                    let len = (len_bits + 1) as usize;
                    let offset32 = offset - self.scalars16.len();
                    for ch in unwrap_or_gigo(
                        self.scalars32.get_subslice(offset32..offset32 + len),
                        SINGLE_REPLACEMENT_CHARACTER_CHAR, 
                    )
                    .iter()
                    {
                        let trie_value = self.iter.trie().scalar(ch);
                        self.upcoming
                            .push(CharacterAndClassAndTrieValue::new_with_non_special_decomposition_trie_val(ch, trie_value));
                    }
                }
                search_start_combining = !only_non_starters_in_trail;
            }
        }
        if search_start_combining {
            let mut i = self.upcoming.len() - 1;
            loop {
                if let Some(ch) = self.upcoming.get(i) {
                    if ch.decomposition_starts_with_non_starter() {
                        i -= 1;
                        continue;
                    }
                    break;
                }
                debug_assert!(false);
                i = usize::MAX;
                break;
            }
            i + 1
        } else {
            old_len + 1
        }
    }

    fn push_decomposed_and_gather_combining(&mut self, c: CharacterAndClassAndTrieValue) {
        let start_combining = self.push_decomposed_starter(c);
        loop {
            if let Some(ch) = self.iter_next() {
                if ch.decomposition_starts_with_non_starter() {
                    self.push_decomposed_combining(ch);
                } else {
                    #[expect(clippy::indexing_slicing)]
                    self.upcoming[start_combining..].sort_by_key(|c| c.ccc());
                    self.upcoming.push(ch);
                    return;
                }
            } else {
                #[cfg(debug_assertions)]
                {
                    self.iter_exhausted = true;
                }
                #[expect(clippy::indexing_slicing)]
                self.upcoming[start_combining..].sort_by_key(|c| c.ccc());
                return;
            }
        }
    }

    #[expect(clippy::indexing_slicing)]
    fn look_ahead(&mut self, pos: usize) -> Option<CharacterAndClassAndTrieValue> {
        debug_assert!(self.upcoming_normalized);
        if pos + 1 == self.upcoming.len() {
            let c = self.upcoming.remove(pos);
            self.push_decomposed_and_gather_combining(c);
            Some(self.upcoming[pos].clone())
        } else if pos == self.upcoming.len() {
            if let Some(c) = self.iter_next() {
                debug_assert!(
                    false,
                    "The `upcoming` queue should be empty when iteration `pos` at the end"
                );
                self.push_decomposed_and_gather_combining(c);
                Some(self.upcoming[pos].clone())
            } else {
                #[cfg(debug_assertions)]
                {
                    self.iter_exhausted = true;
                }
                None
            }
        } else {
            Some(self.upcoming[pos].clone())
        }
    }

    fn is_next_decomposition_starts_with_starter(&self) -> bool {
        if let Some(c_c_tv) = self.upcoming.first() {
            !c_c_tv.decomposition_starts_with_non_starter()
        } else {
            true
        }
    }

    fn prepend_and_sort_non_starter_prefix_of_suffix(&mut self, c: CharacterAndClassAndTrieValue) {
        let end = 1 + {
            let mut iter = self.upcoming.iter().enumerate();
            loop {
                if let Some((i, ch)) = iter.next() {
                    if !ch.decomposition_starts_with_non_starter() {
                        break i;
                    }
                } else {
                    #[cfg(debug_assertions)]
                    {
                        self.iter_exhausted = true;
                    }
                    break self.upcoming.len();
                }
            }
        };
        let start = c.decomposition_starts_with_non_starter() as usize;
        self.upcoming.insert(0, c);
        #[expect(clippy::indexing_slicing)]
        {
            let slice: &mut [CharacterAndClassAndTrieValue] = &mut self.upcoming[start..end];
            slice.sort_by_key(|cc| cc.ccc());
        };
    }

    fn prefix_push(&mut self, c: char) {
        self.prefix[1] = self.prefix[0];
        self.prefix[0] = c;
    }

    /// Micro optimization for doing a simpler write when
    /// we know the most recent character was a non-starter
    /// that is not a kana voicing mark.
    fn mark_prefix_unmatchable(&mut self) {
        self.prefix[0] = '\u{FFFF}';
    }

    pub fn next(&mut self) -> CollationElement {
        #[cfg(debug_assertions)]
        debug_assert!(self.initialized);
        debug_assert!(self.is_next_decomposition_starts_with_starter());
        if let Some(&ret) = self.pending.get(self.pending_pos) {
            self.pending_pos += 1;
            if self.pending_pos == self.pending.len() {
                self.pending.clear();
                self.pending_pos = 0;
            }
            return ret;
        }
        debug_assert_eq!(self.pending_pos, 0);
        if let Some(c_c_tv) = self.next_internal() {
            let mut c = c_c_tv.character();
            let mut ce32;
            let mut data: &CollationData = self.tailoring;
            let mut combining_characters: SmallVec<
                [CharacterAndClass; COMBINING_CHARACTER_BUFFER_SIZE],
            > = SmallVec::new(); 


            let decomposition = c_c_tv.trie_val;
            if (decomposition & !(BACKWARD_COMBINING_MARKER | NON_ROUND_TRIP_MARKER)) == 0 {

                let jamo_index = (c as usize).wrapping_sub(HANGUL_L_BASE as usize);
                #[expect(clippy::indexing_slicing)]
                if jamo_index >= self.jamo.len() {
                    ce32 = data.ce32_for_char(c);
                    if ce32 == FALLBACK_CE32 {
                        data = self.root;
                        ce32 = data.ce32_for_char(c);
                    }
                } else {


                    data = self.root;
                    ce32 = CollationElement32::new_from_ule(self.jamo[jamo_index]);
                }
                if self.is_next_decomposition_starts_with_starter() {
                    if let Some(ce) = ce32.to_ce_simple_or_long_primary() {
                        self.prefix_push(c);
                        return ce;
                    } else if ce32.tag() == Tag::Contraction
                        && ce32.every_suffix_starts_with_combining()
                    {
                        let default = data.get_default(ce32.index());
                        if let Some(ce) = default.to_ce_simple_or_long_primary() {
                            self.prefix_push(c);
                            return ce;
                        }
                    }
                }
            } else {
                let high_zeros = (decomposition & HIGH_ZEROS_MASK) == 0;
                let low_zeros = (decomposition & LOW_ZEROS_MASK) == 0;
                if !high_zeros && !low_zeros {
                    c = char_from_u32(decomposition & 0x7FFF);
                    ce32 = data.ce32_for_char(c);
                    if ce32 == FALLBACK_CE32 {
                        data = self.root;
                        ce32 = data.ce32_for_char(c);
                    }
                    let combining = char_from_u32((decomposition >> 15) & 0x7FFF);
                    if self.is_next_decomposition_starts_with_starter() {
                        let diacritic_index =
                            (combining as usize).wrapping_sub(COMBINING_DIACRITICS_BASE);
                        if let Some(secondary) = self.diacritics.get(diacritic_index) {
                            debug_assert_ne!(combining, '\u{0344}', "Should never have COMBINING GREEK DIALYTIKA TONOS here, since it should have decomposed further.");
                            if let Some(ce) = ce32.to_ce_simple_or_long_primary() {
                                let ce_for_combining =
                                    CollationElement::new_from_secondary(secondary);
                                self.pending.push(ce_for_combining);
                                self.mark_prefix_unmatchable();
                                return ce;
                            }
                            if ce32.tag() == Tag::Contraction
                                && ce32.every_suffix_starts_with_combining()
                            {
                                let (default, mut trie) = data.get_default_and_trie(ce32.index());
                                match trie.next(combining) {
                                    TrieResult::NoMatch | TrieResult::NoValue => {
                                        if let Some(ce) = default.to_ce_simple_or_long_primary() {
                                            let ce_for_combining =
                                                CollationElement::new_from_secondary(secondary);
                                            self.pending.push(ce_for_combining);
                                            self.mark_prefix_unmatchable();
                                            return ce;
                                        }
                                    }
                                    TrieResult::Intermediate(trie_ce32) => {
                                        if !ce32.at_least_one_suffix_contains_starter() {
                                            if let Some(ce) =
                                                CollationElement32::new(trie_ce32 as u32)
                                                    .to_ce_simple_or_long_primary()
                                            {
                                                self.mark_prefix_unmatchable();
                                                return ce;
                                            }
                                        }
                                    }
                                    TrieResult::FinalValue(trie_ce32) => {
                                        if let Some(ce) = CollationElement32::new(trie_ce32 as u32)
                                            .to_ce_simple_or_long_primary()
                                        {
                                            self.mark_prefix_unmatchable();
                                            return ce;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    combining_characters.push(CharacterAndClass::new_with_placeholder(combining));
                } else if high_zeros {
                    let hangul_offset = u32::from(c).wrapping_sub(HANGUL_S_BASE); 
                    if hangul_offset < HANGUL_S_COUNT {
                        let l = hangul_offset / HANGUL_N_COUNT;
                        let v = (hangul_offset % HANGUL_N_COUNT) / HANGUL_T_COUNT;
                        let t = hangul_offset % HANGUL_T_COUNT;

                        self.mark_prefix_unmatchable();
                        #[expect(clippy::indexing_slicing)]
                        if self.is_next_decomposition_starts_with_starter() {
                            self.pending.push(
                                CollationElement32::new_from_ule(
                                    self.jamo[(HANGUL_V_BASE - HANGUL_L_BASE + v) as usize],
                                )
                                .to_ce_self_contained_or_gigo(),
                            );
                            if t != 0 {
                                self.pending.push(
                                    CollationElement32::new_from_ule(
                                        self.jamo[(HANGUL_T_BASE - HANGUL_L_BASE + t) as usize],
                                    )
                                    .to_ce_self_contained_or_gigo(),
                                );
                            }
                            return CollationElement32::new_from_ule(self.jamo[l as usize])
                                .to_ce_self_contained_or_gigo();
                        }

                        #[expect(clippy::indexing_slicing)]
                        if t != 0 {
                            self.pending.push(
                                CollationElement32::new_from_ule(
                                    self.jamo[(HANGUL_V_BASE - HANGUL_L_BASE + v) as usize],
                                )
                                .to_ce_self_contained_or_gigo(),
                            );
                            self.upcoming.insert(
                                0,
                                CharacterAndClassAndTrieValue::new_with_non_decomposing_starter(
                                    unsafe { core::char::from_u32_unchecked(HANGUL_T_BASE + t) },
                                ),
                            );
                        } else {
                            self.upcoming.insert(
                                0,
                                CharacterAndClassAndTrieValue::new_with_non_decomposing_starter(
                                    unsafe { core::char::from_u32_unchecked(HANGUL_V_BASE + v) },
                                ),
                            );
                        }

                        #[expect(clippy::indexing_slicing)]
                        return CollationElement32::new_from_ule(self.jamo[l as usize])
                            .to_ce_self_contained_or_gigo();
                    }

                    let singleton = decomposition as u16;
                    debug_assert_ne!(
                        singleton, FDFA_MARKER,
                        "How come U+FDFA NFKD marker seen in NFD?"
                    );
                    c = char_from_u16(singleton);
                    ce32 = data.ce32_for_char(c);
                    if ce32 == FALLBACK_CE32 {
                        data = self.root;
                        ce32 = data.ce32_for_char(c);
                    }
                    if self.is_next_decomposition_starts_with_starter() {
                        if let Some(ce) = ce32.to_ce_simple_or_long_primary() {
                            self.prefix_push(c);
                            return ce;
                        }
                    }
                } else {
                    debug_assert!(low_zeros);
                    let offset = (((decomposition & !(0b11 << 30)) >> 16) as usize) - 1;
                    let len_bits = decomposition & 0b1111;
                    let only_non_starters_in_trail = (decomposition & 0b10000) != 0;
                    if offset < self.scalars16.len() {
                        let len = (len_bits + 2) as usize;
                        let (starter, tail) = self
                            .scalars16
                            .get_subslice(offset..offset + len)
                            .and_then(ZeroSlice::split_first)
                            .map_or_else(
                                || {
                                    debug_assert!(false);
                                    (REPLACEMENT_CHARACTER, EMPTY_U16)
                                },
                                |(first, tail)| (char_from_u16(first), tail),
                            );
                        c = starter;
                        if only_non_starters_in_trail {
                            for u in tail.iter() {
                                let char_from_u = char_from_u16(u);
                                let trie_value = self.iter.trie().bmp(u);
                                let ccc = ccc_from_trie_value(trie_value);
                                combining_characters.push(CharacterAndClass::new(char_from_u, ccc));
                            }
                        } else {
                            let mut it = tail.iter();
                            while let Some(u) = it.next() {
                                let ch = char_from_u16(u);
                                let ccc = ccc_from_trie_value(self.iter.trie().bmp(u));
                                if ccc != CanonicalCombiningClass::NotReordered {
                                    combining_characters.push(CharacterAndClass::new(ch, ccc));
                                    continue;
                                }

                                self.maybe_gather_combining();

                                while let Some(u) = it.next_back() {
                                    let tail_char = char_from_u16(u);
                                    let trie_value = self.iter.trie().bmp(u);
                                    self.prepend_and_sort_non_starter_prefix_of_suffix(CharacterAndClassAndTrieValue::new_with_non_special_decomposition_trie_val(tail_char, trie_value));
                                }
                                self.prepend_and_sort_non_starter_prefix_of_suffix(
                                    CharacterAndClassAndTrieValue::new_with_non_decomposing_starter(
                                        ch,
                                    ),
                                );
                                break;
                            }
                        }
                    } else {
                        let len = (len_bits + 1) as usize;
                        let offset32 = offset - self.scalars16.len();
                        let (starter, tail) = self
                            .scalars32
                            .get_subslice(offset32..offset32 + len)
                            .and_then(|slice| slice.split_first())
                            .unwrap_or_else(|| {
                                debug_assert!(false);
                                (REPLACEMENT_CHARACTER, EMPTY_CHAR)
                            });

                        c = starter;
                        if only_non_starters_in_trail {
                            for ch in tail.iter() {
                                let trie_value = self.iter.trie().scalar(ch);
                                let ccc = ccc_from_trie_value(trie_value);
                                combining_characters.push(CharacterAndClass::new(ch, ccc));
                            }
                        } else {
                            let mut it = tail.iter();
                            while let Some(ch) = it.next() {
                                let ccc = ccc_from_trie_value(self.iter.trie().scalar(ch));
                                if ccc != CanonicalCombiningClass::NotReordered {
                                    combining_characters.push(CharacterAndClass::new(ch, ccc));
                                    continue;
                                }
                                self.maybe_gather_combining();

                                while let Some(tail_char) = it.next_back() {
                                    let trie_value = self.iter.trie().scalar(tail_char);
                                    self.prepend_and_sort_non_starter_prefix_of_suffix(CharacterAndClassAndTrieValue::new_with_non_special_decomposition_trie_val(tail_char, trie_value));
                                }
                                self.prepend_and_sort_non_starter_prefix_of_suffix(
                                    CharacterAndClassAndTrieValue::new_with_non_decomposing_starter(
                                        ch,
                                    ),
                                );
                                break;
                            }
                        }
                    }
                    ce32 = data.ce32_for_char(c);
                    if ce32 == FALLBACK_CE32 {
                        data = self.root;
                        ce32 = data.ce32_for_char(c);
                    }
                }
            }
            let mut may_have_contracted_starter = false;
            self.collect_combining(&mut combining_characters);
            let mut looked_ahead = 0;
            let mut drain_from_upcoming = 0;
            'outer: loop {
                'ce32loop: loop {
                    if let Some(ce) = ce32.to_ce_self_contained() {
                        self.pending.push(ce);
                        break 'ce32loop;
                    } else {
                        match ce32.tag() {
                            Tag::Expansion32 => {
                                let ce32s = data.get_ce32s(ce32.index(), ce32.len());
                                for u in ce32s.iter() {
                                    self.pending.push(
                                        CollationElement32::new(u).to_ce_self_contained_or_gigo(),
                                    );
                                }
                                break 'ce32loop;
                            }
                            Tag::Expansion => {
                                let ces = data.get_ces(ce32.index(), ce32.len());
                                for u in ces.iter() {
                                    self.pending.push(CollationElement::new(u));
                                }
                                break 'ce32loop;
                            }
                            Tag::Prefix => {
                                let (default, mut trie) = data.get_default_and_trie(ce32.index());
                                ce32 = default;
                                for &ch in self.prefix.iter() {
                                    match trie.next(ch) {
                                        TrieResult::NoValue => {}
                                        TrieResult::NoMatch => {
                                            continue 'ce32loop;
                                        }
                                        TrieResult::Intermediate(ce32_i) => {
                                            ce32 = CollationElement32::new(ce32_i as u32);
                                        }
                                        TrieResult::FinalValue(ce32_i) => {
                                            ce32 = CollationElement32::new(ce32_i as u32);
                                            continue 'ce32loop;
                                        }
                                    }
                                }
                                continue 'ce32loop;
                            }
                            Tag::Contraction => {
                                let every_suffix_starts_with_combining =
                                    ce32.every_suffix_starts_with_combining();
                                let at_least_one_suffix_contains_starter =
                                    ce32.at_least_one_suffix_contains_starter();
                                let at_least_one_suffix_ends_with_non_starter =
                                    ce32.at_least_one_suffix_ends_with_non_starter();
                                let (default, mut trie) = data.get_default_and_trie(ce32.index());
                                ce32 = default;
                                if every_suffix_starts_with_combining
                                    && combining_characters.is_empty()
                                {
                                    continue 'ce32loop;
                                }
                                let mut longest_matching_state = trie.clone();
                                let mut longest_matching_index = 0;
                                let mut attempt = 0;
                                let mut i = 0;
                                let mut most_recent_skipped_ccc =
                                    CanonicalCombiningClass::NotReordered;
                                let mut pending_removals: SmallVec<[usize; PENDING_REMOVALS_SIZE]> =
                                    SmallVec::new();
                                while let Some((character, ccc)) =
                                    combining_characters.get(i).map(|c| c.character_and_ccc())
                                {
                                    match (most_recent_skipped_ccc < ccc, trie.next(character)) {
                                        (true, TrieResult::Intermediate(ce32_i)) => {
                                            let _ = combining_characters.remove(i);
                                            while let Some(idx) = pending_removals.pop() {
                                                combining_characters.remove(idx);
                                                i -= 1; 
                                            }
                                            attempt = 0;
                                            longest_matching_index = i;
                                            longest_matching_state = trie.clone();
                                            ce32 = CollationElement32::new(ce32_i as u32);
                                        }
                                        (true, TrieResult::FinalValue(ce32_i)) => {
                                            let _ = combining_characters.remove(i);
                                            while let Some(idx) = pending_removals.pop() {
                                                combining_characters.remove(idx);
                                            }
                                            ce32 = CollationElement32::new(ce32_i as u32);
                                            continue 'ce32loop;
                                        }
                                        (_, TrieResult::NoValue) => {
                                            pending_removals.push(i);
                                            i += 1;
                                        }
                                        _ => {
                                            pending_removals.clear();
                                            most_recent_skipped_ccc = ccc;
                                            attempt += 1;
                                            i = longest_matching_index + attempt;
                                            trie = longest_matching_state.clone();
                                        }
                                    }
                                }
                                if !(at_least_one_suffix_contains_starter
                                    && combining_characters.is_empty())
                                {
                                    continue 'ce32loop;
                                }
                                may_have_contracted_starter = true;
                                debug_assert!(pending_removals.is_empty());
                                self.ensure_upcoming_normalized();
                                loop {
                                    let ahead = self.look_ahead(looked_ahead);
                                    looked_ahead += 1;
                                    if let Some(ch) = ahead {
                                        match trie.next(ch.character()) {
                                            TrieResult::NoValue => {}
                                            TrieResult::NoMatch => {
                                                if !at_least_one_suffix_ends_with_non_starter {
                                                    continue 'ce32loop;
                                                }
                                                if !ch.decomposition_starts_with_non_starter() {
                                                    continue 'ce32loop;
                                                }
                                                trie = longest_matching_state.clone();
                                                let mut longest_matching_index = 0;
                                                let mut attempt = 0;
                                                let mut i = 0;
                                                most_recent_skipped_ccc = ch.ccc();
                                                self.ensure_upcoming_normalized();
                                                loop {
                                                    let ahead = self.look_ahead(looked_ahead + i);
                                                    if let Some(ch) = ahead {
                                                        let ccc = ch.ccc();
                                                        if ccc
                                                            == CanonicalCombiningClass::NotReordered
                                                        {
                                                            continue 'ce32loop;
                                                        }
                                                        match (
                                                            most_recent_skipped_ccc < ccc,
                                                            trie.next(ch.character()),
                                                        ) {
                                                            (
                                                                true,
                                                                TrieResult::Intermediate(ce32_i),
                                                            ) => {
                                                                let _ = self
                                                                    .upcoming
                                                                    .remove(looked_ahead + i);
                                                                while let Some(idx) =
                                                                    pending_removals.pop()
                                                                {
                                                                    self.upcoming
                                                                        .remove(looked_ahead + idx);
                                                                    i -= 1; 
                                                                }
                                                                attempt = 0;
                                                                longest_matching_index = i;
                                                                longest_matching_state =
                                                                    trie.clone();
                                                                ce32 = CollationElement32::new(
                                                                    ce32_i as u32,
                                                                );
                                                            }
                                                            (
                                                                true,
                                                                TrieResult::FinalValue(ce32_i),
                                                            ) => {
                                                                let _ = self
                                                                    .upcoming
                                                                    .remove(looked_ahead + i);
                                                                while let Some(idx) =
                                                                    pending_removals.pop()
                                                                {
                                                                    self.upcoming
                                                                        .remove(looked_ahead + idx);
                                                                }
                                                                ce32 = CollationElement32::new(
                                                                    ce32_i as u32,
                                                                );
                                                                continue 'ce32loop;
                                                            }
                                                            (_, TrieResult::NoValue) => {
                                                                pending_removals.push(i);
                                                                i += 1;
                                                            }
                                                            _ => {
                                                                pending_removals.clear();
                                                                most_recent_skipped_ccc = ccc;
                                                                attempt += 1;
                                                                i = longest_matching_index
                                                                    + attempt;
                                                                trie =
                                                                    longest_matching_state.clone();
                                                            }
                                                        }
                                                    } else {
                                                        continue 'ce32loop;
                                                    }
                                                }
                                            }
                                            TrieResult::Intermediate(ce32_i) => {
                                                longest_matching_state = trie.clone();
                                                drain_from_upcoming = looked_ahead;
                                                ce32 = CollationElement32::new(ce32_i as u32);
                                            }
                                            TrieResult::FinalValue(ce32_i) => {
                                                drain_from_upcoming = looked_ahead;
                                                ce32 = CollationElement32::new(ce32_i as u32);
                                                continue 'ce32loop;
                                            }
                                        }
                                    } else {
                                        continue 'ce32loop;
                                    }
                                }
                            }
                            Tag::Digit => {
                                if let Some(high_bits) = self.numeric_primary {
                                    let mut digits: SmallVec<[u8; DIGIT_BUFFER_SIZE]> =
                                        SmallVec::new(); 
                                    digits.push(ce32.digit());
                                    let numeric_primary = u32::from(high_bits) << 24;
                                    if combining_characters.is_empty() {
                                        may_have_contracted_starter = true;
                                        self.ensure_upcoming_normalized();
                                        while let Some(upcoming) = self.look_ahead(looked_ahead) {
                                            looked_ahead += 1;
                                            ce32 =
                                                self.tailoring.ce32_for_char(upcoming.character());
                                            if ce32 == FALLBACK_CE32 {
                                                ce32 =
                                                    self.root.ce32_for_char(upcoming.character());
                                            }
                                            if ce32.tag_checked() != Some(Tag::Digit) {
                                                break;
                                            }
                                            drain_from_upcoming = looked_ahead;
                                            digits.push(ce32.digit());
                                        }
                                    }
                                    let mut remaining = digits.as_slice();
                                    while !remaining.is_empty() {

                                        loop {
                                            let Some((first, tail)) = remaining.split_first()
                                            else {
                                                remaining = &[0];
                                                break;
                                            };
                                            if *first != 0 {
                                                break;
                                            }
                                            remaining = tail;
                                        }
                                        let (head, tail) = remaining
                                            .split_at_checked(254)
                                            .unwrap_or((remaining, b""));
                                        remaining = tail;
                                        if head.len() <= 7 {
                                            let mut digit_iter = head.iter();
                                            #[expect(clippy::unwrap_used)]
                                            let mut value = u32::from(*digit_iter.next().unwrap());
                                            for &digit in digit_iter {
                                                value *= 10;
                                                value += u32::from(digit);
                                            }
                                            let mut first_byte = 2u32;
                                            let mut num_bytes = 74u32;
                                            if value < num_bytes {
                                                self.pending.push(
                                                    CollationElement::new_from_primary(
                                                        numeric_primary
                                                            | ((first_byte + value) << 16),
                                                    ),
                                                );
                                                continue;
                                            }
                                            value -= num_bytes;
                                            first_byte += num_bytes;
                                            num_bytes = 40;
                                            if value < num_bytes * 254 {
                                                self.pending.push(
                                                    CollationElement::new_from_primary(
                                                        numeric_primary
                                                            | ((first_byte + value / 254) << 16)
                                                            | ((2 + value % 254) << 8),
                                                    ),
                                                );
                                                continue;
                                            }
                                            value -= num_bytes * 254;
                                            first_byte += num_bytes;
                                            num_bytes = 16;
                                            if value < num_bytes * 254 * 254 {
                                                let mut primary =
                                                    numeric_primary | (2 + value % 254);
                                                value /= 254;
                                                primary |= (2 + value % 254) << 8;
                                                value /= 254;
                                                primary |= (first_byte + value % 254) << 16;
                                                self.pending.push(
                                                    CollationElement::new_from_primary(primary),
                                                );
                                                continue;
                                            }
                                        }
                                        debug_assert!(head.len() >= 7);

                                        let mut len = head.len();
                                        let num_pairs = (len as u32).div_ceil(2); 
                                        let mut primary =
                                            numeric_primary | ((132 - 4 + num_pairs) << 16);
                                        #[expect(clippy::indexing_slicing)]
                                        while head[len - 1] == 0 && head[len - 2] == 0 {
                                            len -= 2;
                                        }
                                        #[expect(clippy::indexing_slicing)]
                                        let mut digit_iter = head[..len].iter();
                                        #[expect(clippy::unwrap_used)]
                                        let mut pair = if len & 1 == 1 {
                                            u32::from(*digit_iter.next().unwrap())
                                        } else {
                                            u32::from(*digit_iter.next().unwrap()) * 10
                                                + u32::from(*digit_iter.next().unwrap())
                                        };
                                        pair = 11 + 2 * pair;
                                        let mut shift = 8u32;
                                        while let (Some(&left), Some(&right)) =
                                            (digit_iter.next(), digit_iter.next())
                                        {
                                            if shift == 0 {
                                                primary |= pair;
                                                self.pending.push(
                                                    CollationElement::new_from_primary(primary),
                                                );
                                                primary = numeric_primary;
                                                shift = 16;
                                            } else {
                                                primary |= pair << shift;
                                                shift -= 8;
                                            }
                                            pair =
                                                11 + 2 * (u32::from(left) * 10 + u32::from(right));
                                        }
                                        primary |= (pair - 1) << shift;
                                        self.pending
                                            .push(CollationElement::new_from_primary(primary));
                                    }
                                    break 'ce32loop;
                                }
                                ce32 = data.get_ce32(ce32.index());
                                continue 'ce32loop;
                            }
                            Tag::Offset => {
                                self.pending.push(data.ce_from_offset_ce32(c, ce32));
                                break 'ce32loop;
                            }
                            Tag::Implicit => {
                                self.pending
                                    .push(CollationElement::new_implicit_from_char(c));
                                break 'ce32loop;
                            }
                            Tag::Fallback
                            | Tag::Reserved3
                            | Tag::LongPrimary
                            | Tag::LongSecondary
                            | Tag::BuilderData
                            | Tag::LeadSurrogate
                            | Tag::LatinExpansion
                            | Tag::U0000
                            | Tag::Hangul => {
                                debug_assert!(false);
                                self.pending.push(FFFD_CE);
                                break 'ce32loop;
                            }
                        }
                    }
                }
                self.prefix_push(c);
                'combining_outer: loop {
                    debug_assert!(drain_from_upcoming == 0 || combining_characters.is_empty());
                    let mut i = 0;
                    'combining: while let Some(ch) =
                        combining_characters.get(i).map(|c| c.character())
                    {
                        c = ch;
                        let diacritic_index = (c as usize).wrapping_sub(COMBINING_DIACRITICS_BASE);
                        if let Some(secondary) = self.diacritics.get(diacritic_index) {
                            if c == '\u{0307}' && self.lithuanian_dot_above {
                                if let Some(next_c) =
                                    combining_characters.get(i + 1).map(|c| c.character())
                                {
                                    if next_c == '\u{0300}'
                                        || next_c == '\u{0301}'
                                        || next_c == '\u{0303}'
                                    {
                                        i += 1;
                                        continue 'combining;
                                    }
                                }
                            }
                            self.pending
                                .push(CollationElement::new_from_secondary(secondary));
                            self.mark_prefix_unmatchable();
                            i += 1;
                            continue 'combining;
                        }
                        let _ = combining_characters.drain(..=i);
                        data = self.tailoring;
                        ce32 = data.ce32_for_char(c);
                        if ce32 == FALLBACK_CE32 {
                            data = self.root;
                            ce32 = data.ce32_for_char(c);
                        }
                        continue 'outer;
                    }
                    i = 0;
                    while i < drain_from_upcoming {
                        #[expect(clippy::indexing_slicing)]
                        let ch = self.upcoming[i].character();
                        self.prefix_push(ch);
                        i += 1;
                    }
                    let _ = self.upcoming.drain(..drain_from_upcoming);
                    if self.upcoming.is_empty() {
                        #[cfg(debug_assertions)]
                        debug_assert!(self.iter_exhausted || may_have_contracted_starter);
                        if let Some(c_c_tv) = self.iter_next() {
                            self.upcoming.push(c_c_tv);
                        } else {
                            #[cfg(debug_assertions)]
                            {
                                self.iter_exhausted = true;
                            }
                        }
                    }
                    if may_have_contracted_starter {
                        may_have_contracted_starter = false;
                        if !self.is_next_decomposition_starts_with_starter() {
                            drain_from_upcoming = 0;
                            self.collect_combining(&mut combining_characters);
                            continue 'combining_outer;
                        }
                    }
                    #[expect(clippy::indexing_slicing)]
                    let ret = self.pending[0];
                    debug_assert_eq!(self.pending_pos, 0);
                    if self.pending.len() == 1 {
                        self.pending.clear();
                    } else {
                        self.pending_pos = 1;
                    }
                    return ret;
                }
            }
        } else {
            NO_CE
        }
    }

    #[inline(always)]
    fn collect_combining(
        &mut self,
        combining_characters: &mut SmallVec<[CharacterAndClass; COMBINING_CHARACTER_BUFFER_SIZE]>,
    ) {
        while !self.is_next_decomposition_starts_with_starter() {
            #[expect(clippy::unwrap_used)]
            let combining = self.next_internal().unwrap().c_and_c;
            let combining_c = combining.character();
            if !in_inclusive_range(combining_c, '\u{0340}', '\u{0F81}') {
                combining_characters.push(combining);
            } else {
                match combining_c {
                    '\u{0340}' => {
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0300}',
                            CanonicalCombiningClass::Above,
                        ));
                    }
                    '\u{0341}' => {
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0301}',
                            CanonicalCombiningClass::Above,
                        ));
                    }
                    '\u{0343}' => {
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0313}',
                            CanonicalCombiningClass::Above,
                        ));
                    }
                    '\u{0344}' => {
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0308}',
                            CanonicalCombiningClass::Above,
                        ));
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0301}',
                            CanonicalCombiningClass::Above,
                        ));
                    }
                    '\u{0F73}' => {
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0F71}',
                            CanonicalCombiningClass::CCC129,
                        ));
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0F72}',
                            CanonicalCombiningClass::CCC130,
                        ));
                    }
                    '\u{0F75}' => {
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0F71}',
                            CanonicalCombiningClass::CCC129,
                        ));
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0F74}',
                            CanonicalCombiningClass::CCC132,
                        ));
                    }
                    '\u{0F81}' => {
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0F71}',
                            CanonicalCombiningClass::CCC129,
                        ));
                        combining_characters.push(CharacterAndClass::new(
                            '\u{0F80}',
                            CanonicalCombiningClass::CCC130,
                        ));
                    }
                    _ => {
                        combining_characters.push(combining);
                    }
                };
            }
        }
        if combining_characters.len() > 1 {
            combining_characters
                .iter_mut()
                .for_each(|cc| cc.set_ccc_from_trie_if_not_already_set(self.iter.trie()));
            combining_characters.sort_by_key(|cc| cc.ccc());
        }
    }
}
