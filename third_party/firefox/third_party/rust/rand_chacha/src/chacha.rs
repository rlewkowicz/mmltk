// Copyright 2018 Developers of the Rand project.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! The ChaCha random number generator.

#[cfg(not(feature = "std"))] use core;
#[cfg(feature = "std")] use std as core;

use self::core::fmt;
use crate::guts::ChaCha;
use rand_core::block::{BlockRng, BlockRngCore};
use rand_core::{CryptoRng, Error, RngCore, SeedableRng};

#[cfg(feature = "serde1")] use serde::{Serialize, Deserialize, Serializer, Deserializer};

const BUF_BLOCKS: u8 = 4;
const BLOCK_WORDS: u8 = 16;

#[repr(transparent)]
pub struct Array64<T>([T; 64]);
impl<T> Default for Array64<T>
where T: Default
{
    #[rustfmt::skip]
    fn default() -> Self {
        Self([
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
            T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(), T::default(),
        ])
    }
}
impl<T> AsRef<[T]> for Array64<T> {
    fn as_ref(&self) -> &[T] {
        &self.0
    }
}
impl<T> AsMut<[T]> for Array64<T> {
    fn as_mut(&mut self) -> &mut [T] {
        &mut self.0
    }
}
impl<T> Clone for Array64<T>
where T: Copy + Default
{
    fn clone(&self) -> Self {
        let mut new = Self::default();
        new.0.copy_from_slice(&self.0);
        new
    }
}
impl<T> fmt::Debug for Array64<T> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "Array64 {{}}")
    }
}

