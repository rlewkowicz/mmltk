// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

#![allow(unsafe_code)]

use core::slice;
use std::{
    fmt::Debug,
    mem::MaybeUninit,
    ops::{Deref, DerefMut},
};

/// Note: this implementation of SmallVec is not panic-safe, in the sense
/// that in presence of panics the SmallVec will be left in some valid but
/// unspecified state.
pub enum SmallVec<T, const N: usize> {
    Stack {
        len: usize,
        data: [MaybeUninit<T>; N],
    },
    Heap(Vec<T>),
}

impl<T, const N: usize> Deref for SmallVec<T, N> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        match self {
            SmallVec::Stack { len, data } => {
                let data = &data[..*len];
                unsafe { slice::from_raw_parts(data.as_ptr().cast::<T>(), data.len()) }
            }
            SmallVec::Heap(v) => &v[..],
        }
    }
}

impl<T, const N: usize> DerefMut for SmallVec<T, N> {
    fn deref_mut(&mut self) -> &mut [T] {
        match self {
            SmallVec::Stack { len, data } => {
                let data = &mut data[..*len];
                unsafe { slice::from_raw_parts_mut(data.as_mut_ptr().cast::<T>(), data.len()) }
            }
            SmallVec::Heap(v) => &mut v[..],
        }
    }
}

impl<T: Debug, const N: usize> Debug for SmallVec<T, N> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "SmallVec<{N}>({:?})", &**self)
    }
}

impl<T, const N: usize> Default for SmallVec<T, N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T, const N: usize> SmallVec<T, N> {
    #[inline]
    pub fn new() -> Self {
        Self::Stack {
            len: 0,
            data: [const { MaybeUninit::uninit() }; N],
        }
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        match self {
            Self::Stack { len, .. } => *len == 0,
            Self::Heap(v) => v.is_empty(),
        }
    }

    #[inline]
    pub fn len(&self) -> usize {
        match self {
            Self::Stack { len, .. } => *len,
            Self::Heap(v) => v.len(),
        }
    }

    #[inline(never)]
    fn move_to_heap(&mut self) {
        let Self::Stack { len, data } = self else {
            return;
        };
        let mut ret = Vec::<T>::with_capacity(*len);
        let old_len = *len;
        *len = 0;
        for data in data[..old_len].iter_mut() {
            let mut tmp = MaybeUninit::uninit();
            std::mem::swap(&mut tmp, data);
            ret.push(unsafe { tmp.assume_init() });
        }
        *self = Self::Heap(ret);
    }

    #[inline(always)]
    pub fn extend<I: IntoIterator<Item = T>>(&mut self, iter: I) {
        let mut iter = iter.into_iter();
        let new_size = iter.size_hint().1.and_then(|x| x.checked_add(self.len()));
        if new_size.is_none_or(|u| u > N) {
            self.move_to_heap();
        }
        let (len, data) = match self {
            Self::Heap(v) => {
                v.extend(iter);
                return;
            }
            Self::Stack { len, data } => (len, data),
        };

        while *len < N
            && let Some(e) = iter.next()
        {
            data[*len].write(e);
            *len += 1;
        }
    }

    #[inline]
    pub fn push(&mut self, val: T) {
        if self.len() + 1 > N {
            self.move_to_heap();
        }
        let (len, data) = match self {
            Self::Heap(v) => {
                v.push(val);
                return;
            }
            Self::Stack { len, data } => (len, data),
        };
        data[*len].write(val);
        *len += 1;
    }

    #[inline]
    pub fn extend_sv<const M: usize>(&mut self, mut other: SmallVec<T, M>) {
        if self.len() + other.len() > N {
            self.move_to_heap();
        }
        if matches!(self, Self::Heap(_)) {
            other.move_to_heap();
        }
        if matches!(other, SmallVec::Heap(_)) {
            self.move_to_heap();
        }
        let (len, data) = match self {
            Self::Heap(v) => {
                let SmallVec::Heap(o) = &mut other else {
                    unreachable!()
                };
                v.extend(std::mem::take(o));
                return;
            }
            Self::Stack { len, data } => (len, data),
        };

        let SmallVec::Stack {
            len: olen,
            data: odata,
        } = &mut other
        else {
            unreachable!()
        };
        let other_len = *olen;
        *olen = 0;
        data[*len..*len + other_len].swap_with_slice(&mut odata[..other_len]);
        *len += other_len;
    }
}

impl<T, const N: usize> FromIterator<T> for SmallVec<T, N> {
    #[inline]
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> Self {
        let mut ret = Self::new();
        ret.extend(iter);
        ret
    }
}

impl<T, const N: usize> Drop for SmallVec<T, N> {
    fn drop(&mut self) {
        if let SmallVec::Stack { len, data } = self {
            let old_len = *len;
            *len = 0;
            for el in data[..old_len].iter_mut() {
                unsafe { el.assume_init_drop() };
            }
        }
    }
}
