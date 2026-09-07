// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

//! This module provides functionality for querying of sets of Unicode code points and strings.
//!
//! It depends on [`CodePointInversionList`] to efficiently represent Unicode code points, while
//! it also maintains a list of strings in the set.
//!
//! It is an implementation of the existing [ICU4C UnicodeSet API](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/classicu_1_1UnicodeSet.html).

#[cfg(feature = "alloc")]
use crate::codepointinvlist::CodePointInversionListBuilder;
use crate::codepointinvlist::{CodePointInversionList, CodePointInversionListULE};
#[cfg(feature = "alloc")]
use alloc::string::{String, ToString};
#[cfg(feature = "alloc")]
use alloc::vec::Vec;
use displaydoc::Display;
use yoke::Yokeable;
use zerofrom::ZeroFrom;
use zerovec::{VarZeroSlice, VarZeroVec};

/// A data structure providing a concrete implementation of a set of code points and strings,
/// using an inversion list for the code points.
///
/// This is what ICU4C calls a `UnicodeSet`.
#[zerovec::make_varule(CodePointInversionListAndStringListULE)]
#[zerovec::skip_derive(Ord)]
#[zerovec::derive(Debug)]
#[derive(Debug, Eq, PartialEq, Clone, Yokeable, ZeroFrom)]
#[cfg_attr(not(feature = "alloc"), zerovec::skip_derive(ZeroMapKV, ToOwned))]
#[cfg_attr(feature = "serde", derive(serde::Deserialize, serde::Serialize))]
#[cfg_attr(feature = "serde", zerovec::derive(Serialize, Deserialize, Debug))]
pub struct CodePointInversionListAndStringList<'data> {
    #[cfg_attr(feature = "serde", serde(borrow))]
    #[zerovec::varule(CodePointInversionListULE)]
    cp_inv_list: CodePointInversionList<'data>,
    #[cfg_attr(feature = "serde", serde(borrow))]
    str_list: VarZeroVec<'data, str>,
}

#[cfg(feature = "databake")]
impl databake::Bake for CodePointInversionListAndStringList<'_> {
    fn bake(&self, env: &databake::CrateEnv) -> databake::TokenStream {
        env.insert("icu_collections");
        let cp_inv_list = self.cp_inv_list.bake(env);
        let str_list = self.str_list.bake(env);
        databake::quote! {
            icu_collections::codepointinvliststringlist::CodePointInversionListAndStringList::from_parts_unchecked(#cp_inv_list, #str_list)
        }
    }
}

#[cfg(feature = "databake")]
impl databake::BakeSize for CodePointInversionListAndStringList<'_> {
    fn borrows_size(&self) -> usize {
        self.cp_inv_list.borrows_size() + self.str_list.borrows_size()
    }
}

impl<'data> CodePointInversionListAndStringList<'data> {
    /// Returns a new [`CodePointInversionListAndStringList`] from both a [`CodePointInversionList`] for the
    /// code points and a [`VarZeroVec`]`<`[`str`]`>` of strings.
    pub fn try_from(
        cp_inv_list: CodePointInversionList<'data>,
        str_list: VarZeroVec<'data, str>,
    ) -> Result<Self, InvalidStringList> {
        {
            let mut it = str_list.iter();
            if let Some(mut x) = it.next() {
                if x.len() == 1 {
                    return Err(InvalidStringList::InvalidStringLength(
                        #[cfg(feature = "alloc")]
                        x.to_string(),
                    ));
                }
                for y in it {
                    if x.len() == 1 {
                        return Err(InvalidStringList::InvalidStringLength(
                            #[cfg(feature = "alloc")]
                            x.to_string(),
                        ));
                    } else if x == y {
                        return Err(InvalidStringList::StringListNotUnique(
                            #[cfg(feature = "alloc")]
                            x.to_string(),
                        ));
                    } else if x > y {
                        return Err(InvalidStringList::StringListNotSorted(
                            #[cfg(feature = "alloc")]
                            x.to_string(),
                            #[cfg(feature = "alloc")]
                            y.to_string(),
                        ));
                    }

                    x = y;
                }
            }
        }

        Ok(CodePointInversionListAndStringList {
            cp_inv_list,
            str_list,
        })
    }

    #[doc(hidden)] 
    pub const fn from_parts_unchecked(
        cp_inv_list: CodePointInversionList<'data>,
        str_list: VarZeroVec<'data, str>,
    ) -> Self {
        CodePointInversionListAndStringList {
            cp_inv_list,
            str_list,
        }
    }

    /// Returns the number of elements in this set (its cardinality).
    /// Note than the elements of a set may include both individual
    /// codepoints and strings.
    pub fn size(&self) -> usize {
        self.cp_inv_list.size() + self.str_list.len()
    }

    /// Return true if this set contains multi-code point strings or the empty string.
    pub fn has_strings(&self) -> bool {
        !self.str_list.is_empty()
    }

