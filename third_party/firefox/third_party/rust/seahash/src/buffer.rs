//! A highly optimized version of SeaHash.

use std::slice;

use helper;

/// A SeaHash state.
#[derive(Clone)]
pub struct State {
    /// `a`
    a: u64,
    /// `b`
    b: u64,
    /// `c`
    c: u64,
    /// `d`
    d: u64,
    /// The number of written bytes.
    written: u64,
}

impl State {
    /// Create a new state vector with some initial values.
    pub fn new(a: u64, b: u64, c: u64, d: u64) -> State {
        State {
            a: a,
            b: b,
            c: c,
            d: d,
            written: 0,
        }
    }

    /// Hash a buffer with some seed.
    pub fn hash(buf: &[u8], (mut a, mut b, mut c, mut d): (u64, u64, u64, u64)) -> State {
        unsafe {

            let mut ptr = buf.as_ptr();
            let end_ptr = buf.as_ptr().offset(buf.len() as isize & !0x1F);

            while end_ptr > ptr {
                a ^= helper::read_u64(ptr);
                b ^= helper::read_u64(ptr.offset(8));
                c ^= helper::read_u64(ptr.offset(16));
                d ^= helper::read_u64(ptr.offset(24));

                ptr = ptr.offset(32);

                a = helper::diffuse(a);
                b = helper::diffuse(b);
                c = helper::diffuse(c);
                d = helper::diffuse(d);
            }

            let mut excessive = buf.len() as usize + buf.as_ptr() as usize - end_ptr as usize;
            match excessive {
                0 => {}
                1..=7 => {

                    a ^= helper::read_int(slice::from_raw_parts(ptr as *const u8, excessive));

                    a = helper::diffuse(a);
                }
                8 => {

                    a ^= helper::read_u64(ptr);

                    a = helper::diffuse(a);
                }
                9..=15 => {

                    a ^= helper::read_u64(ptr);

                    excessive = excessive - 8;
                    b ^= helper::read_int(slice::from_raw_parts(ptr.offset(8), excessive));

                    a = helper::diffuse(a);
                    b = helper::diffuse(b);
                }
                16 => {

                    a = helper::diffuse(a ^ helper::read_u64(ptr));
                    b = helper::diffuse(b ^ helper::read_u64(ptr.offset(8)));
                }
                17..=23 => {

                    a ^= helper::read_u64(ptr);
                    b ^= helper::read_u64(ptr.offset(8));

                    excessive = excessive - 16;
                    c ^= helper::read_int(slice::from_raw_parts(ptr.offset(16), excessive));

                    a = helper::diffuse(a);
                    b = helper::diffuse(b);
                    c = helper::diffuse(c);
                }
                24 => {

                    a ^= helper::read_u64(ptr);
                    b ^= helper::read_u64(ptr.offset(8));
                    c ^= helper::read_u64(ptr.offset(16));

                    a = helper::diffuse(a);
                    b = helper::diffuse(b);
                    c = helper::diffuse(c);
                }
                _ => {

                    a ^= helper::read_u64(ptr);
                    b ^= helper::read_u64(ptr.offset(8));
                    c ^= helper::read_u64(ptr.offset(16));

                    excessive = excessive - 24;
                    d ^= helper::read_int(slice::from_raw_parts(ptr.offset(24), excessive));

                    a = helper::diffuse(a);
                    b = helper::diffuse(b);
                    c = helper::diffuse(c);
                    d = helper::diffuse(d);
                }
            }
        }

        State {
            a: a,
            b: b,
            c: c,
            d: d,
            written: buf.len() as u64,
        }
    }

    /// Write another 64-bit integer into the state.
    pub fn push(&mut self, x: u64) {
        let a = helper::diffuse(self.a ^ x);

        self.a = self.b;
        self.b = self.c;
        self.c = self.d;
        self.d = a;

        self.written += 8;
    }

    /// Remove the most recently written 64-bit integer from the state.
    ///
    /// Given the value of the most recently written u64 `last`, remove it from the state.
    pub fn pop(&mut self, last: u64) {
        let d = helper::undiffuse(self.d) ^ last;

        self.d = self.c;
        self.c = self.b;
        self.b = self.a;
        self.a = d;

        self.written -= 8;
    }

    /// Finalize the state.
    #[inline]
    pub fn finalize(self) -> u64 {
        let State {
            written,
            mut a,
            b,
            mut c,
            d,
        } = self;

        a ^= b;
        c ^= d;
        a ^= c;
        a ^= written;

        helper::diffuse(a)
    }
}

/// Hash some buffer.
///
/// This is a highly optimized implementation of SeaHash. It implements numerous techniques to
/// improve performance:
///
/// - Register allocation: This makes a great deal out of making sure everything fits into
///   registers such that minimal memory accesses are needed. This works quite successfully on most
///   CPUs, and the only time it reads from memory is when it fetches the data of the buffer.
/// - Bulk reads: Like most other good hash functions, we read 8 bytes a time. This obviously
///   improves performance a lot
/// - Independent updates: We make sure very few statements next to each other depends on the
///   other. This means that almost always the CPU will be able to run the instructions in parallel.
/// - Loop unrolling: The hot loop is unrolled such that very little branches (one every 32 bytes)
///   are needed.
///
/// and more.
///
/// The seed of this hash function is prechosen.
pub fn hash(buf: &[u8]) -> u64 {
    hash_seeded(
        buf,
        0x16f11fe89b0d677c,
        0xb480a793d8e6c86c,
        0x6fe2e5aaf078ebc9,
        0x14f994a4c5259381,
    )
}

/// Hash some buffer according to a chosen seed.
///
/// The keys are expected to be chosen from a uniform distribution. The keys should be mutually
/// distinct to avoid issues with collisions if the lanes are permuted.
///
/// This is not secure, as [the key can be extracted with a bit of computational
/// work](https://github.com/ticki/tfs/issues/5), as such, it is recommended to have a fallback
/// hash function (adaptive hashing) in the case of hash flooding. It can be considered unbroken if
/// the output is not known (i.e. no malicious party has access to the raw values of the keys, only
/// a permutation thereof).), however I absolutely do not recommend using it for this. If you want
/// to be strict, this should only be used as a layer of obfuscation, such that the fallback (e.g.
/// SipHash) is harder to trigger.
///
/// In the future, I might strengthen the security if possible while having backward compatibility
/// with the default initialization vector.
pub fn hash_seeded(buf: &[u8], a: u64, b: u64, c: u64, d: u64) -> u64 {
    State::hash(buf, (a, b, c, d)).finalize()
}
