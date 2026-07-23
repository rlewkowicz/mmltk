// Copyright © WHATWG (Apple, Google, Mozilla, Microsoft).
// Redistribution and use in source and binary forms, with or without
// 1. Redistributions of source code must retain the above copyright notice, this
// 2. Redistributions in binary form must reproduce the above copyright notice,
// 3. Neither the name of the copyright holder nor the names of its
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE

/// The PUA code points special-cased in the GB18030 encoder.
pub(crate) static GB18030_2022_OVERRIDE_PUA: [u16; 18] = [
    0xE78D, 0xE78E, 0xE78F, 0xE790, 0xE791, 0xE792, 0xE793, 0xE794, 0xE795, 0xE796, 0xE81E, 0xE826,
    0xE82B, 0xE82C, 0xE832, 0xE843, 0xE854, 0xE864,
];

/// The bytes corresponding to the PUA code points special-cased in the GB18030 encoder.
pub(crate) static GB18030_2022_OVERRIDE_BYTES: [[u8; 2]; 18] = [
    [0xA6, 0xD9],
    [0xA6, 0xDA],
    [0xA6, 0xDB],
    [0xA6, 0xDC],
    [0xA6, 0xDD],
    [0xA6, 0xDE],
    [0xA6, 0xDF],
    [0xA6, 0xEC],
    [0xA6, 0xED],
    [0xA6, 0xF3],
    [0xFE, 0x59],
    [0xFE, 0x61],
    [0xFE, 0x66],
    [0xFE, 0x67],
    [0xFE, 0x6D],
    [0xFE, 0x7E],
    [0xFE, 0x90],
    [0xFE, 0xA0],
];