macro_rules! chacha_impl {
    ($ChaChaXCore:ident, $ChaChaXRng:ident, $rounds:expr, $doc:expr, $abst:ident) => {
        #[doc=$doc]
        #[derive(Clone, PartialEq, Eq)]
        pub struct $ChaChaXCore {
            state: ChaCha,
        }

        impl fmt::Debug for $ChaChaXCore {
            fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
                write!(f, "ChaChaXCore {{}}")
            }
        }

        impl BlockRngCore for $ChaChaXCore {
            type Item = u32;
            type Results = Array64<u32>;
            #[inline]
            fn generate(&mut self, r: &mut Self::Results) {
                self.state.refill4($rounds, unsafe {
                    &mut *(&mut *r as *mut Array64<u32> as *mut [u8; 256])
                });
                for x in r.as_mut() {
                    *x = x.to_le();
                }
            }
        }

        impl SeedableRng for $ChaChaXCore {
            type Seed = [u8; 32];
            #[inline]
            fn from_seed(seed: Self::Seed) -> Self {
                $ChaChaXCore { state: ChaCha::new(&seed, &[0u8; 8]) }
            }
        }

        impl CryptoRng for $ChaChaXCore {}

        /// A cryptographically secure random number generator that uses the ChaCha algorithm.
        ///
        /// ChaCha is a stream cipher designed by Daniel J. Bernstein[^1], that we use as an RNG. It is
        /// an improved variant of the Salsa20 cipher family, which was selected as one of the "stream
        /// ciphers suitable for widespread adoption" by eSTREAM[^2].
        ///
        /// ChaCha uses add-rotate-xor (ARX) operations as its basis. These are safe against timing
        /// attacks, although that is mostly a concern for ciphers and not for RNGs. We provide a SIMD
        /// implementation to support high throughput on a variety of common hardware platforms.
        ///
        /// With the ChaCha algorithm it is possible to choose the number of rounds the core algorithm
        /// should run. The number of rounds is a tradeoff between performance and security, where 8
        /// rounds is the minimum potentially secure configuration, and 20 rounds is widely used as a
        /// conservative choice.
        ///
        /// We use a 64-bit counter and 64-bit stream identifier as in Bernstein's implementation[^1]
        /// except that we use a stream identifier in place of a nonce. A 64-bit counter over 64-byte
        /// (16 word) blocks allows 1 ZiB of output before cycling, and the stream identifier allows
        /// 2<sup>64</sup> unique streams of output per seed. Both counter and stream are initialized
        /// to zero but may be set via the `set_word_pos` and `set_stream` methods.
        ///
        /// The word layout is:
        ///
        /// ```text
        /// constant  constant  constant  constant
        /// seed      seed      seed      seed
        /// seed      seed      seed      seed
        /// counter   counter   stream_id stream_id
        /// ```
        ///
        /// This implementation uses an output buffer of sixteen `u32` words, and uses
        /// [`BlockRng`] to implement the [`RngCore`] methods.
        ///
        /// [^1]: D. J. Bernstein, [*ChaCha, a variant of Salsa20*](
        ///       https://cr.yp.to/chacha.html)
        ///
        /// [^2]: [eSTREAM: the ECRYPT Stream Cipher Project](
        ///       http://www.ecrypt.eu.org/stream/)
        #[derive(Clone, Debug)]
        pub struct $ChaChaXRng {
            rng: BlockRng<$ChaChaXCore>,
        }

        impl SeedableRng for $ChaChaXRng {
            type Seed = [u8; 32];
            #[inline]
            fn from_seed(seed: Self::Seed) -> Self {
                let core = $ChaChaXCore::from_seed(seed);
                Self {
                    rng: BlockRng::new(core),
                }
            }
        }

        impl RngCore for $ChaChaXRng {
            #[inline]
            fn next_u32(&mut self) -> u32 {
                self.rng.next_u32()
            }
            #[inline]
            fn next_u64(&mut self) -> u64 {
                self.rng.next_u64()
            }
            #[inline]
            fn fill_bytes(&mut self, bytes: &mut [u8]) {
                self.rng.fill_bytes(bytes)
            }
            #[inline]
            fn try_fill_bytes(&mut self, bytes: &mut [u8]) -> Result<(), Error> {
                self.rng.try_fill_bytes(bytes)
            }
        }

        impl $ChaChaXRng {

            /// Get the offset from the start of the stream, in 32-bit words.
            ///
            /// Since the generated blocks are 16 words (2<sup>4</sup>) long and the
            /// counter is 64-bits, the offset is a 68-bit number. Sub-word offsets are
            /// not supported, hence the result can simply be multiplied by 4 to get a
            /// byte-offset.
            #[inline]
            pub fn get_word_pos(&self) -> u128 {
                let buf_start_block = {
                    let buf_end_block = self.rng.core.state.get_block_pos();
                    u64::wrapping_sub(buf_end_block, BUF_BLOCKS.into())
                };
                let (buf_offset_blocks, block_offset_words) = {
                    let buf_offset_words = self.rng.index() as u64;
                    let blocks_part = buf_offset_words / u64::from(BLOCK_WORDS);
                    let words_part = buf_offset_words % u64::from(BLOCK_WORDS);
                    (blocks_part, words_part)
                };
                let pos_block = u64::wrapping_add(buf_start_block, buf_offset_blocks);
                let pos_block_words = u128::from(pos_block) * u128::from(BLOCK_WORDS);
                pos_block_words + u128::from(block_offset_words)
            }

            /// Set the offset from the start of the stream, in 32-bit words.
            ///
            /// As with `get_word_pos`, we use a 68-bit number. Since the generator
            /// simply cycles at the end of its period (1 ZiB), we ignore the upper
            /// 60 bits.
            #[inline]
            pub fn set_word_pos(&mut self, word_offset: u128) {
                let block = (word_offset / u128::from(BLOCK_WORDS)) as u64;
                self.rng
                    .core
                    .state
                    .set_block_pos(block);
                self.rng.generate_and_set((word_offset % u128::from(BLOCK_WORDS)) as usize);
            }

            /// Set the stream number.
            ///
            /// This is initialized to zero; 2<sup>64</sup> unique streams of output
            /// are available per seed/key.
            ///
            /// Note that in order to reproduce ChaCha output with a specific 64-bit
            /// nonce, one can convert that nonce to a `u64` in little-endian fashion
            /// and pass to this function. In theory a 96-bit nonce can be used by
            /// passing the last 64-bits to this function and using the first 32-bits as
            /// the most significant half of the 64-bit counter (which may be set
            /// indirectly via `set_word_pos`), but this is not directly supported.
            #[inline]
            pub fn set_stream(&mut self, stream: u64) {
                self.rng
                    .core
                    .state
                    .set_nonce(stream);
                if self.rng.index() != 64 {
                    let wp = self.get_word_pos();
                    self.set_word_pos(wp);
                }
            }

            /// Get the stream number.
            #[inline]
            pub fn get_stream(&self) -> u64 {
                self.rng
                    .core
                    .state
                    .get_nonce()
            }

            /// Get the seed.
            #[inline]
            pub fn get_seed(&self) -> [u8; 32] {
                self.rng
                    .core
                    .state
                    .get_seed()
            }
        }

        impl CryptoRng for $ChaChaXRng {}

        impl From<$ChaChaXCore> for $ChaChaXRng {
            fn from(core: $ChaChaXCore) -> Self {
                $ChaChaXRng {
                    rng: BlockRng::new(core),
                }
            }
        }

        impl PartialEq<$ChaChaXRng> for $ChaChaXRng {
            fn eq(&self, rhs: &$ChaChaXRng) -> bool {
                let a: $abst::$ChaChaXRng = self.into();
                let b: $abst::$ChaChaXRng = rhs.into();
                a == b
            }
        }
        impl Eq for $ChaChaXRng {}

        #[cfg(feature = "serde1")]
        impl Serialize for $ChaChaXRng {
            fn serialize<S>(&self, s: S) -> Result<S::Ok, S::Error>
            where S: Serializer {
                $abst::$ChaChaXRng::from(self).serialize(s)
            }
        }
        #[cfg(feature = "serde1")]
        impl<'de> Deserialize<'de> for $ChaChaXRng {
            fn deserialize<D>(d: D) -> Result<Self, D::Error> where D: Deserializer<'de> {
                $abst::$ChaChaXRng::deserialize(d).map(|x| Self::from(&x))
            }
        }

        mod $abst {
            #[cfg(feature = "serde1")] use serde::{Serialize, Deserialize};

            #[derive(Debug, PartialEq, Eq)]
#[cfg_attr(feature = "serde1", derive(Serialize, Deserialize),)]
pub(crate) struct $ChaChaXRng {
                seed: [u8; 32],
                stream: u64,
                word_pos: u128,
            }

            impl From<&super::$ChaChaXRng> for $ChaChaXRng {
                fn from(r: &super::$ChaChaXRng) -> Self {
                    Self {
                        seed: r.get_seed(),
                        stream: r.get_stream(),
                        word_pos: r.get_word_pos(),
                    }
                }
            }

            impl From<&$ChaChaXRng> for super::$ChaChaXRng {
                fn from(a: &$ChaChaXRng) -> Self {
                    use rand_core::SeedableRng;
                    let mut r = Self::from_seed(a.seed);
                    r.set_stream(a.stream);
                    r.set_word_pos(a.word_pos);
                    r
                }
            }
        }
    }
}

chacha_impl!(ChaCha20Core, ChaCha20Rng, 10, "ChaCha with 20 rounds", abstract20);
chacha_impl!(ChaCha12Core, ChaCha12Rng, 6, "ChaCha with 12 rounds", abstract12);
chacha_impl!(ChaCha8Core, ChaCha8Rng, 4, "ChaCha with 8 rounds", abstract8);
