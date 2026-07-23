use super::braid::CRC32_LSB_POLY;

pub const fn crc32_combine(crc1: u32, crc2: u32, len2: u64) -> u32 {
    crc32_combine_op(crc1, crc2, crc32_combine_gen(len2))
}

#[inline(always)]
pub const fn crc32_combine_gen(len2: u64) -> u32 {
    x2nmodp(len2, 3)
}

#[inline(always)]
pub const fn crc32_combine_op(crc1: u32, crc2: u32, op: u32) -> u32 {
    multmodp(op, crc1) ^ crc2
}

const X2N_TABLE: [u32; 32] = [
    0x40000000, 0x20000000, 0x08000000, 0x00800000, 0x00008000, 0xedb88320, 0xb1e6b092, 0xa06a2517,
    0xed627dae, 0x88d14467, 0xd7bbfe6a, 0xec447f11, 0x8e7ea170, 0x6427800e, 0x4d47bae0, 0x09fe548f,
    0x83852d0f, 0x30362f1a, 0x7b5a9cc3, 0x31fec169, 0x9fec022a, 0x6c8dedc4, 0x15d6874d, 0x5fde7a4e,
    0xbad90e37, 0x2e4e5eef, 0x4eaba214, 0xa8a472c0, 0x429a969e, 0x148d302a, 0xc40ba6d0, 0xc4e22c3c,
];

const fn multmodp(a: u32, mut b: u32) -> u32 {
    let mut m = 1 << 31;
    let mut p = 0;

    loop {
        if (a & m) != 0 {
            p ^= b;
            if (a & (m - 1)) == 0 {
                break;
            }
        }
        m >>= 1;
        b = if (b & 1) != 0 {
            (b >> 1) ^ CRC32_LSB_POLY as u32
        } else {
            b >> 1
        };
    }

    p
}

const fn x2nmodp(mut n: u64, mut k: u32) -> u32 {
    let mut p: u32 = 1 << 31; 

    while n > 0 {
        if (n & 1) != 0 {
            p = multmodp(X2N_TABLE[k as usize & 31], p);
        }
        n >>= 1;
        k += 1;
    }

    p
}
