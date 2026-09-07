// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use super::*;
use core::cmp::Ordering;
use core::marker::PhantomData;
use core::mem::{self, MaybeUninit};

/// This type is the [`ULE`] type for `Option<U>` where `U` is a [`ULE`] type
///
/// # Example
///
/// ```rust
/// use zerovec::ZeroVec;
///
/// let z = ZeroVec::alloc_from_slice(&[
///     Some('a'),
///     Some('á'),
///     Some('ø'),
///     None,
///     Some('ł'),
/// ]);
///
/// assert_eq!(z.get(2), Some(Some('ø')));
/// assert_eq!(z.get(3), Some(None));
/// ```
#[repr(C, packed)]
pub struct OptionULE<U>(bool, MaybeUninit<U>);

impl<U: Copy> OptionULE<U> {
    /// Obtain this as an `Option<T>`
    pub fn get(self) -> Option<U> {
        if self.0 {
            unsafe {
                Some(self.1.assume_init())
            }
        } else {
            None
        }
    }

    /// Construct an `OptionULE<U>` from an equivalent `Option<T>`
    pub fn new(opt: Option<U>) -> Self {
        if let Some(inner) = opt {
            Self(true, MaybeUninit::new(inner))
        } else {
            Self(false, MaybeUninit::zeroed())
        }
    }
}

impl<U: Copy + core::fmt::Debug> core::fmt::Debug for OptionULE<U> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

///    zeroed or valid-T byte sequences to fill it)
unsafe impl<U: ULE> ULE for OptionULE<U> {
    fn validate_bytes(bytes: &[u8]) -> Result<(), UleError> {
        let size = mem::size_of::<Self>();
        if bytes.len() % size != 0 {
            return Err(UleError::length::<Self>(bytes.len()));
        }
        for chunk in bytes.chunks(size) {
            #[expect(clippy::indexing_slicing)] 
            match chunk[0] {
                0 => {
                    if !chunk[1..].iter().all(|x| *x == 0) {
                        return Err(UleError::parse::<Self>());
                    }
                }
                1 => U::validate_bytes(&chunk[1..])?,
                _ => return Err(UleError::parse::<Self>()),
            }
        }
        Ok(())
    }
}

impl<T: AsULE> AsULE for Option<T> {
    type ULE = OptionULE<T::ULE>;
    fn to_unaligned(self) -> OptionULE<T::ULE> {
        OptionULE::new(self.map(T::to_unaligned))
    }

    fn from_unaligned(other: OptionULE<T::ULE>) -> Self {
        other.get().map(T::from_unaligned)
    }
}

impl<U: Copy> Copy for OptionULE<U> {}

impl<U: Copy> Clone for OptionULE<U> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<U: Copy + PartialEq> PartialEq for OptionULE<U> {
    fn eq(&self, other: &Self) -> bool {
        self.get().eq(&other.get())
    }
}

impl<U: Copy + Eq> Eq for OptionULE<U> {}

/// A type allowing one to represent `Option<U>` for [`VarULE`] `U` types.
///
/// ```rust
/// use zerovec::ule::OptionVarULE;
/// use zerovec::VarZeroVec;
///
/// let mut zv: VarZeroVec<OptionVarULE<str>> = VarZeroVec::new();
///
/// zv.make_mut().push(&None::<&str>);
/// zv.make_mut().push(&Some("hello"));
/// zv.make_mut().push(&Some("world"));
/// zv.make_mut().push(&None::<&str>);
///
/// assert_eq!(zv.get(0).unwrap().as_ref(), None);
/// assert_eq!(zv.get(1).unwrap().as_ref(), Some("hello"));
/// ```
#[repr(C, packed)]
pub struct OptionVarULE<U: VarULE + ?Sized>(PhantomData<U>, bool, [u8]);

impl<U: VarULE + ?Sized> OptionVarULE<U> {
    /// Obtain this as an `Option<&U>`
    pub fn as_ref(&self) -> Option<&U> {
        if self.1 {
            unsafe {
                Some(U::from_bytes_unchecked(&self.2))
            }
        } else {
            None
        }
    }
}

impl<U: VarULE + ?Sized + core::fmt::Debug> core::fmt::Debug for OptionVarULE<U> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.as_ref().fmt(f)
    }
}

unsafe impl<U: VarULE + ?Sized> VarULE for OptionVarULE<U> {
    #[inline]
    fn validate_bytes(slice: &[u8]) -> Result<(), UleError> {
        if slice.is_empty() {
            return Err(UleError::length::<Self>(slice.len()));
        }
        #[expect(clippy::indexing_slicing)] 
        match slice[0] {
            0 => {
                if slice.len() != 1 {
                    Err(UleError::length::<Self>(slice.len()))
                } else {
                    Ok(())
                }
            }
            1 => U::validate_bytes(&slice[1..]),
            _ => Err(UleError::parse::<Self>()),
        }
    }

    #[inline]
    unsafe fn from_bytes_unchecked(bytes: &[u8]) -> &Self {
        let entire_struct_as_slice: *const [u8] =
            ::core::ptr::slice_from_raw_parts(bytes.as_ptr(), bytes.len() - 1);
        &*(entire_struct_as_slice as *const Self)
    }
}

unsafe impl<T, U> EncodeAsVarULE<OptionVarULE<U>> for Option<T>
where
    T: EncodeAsVarULE<U>,
    U: VarULE + ?Sized,
{
    fn encode_var_ule_as_slices<R>(&self, _: impl FnOnce(&[&[u8]]) -> R) -> R {
        unreachable!()
    }

    #[inline]
    fn encode_var_ule_len(&self) -> usize {
        if let Some(ref inner) = *self {
            1 + inner.encode_var_ule_len()
        } else {
            1
        }
    }

    #[expect(clippy::indexing_slicing)] 
    fn encode_var_ule_write(&self, dst: &mut [u8]) {
        if let Some(ref inner) = *self {
            debug_assert!(
                !dst.is_empty(),
                "OptionVarULE must have at least one byte when Some"
            );
            dst[0] = 1;
            inner.encode_var_ule_write(&mut dst[1..]);
        } else {
            debug_assert!(
                dst.len() == 1,
                "OptionVarULE must have exactly one byte when None"
            );
            dst[0] = 0;
        }
    }
}

impl<U: VarULE + ?Sized + PartialEq> PartialEq for OptionVarULE<U> {
    fn eq(&self, other: &Self) -> bool {
        self.as_ref().eq(&other.as_ref())
    }
}

impl<U: VarULE + ?Sized + Eq> Eq for OptionVarULE<U> {}

impl<U: VarULE + ?Sized + PartialOrd> PartialOrd for OptionVarULE<U> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        self.as_ref().partial_cmp(&other.as_ref())
    }
}

impl<U: VarULE + ?Sized + Ord> Ord for OptionVarULE<U> {
    fn cmp(&self, other: &Self) -> Ordering {
        self.as_ref().cmp(&other.as_ref())
    }
}
