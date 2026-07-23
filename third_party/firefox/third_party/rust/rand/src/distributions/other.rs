// Copyright 2018 Developers of the Rand project.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! The implementations of the `Standard` distribution for other built-in types.

use core::char;
use core::num::Wrapping;
#[cfg(feature = "alloc")]
use alloc::string::String;

use crate::distributions::{Distribution, Standard, Uniform};
#[cfg(feature = "alloc")]
use crate::distributions::DistString;
use crate::Rng;

#[cfg(feature = "serde1")]
use serde::{Serialize, Deserialize};
#[cfg(feature = "min_const_gen")]
use core::mem::{self, MaybeUninit};



/// Sample a `u8`, uniformly distributed over ASCII letters and numbers:
/// a-z, A-Z and 0-9.
///
/// # Example
///
/// ```
/// use rand::{Rng, thread_rng};
/// use rand::distributions::Alphanumeric;
///
/// let mut rng = thread_rng();
/// let chars: String = (0..7).map(|_| rng.sample(Alphanumeric) as char).collect();
/// println!("Random chars: {}", chars);
/// ```
///
/// The [`DistString`] trait provides an easier method of generating
/// a random `String`, and offers more efficient allocation:
/// ```
/// use rand::distributions::{Alphanumeric, DistString};
/// let string = Alphanumeric.sample_string(&mut rand::thread_rng(), 16);
/// println!("Random string: {}", string);
/// ```
///
/// # Passwords
///
/// Users sometimes ask whether it is safe to use a string of random characters
/// as a password. In principle, all RNGs in Rand implementing `CryptoRng` are
/// suitable as a source of randomness for generating passwords (if they are
/// properly seeded), but it is more conservative to only use randomness
/// directly from the operating system via the `getrandom` crate, or the
/// corresponding bindings of a crypto library.
///
/// When generating passwords or keys, it is important to consider the threat
/// model and in some cases the memorability of the password. This is out of
/// scope of the Rand project, and therefore we defer to the following
/// references:
///
/// - [Wikipedia article on Password Strength](https://en.wikipedia.org/wiki/Password_strength)
/// - [Diceware for generating memorable passwords](https://en.wikipedia.org/wiki/Diceware)
#[derive(Debug, Clone, Copy)]
#[cfg_attr(feature = "serde1", derive(Serialize, Deserialize))]
pub struct Alphanumeric;



impl Distribution<char> for Standard {
    #[inline]
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> char {
        const GAP_SIZE: u32 = 0xDFFF - 0xD800 + 1;

        let range = Uniform::new(GAP_SIZE, 0x11_0000);

        let mut n = range.sample(rng);
        if n <= 0xDFFF {
            n -= GAP_SIZE;
        }
        unsafe { char::from_u32_unchecked(n) }
    }
}

/// Note: the `String` is potentially left with excess capacity; optionally the
/// user may call `string.shrink_to_fit()` afterwards.
#[cfg(feature = "alloc")]
impl DistString for Standard {
    fn append_string<R: Rng + ?Sized>(&self, rng: &mut R, s: &mut String, len: usize) {
        s.reserve(4 * len);
        s.extend(Distribution::<char>::sample_iter(self, rng).take(len));
    }
}

impl Distribution<u8> for Alphanumeric {
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> u8 {
        const RANGE: u32 = 26 + 26 + 10;
        const GEN_ASCII_STR_CHARSET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ\
                abcdefghijklmnopqrstuvwxyz\
                0123456789";
        loop {
            let var = rng.next_u32() >> (32 - 6);
            if var < RANGE {
                return GEN_ASCII_STR_CHARSET[var as usize];
            }
        }
    }
}

#[cfg(feature = "alloc")]
impl DistString for Alphanumeric {
    fn append_string<R: Rng + ?Sized>(&self, rng: &mut R, string: &mut String, len: usize) {
        unsafe {
            let v = string.as_mut_vec();
            v.extend(self.sample_iter(rng).take(len));
        }
    }
}

impl Distribution<bool> for Standard {
    #[inline]
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> bool {
        (rng.next_u32() as i32) < 0
    }
}

macro_rules! tuple_impl {
    ($($tyvar:ident),* ) => {
        impl< $( $tyvar ),* >
            Distribution<( $( $tyvar ),* , )>
            for Standard
            where $( Standard: Distribution<$tyvar> ),*
        {
            #[inline]
            fn sample<R: Rng + ?Sized>(&self, _rng: &mut R) -> ( $( $tyvar ),* , ) {
                (
                    $(
                        _rng.gen::<$tyvar>()
                    ),*
                    ,
                )
            }
        }
    }
}

impl Distribution<()> for Standard {
    #[allow(clippy::unused_unit)]
    #[inline]
    fn sample<R: Rng + ?Sized>(&self, _: &mut R) -> () {
        ()
    }
}
tuple_impl! {A}
tuple_impl! {A, B}
tuple_impl! {A, B, C}
tuple_impl! {A, B, C, D}
tuple_impl! {A, B, C, D, E}
tuple_impl! {A, B, C, D, E, F}
tuple_impl! {A, B, C, D, E, F, G}
tuple_impl! {A, B, C, D, E, F, G, H}
tuple_impl! {A, B, C, D, E, F, G, H, I}
tuple_impl! {A, B, C, D, E, F, G, H, I, J}
tuple_impl! {A, B, C, D, E, F, G, H, I, J, K}
tuple_impl! {A, B, C, D, E, F, G, H, I, J, K, L}

#[cfg(feature = "min_const_gen")]
#[cfg_attr(docsrs, doc(cfg(feature = "min_const_gen")))]
impl<T, const N: usize> Distribution<[T; N]> for Standard
where Standard: Distribution<T>
{
    #[inline]
    fn sample<R: Rng + ?Sized>(&self, _rng: &mut R) -> [T; N] {
        let mut buff: [MaybeUninit<T>; N] = unsafe { MaybeUninit::uninit().assume_init() };

        for elem in &mut buff {
            *elem = MaybeUninit::new(_rng.gen());
        }

        unsafe { mem::transmute_copy::<_, _>(&buff) }
    }
}

#[cfg(not(feature = "min_const_gen"))]
macro_rules! array_impl {
    {$n:expr, $t:ident, $($ts:ident,)*} => {
        array_impl!{($n - 1), $($ts,)*}

        impl<T> Distribution<[T; $n]> for Standard where Standard: Distribution<T> {
            #[inline]
            fn sample<R: Rng + ?Sized>(&self, _rng: &mut R) -> [T; $n] {
                [_rng.gen::<$t>(), $(_rng.gen::<$ts>()),*]
            }
        }
    };
    {$n:expr,} => {
        impl<T> Distribution<[T; $n]> for Standard {
            fn sample<R: Rng + ?Sized>(&self, _rng: &mut R) -> [T; $n] { [] }
        }
    };
}

#[cfg(not(feature = "min_const_gen"))]
array_impl! {32, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T, T,}

impl<T> Distribution<Option<T>> for Standard
where Standard: Distribution<T>
{
    #[inline]
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> Option<T> {
        if rng.gen::<bool>() {
            Some(rng.gen())
        } else {
            None
        }
    }
}

impl<T> Distribution<Wrapping<T>> for Standard
where Standard: Distribution<T>
{
    #[inline]
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> Wrapping<T> {
        Wrapping(rng.gen())
    }
}
