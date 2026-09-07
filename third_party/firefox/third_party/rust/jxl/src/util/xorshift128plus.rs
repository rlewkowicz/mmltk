// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

// (MIT-license)

pub struct Xorshift128Plus {
    s0: [u64; Self::N],
    s1: [u64; Self::N],
}

impl Xorshift128Plus {
    pub const N: usize = 8;

    pub fn new_with_seed(seed: u64) -> Self {
        let mut s0 = [0; Self::N];
        let mut s1 = [0; Self::N];

        s0[0] = Self::split_mix_64(seed + 0x9E3779B97F4A7C15);
        s1[0] = Self::split_mix_64(s0[0]);

        for i in 1..Self::N {
            s0[i] = Self::split_mix_64(s1[i - 1]);
            s1[i] = Self::split_mix_64(s0[i]);
        }

        Self { s0, s1 }
    }

    pub fn new_with_seeds(seed1: u32, seed2: u32, seed3: u32, seed4: u32) -> Self {
        let mut s0 = [0; Self::N];
        let mut s1 = [0; Self::N];

        s0[0] = Self::split_mix_64(
            (((seed1 as u64) << 32) + seed2 as u64).wrapping_add(0x9E3779B97F4A7C15),
        );
        s1[0] = Self::split_mix_64(
            (((seed3 as u64) << 32) + seed4 as u64).wrapping_add(0x9E3779B97F4A7C15),
        );
        for i in 1..Self::N {
            s0[i] = Self::split_mix_64(s0[i - 1]);
            s1[i] = Self::split_mix_64(s1[i - 1]);
        }

        Self { s0, s1 }
    }

    pub fn fill(&mut self, random_bits: &mut [u64; Self::N]) {
        for ((s0, s1), random_bits) in self
            .s0
            .iter_mut()
            .zip(self.s1.iter_mut())
            .zip(random_bits.iter_mut())
        {
            let mut new_s1 = *s0;
            *s0 = *s1;
            let bits = new_s1.wrapping_add(*s0); 
            new_s1 ^= new_s1 << 23;
            *random_bits = bits;
            new_s1 ^= *s0 ^ (new_s1 >> 18) ^ (*s0 >> 5);
            *s1 = new_s1;
        }
    }

    fn split_mix_64(mut z: u64) -> u64 {
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
        z ^ (z >> 31)
    }
}