    ///
    /// # Examples
    /// ```
    /// use icu::collections::codepointinvlist::CodePointInversionList;
    /// use icu::collections::codepointinvliststringlist::CodePointInversionListAndStringList;
    /// use zerovec::VarZeroVec;
    ///
    /// let cp_slice = &[0, 0x1_0000, 0x10_FFFF, 0x11_0000];
    /// let cp_list =
    ///    CodePointInversionList::try_from_u32_inversion_list_slice(cp_slice).unwrap();
    /// let str_slice = &["", "bmp_max", "unicode_max", "zero"];
    /// let str_list = VarZeroVec::<str>::from(str_slice);
    ///
    /// let cpilsl = CodePointInversionListAndStringList::try_from(cp_list, str_list).unwrap();
    ///
    /// assert!(cpilsl.contains_str("bmp_max"));
    /// assert!(cpilsl.contains_str(""));
    /// assert!(cpilsl.contains_str("A"));
    /// assert!(cpilsl.contains_str("ቔ"));  // U+1254 ETHIOPIC SYLLABLE QHEE
    /// assert!(!cpilsl.contains_str("bazinga!"));
    /// ```
    pub fn contains_str(&self, s: &str) -> bool {
        let mut chars = s.chars();
        if let Some(first_char) = chars.next() {
            if chars.next().is_none() {
                return self.contains(first_char);
            }
        }
        self.str_list.binary_search(s).is_ok()
    }

    /// See [`Self::contains_str`]
    pub fn contains_utf8(&self, s: &[u8]) -> bool {
        if let Ok(well_formed) = core::str::from_utf8(s) {
            self.contains_str(well_formed)
        } else {
            false
        }
    }

    ///
    /// # Examples
    /// ```
    /// use icu::collections::codepointinvlist::CodePointInversionList;
    /// use icu::collections::codepointinvliststringlist::CodePointInversionListAndStringList;
    /// use zerovec::VarZeroVec;
    ///
    /// let cp_slice = &[0, 0x80, 0xFFFF, 0x1_0000, 0x10_FFFF, 0x11_0000];
    /// let cp_list =
    ///     CodePointInversionList::try_from_u32_inversion_list_slice(cp_slice).unwrap();
    /// let str_slice = &["", "ascii_max", "bmp_max", "unicode_max", "zero"];
    /// let str_list = VarZeroVec::<str>::from(str_slice);
    ///
    /// let cpilsl = CodePointInversionListAndStringList::try_from(cp_list, str_list).unwrap();
    ///
    /// assert!(cpilsl.contains32(0));
    /// assert!(cpilsl.contains32(0x0042));
    /// assert!(!cpilsl.contains32(0x0080));
    /// ```
    pub fn contains32(&self, cp: u32) -> bool {
        self.cp_inv_list.contains32(cp)
    }

    ///
    /// # Examples
    /// ```
    /// use icu::collections::codepointinvlist::CodePointInversionList;
    /// use icu::collections::codepointinvliststringlist::CodePointInversionListAndStringList;
    /// use zerovec::VarZeroVec;
    ///
    /// let cp_slice = &[0, 0x1_0000, 0x10_FFFF, 0x11_0000];
    /// let cp_list =
    ///    CodePointInversionList::try_from_u32_inversion_list_slice(cp_slice).unwrap();
    /// let str_slice = &["", "bmp_max", "unicode_max", "zero"];
    /// let str_list = VarZeroVec::<str>::from(str_slice);
    ///
    /// let cpilsl = CodePointInversionListAndStringList::try_from(cp_list, str_list).unwrap();
    ///
    /// assert!(cpilsl.contains('A'));
    /// assert!(cpilsl.contains('ቔ'));  // U+1254 ETHIOPIC SYLLABLE QHEE
    /// assert!(!cpilsl.contains('\u{1_0000}'));
    /// assert!(!cpilsl.contains('🨫'));  // U+1FA2B NEUTRAL CHESS TURNED QUEEN
    pub fn contains(&self, ch: char) -> bool {
        self.contains32(ch as u32)
    }

    /// Access the underlying [`CodePointInversionList`].
    pub fn code_points(&self) -> &CodePointInversionList<'data> {
        &self.cp_inv_list
    }

    /// Access the contained strings.
    pub fn strings(&self) -> &VarZeroSlice<str> {
        &self.str_list
    }
}

#[cfg(feature = "alloc")]
/// ✨ *Enabled with the `alloc` Cargo feature.*
impl<'a> FromIterator<&'a str> for CodePointInversionListAndStringList<'_> {
    fn from_iter<I>(it: I) -> Self
    where
        I: IntoIterator<Item = &'a str>,
    {
        let mut builder = CodePointInversionListBuilder::new();
        let mut strings = Vec::<&str>::new();
        for s in it {
            let mut chars = s.chars();
            if let Some(first_char) = chars.next() {
                if chars.next().is_none() {
                    builder.add_char(first_char);
                    continue;
                }
            }
            strings.push(s);
        }

        strings.sort_unstable();
        strings.dedup();

        let cp_inv_list = builder.build();
        let str_list = VarZeroVec::<str>::from(&strings);

        CodePointInversionListAndStringList {
            cp_inv_list,
            str_list,
        }
    }
}

/// Custom Errors for [`CodePointInversionListAndStringList`].
#[derive(Display, Debug)]
#[allow(clippy::exhaustive_enums)] 
pub enum InvalidStringList {
    /// A string in the string list had an invalid length
    #[cfg_attr(feature = "alloc", displaydoc("Invalid string length for string: {0}"))]
    InvalidStringLength(#[cfg(feature = "alloc")] String),
    /// A string in the string list appears more than once
    #[cfg_attr(feature = "alloc", displaydoc("String list has duplicate: {0}"))]
    StringListNotUnique(#[cfg(feature = "alloc")] String),
    /// Two strings in the string list compare to each other opposite of sorted order
#[cfg_attr(feature = "alloc", displaydoc("Strings in string list not in sorted order: ({0}, {1})"))]
StringListNotSorted(
        #[cfg(feature = "alloc")] String,
        #[cfg(feature = "alloc")] String,
    ),
}
