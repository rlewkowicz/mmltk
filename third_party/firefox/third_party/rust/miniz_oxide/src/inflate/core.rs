//! Streaming decompression functionality.

use super::*;
use crate::shared::{update_adler32, HUFFMAN_LENGTH_ORDER};
use ::core::cell::Cell;

use ::core::cmp;
use ::core::convert::TryInto;

use self::output_buffer::{InputWrapper, OutputBuffer};

#[cfg(feature = "serde")]
use crate::serde::big_array::BigArray;
#[cfg(feature = "serde")]
use serde::{Deserialize, Serialize};

pub const TINFL_LZ_DICT_SIZE: usize = 32_768;

/// A struct containing huffman code lengths and the huffman code tree used by the decompressor.
#[cfg_attr(not(feature = "rustc-dep-of-std"), derive(Clone))]
#[cfg_attr(feature = "serde", derive(Serialize, Deserialize))]
struct HuffmanTable {
    /// Fast lookup table for shorter huffman codes.
    ///
    /// See `HuffmanTable::fast_lookup`.
    #[cfg_attr(feature = "serde", serde(with = "BigArray"))]
    pub look_up: [i16; FAST_LOOKUP_SIZE as usize],
    /// Full huffman tree.
    ///
    /// Positive values are edge nodes/symbols, negative values are
    /// parent nodes/references to other nodes.
    #[cfg_attr(feature = "serde", serde(with = "BigArray"))]
    pub tree: [i16; MAX_HUFF_TREE_SIZE],
}

impl HuffmanTable {
    const fn new() -> HuffmanTable {
        HuffmanTable {
            look_up: [0; FAST_LOOKUP_SIZE as usize],
            tree: [0; MAX_HUFF_TREE_SIZE],
        }
    }

    /// Look for a symbol in the fast lookup table.
    /// The symbol is stored in the lower 9 bits, the length in the next 6.
    /// If the returned value is negative, the code wasn't found in the
    /// fast lookup table and the full tree has to be traversed to find the code.
    #[inline]
    fn fast_lookup(&self, bit_buf: BitBuffer) -> i16 {
        self.look_up[(bit_buf & BitBuffer::from(FAST_LOOKUP_SIZE - 1)) as usize]
    }

    /// Get the symbol and the code length from the huffman tree.
    #[inline]
    fn tree_lookup(&self, fast_symbol: i32, bit_buf: BitBuffer, mut code_len: u8) -> (i32, u32) {
        let mut symbol = fast_symbol;
        loop {
            let tree_index = (!symbol + ((bit_buf >> code_len) & 1) as i32) as usize;

            debug_assert!(tree_index < self.tree.len());
            symbol = i32::from(self.tree.get(tree_index).copied().unwrap_or(i16::MAX));
            code_len += 1;
            if symbol >= 0 {
                break;
            }
        }
        (symbol, u32::from(code_len))
    }

    #[inline]
    /// Look up a symbol and code length from the bits in the provided bit buffer.
    ///
    /// Returns Some(symbol, length) on success,
    /// None if the length is 0.
    ///
    /// It's possible we could avoid checking for 0 if we can guarantee a sane table.
    /// TODO: Check if a smaller type for code_len helps performance.
    fn lookup(&self, bit_buf: BitBuffer) -> (i32, u32) {
        let symbol = self.fast_lookup(bit_buf).into();
        if symbol >= 0 {
            let length = (symbol >> 9) as u32;
            (symbol, length)
        } else {
            self.tree_lookup(symbol, bit_buf, FAST_LOOKUP_BITS)
        }
    }
}

/// The number of huffman tables used.
const MAX_HUFF_TABLES: usize = 3;
/// The length of the first (literal/length) huffman table.
const MAX_HUFF_SYMBOLS_0: usize = 288;
/// The length of the second (distance) huffman table.
const MAX_HUFF_SYMBOLS_1: usize = 32;
/// The length of the last (huffman code length) huffman table.
const MAX_HUFF_SYMBOLS_2: usize = 19;
/// The maximum length of a code that can be looked up in the fast lookup table.
const FAST_LOOKUP_BITS: u8 = 10;
/// The size of the fast lookup table.
const FAST_LOOKUP_SIZE: u16 = 1 << FAST_LOOKUP_BITS;
const MAX_HUFF_TREE_SIZE: usize = MAX_HUFF_SYMBOLS_0 * 2;
const LITLEN_TABLE: usize = 0;
const DIST_TABLE: usize = 1;
const HUFFLEN_TABLE: usize = 2;
const LEN_CODES_SIZE: usize = 512;
const LEN_CODES_MASK: usize = LEN_CODES_SIZE - 1;

/// Flags to [`decompress()`] to control how inflation works.
///
/// These define bits for a bitmask argument.
pub mod inflate_flags {
    /// Should we try to parse a zlib header?
    ///
    /// If unset, the function will expect an RFC1951 deflate stream.  If set, it will expect a
    /// RFC1950 zlib wrapper around the deflate stream.
    pub const TINFL_FLAG_PARSE_ZLIB_HEADER: u32 = 1;

    /// There will be more input that hasn't been given to the decompressor yet.
    ///
    /// This is useful when you want to decompress what you have so far,
    /// even if you know there is probably more input that hasn't gotten here yet (_e.g._, over a
    /// network connection).  When [`decompress()`][super::decompress] reaches the end of the input
    /// without finding the end of the compressed stream, it will return
    /// [`TINFLStatus::NeedsMoreInput`][super::TINFLStatus::NeedsMoreInput] if this is set,
    /// indicating that you should get more data before calling again.  If not set, it will return
    /// [`TINFLStatus::FailedCannotMakeProgress`][super::TINFLStatus::FailedCannotMakeProgress]
    /// suggesting the stream is corrupt, since you claimed it was all there.
    pub const TINFL_FLAG_HAS_MORE_INPUT: u32 = 2;

    /// The output buffer should not wrap around.
    pub const TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: u32 = 4;

    /// Calculate the adler32 checksum of the output data even if we're not inflating a zlib stream.
    ///
    /// If [`TINFL_FLAG_IGNORE_ADLER32`] is specified, it will override this.
    ///
    /// NOTE: Enabling/disabling this between calls to decompress will result in an incorrect
    /// checksum.
    pub const TINFL_FLAG_COMPUTE_ADLER32: u32 = 8;

    /// Ignore adler32 checksum even if we are inflating a zlib stream.
    ///
    /// Overrides [`TINFL_FLAG_COMPUTE_ADLER32`] if both are enabled.
    ///
    /// NOTE: This flag does not exist in miniz as it does not support this and is a
    /// custom addition for miniz_oxide.
    ///
    /// NOTE: Should not be changed from enabled to disabled after decompression has started,
    /// this will result in checksum failure (outside the unlikely event where the checksum happens
    /// to match anyway).
    pub const TINFL_FLAG_IGNORE_ADLER32: u32 = 64;

    /// Return [`TINFLStatus::BlockBoundary`][super::TINFLStatus::BlockBoundary]
    /// on reaching the boundary between deflate blocks. Calling [`decompress()`][super::decompress]
    /// again will resume decompression of the next block.
    #[cfg(feature = "block-boundary")]
    pub const TINFL_FLAG_STOP_ON_BLOCK_BOUNDARY: u32 = 128;
}

use self::inflate_flags::*;

const MIN_TABLE_SIZES: [u16; 3] = [257, 1, 4];

#[cfg(target_pointer_width = "64")]
type BitBuffer = u64;

#[cfg(not(target_pointer_width = "64"))]
type BitBuffer = u32;


/// Minimal data representing the [`DecompressorOxide`] state when it is between deflate blocks
/// (i.e. [`decompress()`] has returned [`TINFLStatus::BlockBoundary`]).
/// This can be serialized along with the last 32KiB of the output buffer, then passed to
/// [`DecompressorOxide::from_block_boundary_state()`] to resume decompression from the same point.
///
/// The Zlib/Adler32 fields can be ignored if you aren't using those features
/// ([`TINFL_FLAG_PARSE_ZLIB_HEADER`], [`TINFL_FLAG_COMPUTE_ADLER32`]).
/// When deserializing, you can reconstruct `bit_buf` from the previous byte in the input file
/// (if you still have access to it), so `num_bits` is the only field that is always required.
#[derive(Clone)]
#[cfg(feature = "block-boundary")]
#[cfg_attr(feature = "serde", derive(Serialize, Deserialize))]
pub struct BlockBoundaryState {
    /// The number of bits from the last byte of input consumed,
    /// that are needed for decoding the next deflate block.
    /// Value is in range `0..=7`
    pub num_bits: u8,

    /// The `num_bits` MSBs from the last byte of input consumed,
    /// that are needed for decoding the next deflate block.
    /// Stored in the LSBs of this field.
    pub bit_buf: u8,

    /// Zlib CMF
    pub z_header0: u32,
    /// Zlib FLG
    pub z_header1: u32,
    /// Adler32 checksum of the data decompressed so far
    pub check_adler32: u32,
}

#[cfg(feature = "block-boundary")]
impl Default for BlockBoundaryState {
    fn default() -> Self {
        BlockBoundaryState {
            num_bits: 0,
            bit_buf: 0,
            z_header0: 0,
            z_header1: 0,
            check_adler32: 1,
        }
    }
}

/// Main decompression struct.
///
#[cfg_attr(not(feature = "rustc-dep-of-std"), derive(Clone))]
#[cfg_attr(feature = "serde", derive(Serialize, Deserialize))]
pub struct DecompressorOxide {
    /// Current state of the decompressor.
    state: core::State,
    /// Number of bits in the bit buffer.
    num_bits: u32,
    /// Zlib CMF
    z_header0: u32,
    /// Zlib FLG
    z_header1: u32,
    /// Adler32 checksum from the zlib header.
    z_adler32: u32,
    /// 1 if the current block is the last block, 0 otherwise.
    finish: u8,
    /// The type of the current block.
    /// or if in a dynamic block, which huffman table we are currently
    block_type: u8,
    /// 1 if the adler32 value should be checked.
    check_adler32: u32,
    /// Last match distance.
    dist: u32,
    /// Variable used for match length, symbols, and a number of other things.
    counter: u32,
    /// Number of extra bits for the last length or distance code.
    num_extra: u8,
    /// Number of entries in each huffman table.
    table_sizes: [u16; MAX_HUFF_TABLES],
    /// Buffer of input data.
    bit_buf: BitBuffer,
    /// Huffman tables.
    tables: [HuffmanTable; MAX_HUFF_TABLES],

    #[cfg_attr(feature = "serde", serde(with = "BigArray"))]
    code_size_literal: [u8; MAX_HUFF_SYMBOLS_0],
    code_size_dist: [u8; MAX_HUFF_SYMBOLS_1],
    code_size_huffman: [u8; MAX_HUFF_SYMBOLS_2],
    /// Raw block header.
    raw_header: [u8; 4],
    /// Huffman length codes.
    #[cfg_attr(feature = "serde", serde(with = "BigArray"))]
    len_codes: [u8; LEN_CODES_SIZE],
}

impl DecompressorOxide {
    /// Create a new tinfl_decompressor with all fields set to 0.
    pub fn new() -> DecompressorOxide {
        DecompressorOxide::default()
    }

    /// Set the current state to `Start`.
    #[inline]
    pub fn init(&mut self) {
        self.state = core::State::Start;
    }

    /// Returns the adler32 checksum of the currently decompressed data.
    /// Note: Will return Some(1) if decompressing zlib but ignoring adler32.
    #[inline]
    #[cfg(not(feature = "rustc-dep-of-std"))]
    pub fn adler32(&self) -> Option<u32> {
        if self.state != State::Start && !self.state.is_failure() && self.z_header0 != 0 {
            Some(self.check_adler32)
        } else {
            None
        }
    }

    /// Returns the adler32 that was read from the zlib header if it exists.
    #[inline]
    #[cfg(not(feature = "rustc-dep-of-std"))]
    pub fn adler32_header(&self) -> Option<u32> {
        if self.state != State::Start && self.state != State::BadZlibHeader && self.z_header0 != 0 {
            Some(self.z_adler32)
        } else {
            None
        }
    }



    /// Returns the current [`BlockBoundaryState`]. Should only be called when
    /// [`decompress()`] has returned [`TINFLStatus::BlockBoundary`];
    /// otherwise this will return `None`.
    #[cfg(feature = "block-boundary")]
    pub fn block_boundary_state(&self) -> Option<BlockBoundaryState> {
        if self.state == core::State::ReadBlockHeader {
            assert!(self.num_bits < 8);

            Some(BlockBoundaryState {
                num_bits: self.num_bits as u8,
                bit_buf: self.bit_buf as u8,
                z_header0: self.z_header0,
                z_header1: self.z_header1,
                check_adler32: self.check_adler32,
            })
        } else {
            None
        }
    }

    /// Creates a new `DecompressorOxide` from the state returned by
    /// `block_boundary_state()`.
    ///
    /// When calling [`decompress()`], the 32KiB of `out` preceding `out_pos` must be
    /// initialized with the same data that it contained when `block_boundary_state()`
    /// was called.
    #[cfg(feature = "block-boundary")]
    pub fn from_block_boundary_state(st: &BlockBoundaryState) -> Self {
        DecompressorOxide {
            state: core::State::ReadBlockHeader,
            num_bits: st.num_bits as u32,
            bit_buf: st.bit_buf as BitBuffer,
            z_header0: st.z_header0,
            z_header1: st.z_header1,
            z_adler32: 1,
            check_adler32: st.check_adler32,
            ..DecompressorOxide::default()
        }
    }
}

impl Default for DecompressorOxide {
    /// Create a new tinfl_decompressor with all fields set to 0.
    #[inline(always)]
    fn default() -> Self {
        DecompressorOxide {
            state: core::State::Start,
            num_bits: 0,
            z_header0: 0,
            z_header1: 0,
            z_adler32: 0,
            finish: 0,
            block_type: 0,
            check_adler32: 0,
            dist: 0,
            counter: 0,
            num_extra: 0,
            table_sizes: [0; MAX_HUFF_TABLES],
            bit_buf: 0,
            tables: [
                HuffmanTable::new(),
                HuffmanTable::new(),
                HuffmanTable::new(),
            ],
            code_size_literal: [0; MAX_HUFF_SYMBOLS_0],
            code_size_dist: [0; MAX_HUFF_SYMBOLS_1],
            code_size_huffman: [0; MAX_HUFF_SYMBOLS_2],
            raw_header: [0; 4],
            len_codes: [0; LEN_CODES_SIZE],
        }
    }
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
#[cfg_attr(feature = "serde", derive(Serialize, Deserialize))]
#[non_exhaustive]
enum State {
    Start = 0,
    ReadZlibCmf,
    ReadZlibFlg,
    ReadBlockHeader,
    BlockTypeNoCompression,
    RawHeader,
    RawMemcpy1,
    RawMemcpy2,
    ReadTableSizes,
    ReadHufflenTableCodeSize,
    ReadLitlenDistTablesCodeSize,
    ReadExtraBitsCodeSize,
    DecodeLitlen,
    WriteSymbol,
    ReadExtraBitsLitlen,
    DecodeDistance,
    ReadExtraBitsDistance,
    RawReadFirstByte,
    RawStoreFirstByte,
    WriteLenBytesToEnd,
    BlockDone,
    HuffDecodeOuterLoop1,
    HuffDecodeOuterLoop2,
    ReadAdler32,

    DoneForever,

    BlockTypeUnexpected,
    BadCodeSizeSum,
    BadDistOrLiteralTableLength,
    BadTotalSymbols,
    BadZlibHeader,
    DistanceOutOfBounds,
    BadRawLength,
    BadCodeSizeDistPrevLookup,
    InvalidLitlen,
    InvalidDist,
}

impl State {
    #[cfg(not(feature = "rustc-dep-of-std"))]
    const fn is_failure(self) -> bool {
        matches!(
            self,
            BlockTypeUnexpected
                | BadCodeSizeSum
                | BadDistOrLiteralTableLength
                | BadTotalSymbols
                | BadZlibHeader
                | DistanceOutOfBounds
                | BadRawLength
                | BadCodeSizeDistPrevLookup
                | InvalidLitlen
                | InvalidDist
        )
    }

    #[inline]
    fn begin(&mut self, new_state: State) {
        *self = new_state;
    }
}

use self::State::*;

/// Base length for each length code.
///
/// The base is used together with the value of the extra bits to decode the actual
/// length/distance values in a match.
#[rustfmt::skip]
const LENGTH_BASE: [u16; 32] = [
    3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,  23,  27,  31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 512, 512, 512
];

/// Number of extra bits for each length code.
#[rustfmt::skip]
const LENGTH_EXTRA: [u8; 32] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0, 0
];

/// Base length for each distance code.
#[rustfmt::skip]
const DIST_BASE: [u16; 30] = [
    1,    2,    3,    4,    5,    7,      9,      13,     17,     25,    33,
    49,   65,   97,   129,  193,  257,    385,    513,    769,    1025,  1537,
    2049, 3073, 4097, 6145, 8193, 12_289, 16_385, 24_577
];

/// Get the number of extra bits used for a distance code.
/// (Code numbers above `NUM_DISTANCE_CODES` will give some garbage
/// value.)
#[inline(always)]
const fn num_extra_bits_for_distance_code(code: u8) -> u8 {
    let c = code >> 1;
    c.saturating_sub(1)
}

/// The mask used when indexing the base/extra arrays.
const BASE_EXTRA_MASK: usize = 32 - 1;

/// Read an le u16 value from the slice iterator.
///
/// # Panics
/// Panics if there are less than two bytes left.
#[inline]
fn read_u16_le(iter: &mut InputWrapper) -> u16 {
    let ret = {
        let two_bytes = iter.as_slice()[..2].try_into().unwrap_or_default();
        u16::from_le_bytes(two_bytes)
    };
    iter.advance(2);
    ret
}

/// Ensure that there is data in the bit buffer.
///
/// On 64-bit platform, we use a 64-bit value so this will
/// result in there being at least 32 bits in the bit buffer.
/// This function assumes that there is at least 4 bytes left in the input buffer.
#[inline(always)]
#[cfg(target_pointer_width = "64")]
fn fill_bit_buffer(l: &mut LocalVars, in_iter: &mut InputWrapper) {
    if l.num_bits < 30 {
        l.bit_buf |= BitBuffer::from(in_iter.read_u32_le()) << l.num_bits;
        l.num_bits += 32;
    }
}

/// Same as previous, but for non-64-bit platforms.
/// Ensures at least 16 bits are present, requires at least 2 bytes in the in buffer.
#[inline(always)]
#[cfg(not(target_pointer_width = "64"))]
fn fill_bit_buffer(l: &mut LocalVars, in_iter: &mut InputWrapper) {
    if l.num_bits < 15 {
        l.bit_buf |= BitBuffer::from(read_u16_le(in_iter)) << l.num_bits;
        l.num_bits += 16;
    }
}

/// Check that the zlib header is correct and that there is enough space in the buffer
/// for the window size specified in the header.
///
/// See https://tools.ietf.org/html/rfc1950
#[inline]
const fn validate_zlib_header(cmf: u32, flg: u32, flags: u32, mask: usize) -> Action {
    let mut failed =
        (((cmf * 256) + flg) % 31 != 0) ||
        ((flg & 0b0010_0000) != 0) ||
        ((cmf & 15) != 8);

    let window_size = 1 << ((cmf >> 4) + 8);
    if (flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) == 0 {
        failed |= (mask + 1) < window_size;
    }

    failed |= window_size > 32_768;

    if failed {
        Action::Jump(BadZlibHeader)
    } else {
        Action::Jump(ReadBlockHeader)
    }
}

enum Action {
    None,
    Jump(State),
    End(TINFLStatus),
}

/// Try to decode the next huffman code, and puts it in the counter field of the decompressor
/// if successful.
///
/// # Returns
/// The specified action returned from `f` on success,
/// `Action::End` if there are not enough data left to decode a symbol.
fn decode_huffman_code<F>(
    r: &mut DecompressorOxide,
    l: &mut LocalVars,
    table: usize,
    flags: u32,
    in_iter: &mut InputWrapper,
    f: F,
) -> Action
where
    F: FnOnce(&mut DecompressorOxide, &mut LocalVars, i32) -> Action,
{
    if l.num_bits < 15 {
        if in_iter.bytes_left() < 2 {
            loop {
                let mut temp = i32::from(r.tables[table].fast_lookup(l.bit_buf));
                if temp >= 0 {
                    let code_len = (temp >> 9) as u32;
                    if (code_len != 0) && (l.num_bits >= code_len) {
                        break;
                    }
                } else if l.num_bits > FAST_LOOKUP_BITS.into() {
                    let mut code_len = u32::from(FAST_LOOKUP_BITS);
                    loop {
                        temp = i32::from(
                            r.tables[table].tree
                                [(!temp + ((l.bit_buf >> code_len) & 1) as i32) as usize],
                        );
                        code_len += 1;
                        if temp >= 0 || l.num_bits < code_len + 1 {
                            break;
                        }
                    }
                    if temp >= 0 {
                        break;
                    }
                }

                let mut byte = 0;
                if let a @ Action::End(_) = read_byte(in_iter, flags, |b| {
                    byte = b;
                    Action::None
                }) {
                    return a;
                };

                l.bit_buf |= BitBuffer::from(byte) << l.num_bits;
                l.num_bits += 8;

                if l.num_bits >= 15 {
                    break;
                }
            }
        } else {
            l.bit_buf |= BitBuffer::from(read_u16_le(in_iter)) << l.num_bits;
            l.num_bits += 16;
        }
    }

    let mut symbol = i32::from(r.tables[table].fast_lookup(l.bit_buf));
    let code_len;
    if symbol >= 0 {
        code_len = (symbol >> 9) as u32;
        symbol &= 511;
    } else {
        let res = r.tables[table].tree_lookup(symbol, l.bit_buf, FAST_LOOKUP_BITS);
        symbol = res.0;
        code_len = res.1;
    };

    l.bit_buf >>= code_len;
    l.num_bits -= code_len;
    f(r, l, symbol)
}

/// Try to read one byte from `in_iter` and call `f` with the read byte as an argument,
/// returning the result.
/// If reading fails, `Action::End is returned`
#[inline]
fn read_byte<F>(in_iter: &mut InputWrapper, flags: u32, f: F) -> Action
where
    F: FnOnce(u8) -> Action,
{
    match in_iter.read_byte() {
        None => end_of_input(flags),
        Some(byte) => f(byte),
    }
}

/// Try to read `amount` number of bits from `in_iter` and call the function `f` with the bits as an
/// an argument after reading, returning the result of that function, or `Action::End` if there are
/// not enough bytes left.
#[inline]
#[allow(clippy::while_immutable_condition)]
fn read_bits<F>(
    l: &mut LocalVars,
    amount: u32,
    in_iter: &mut InputWrapper,
    flags: u32,
    f: F,
) -> Action
where
    F: FnOnce(&mut LocalVars, BitBuffer) -> Action,
{
    while l.num_bits < amount {
        let action = read_byte(in_iter, flags, |byte| {
            l.bit_buf |= BitBuffer::from(byte) << l.num_bits;
            l.num_bits += 8;
            Action::None
        });

        if !matches!(action, Action::None) {
            return action;
        }
    }

    let bits = l.bit_buf & ((1 << amount) - 1);
    l.bit_buf >>= amount;
    l.num_bits -= amount;
    f(l, bits)
}

#[inline]
fn pad_to_bytes<F>(l: &mut LocalVars, in_iter: &mut InputWrapper, flags: u32, f: F) -> Action
where
    F: FnOnce(&mut LocalVars) -> Action,
{
    let num_bits = l.num_bits & 7;
    read_bits(l, num_bits, in_iter, flags, |l, _| f(l))
}

#[inline]
const fn end_of_input(flags: u32) -> Action {
    Action::End(if flags & TINFL_FLAG_HAS_MORE_INPUT != 0 {
        TINFLStatus::NeedsMoreInput
    } else {
        TINFLStatus::FailedCannotMakeProgress
    })
}

#[inline]
fn undo_bytes(l: &mut LocalVars, max: u32) -> u32 {
    let res = cmp::min(l.num_bits >> 3, max);
    l.num_bits -= res << 3;
    res
}

fn start_static_table(r: &mut DecompressorOxide) {
    r.table_sizes[LITLEN_TABLE] = 288;
    r.table_sizes[DIST_TABLE] = 32;
    r.code_size_literal[0..144].fill(8);
    r.code_size_literal[144..256].fill(9);
    r.code_size_literal[256..280].fill(7);
    r.code_size_literal[280..288].fill(8);
    r.code_size_dist[0..32].fill(5);
}

#[cfg(any(feature = "rustc-dep-of-std", not(feature = "with-alloc"), target_arch = "aarch64", target_arch = "arm64ec", target_arch = "loongarch64"))]
#[inline]
const fn reverse_bits(n: u16) -> u16 {


    n.reverse_bits()
}

#[cfg(all(not(any(feature = "rustc-dep-of-std", target_arch = "aarch64", target_arch = "arm64ec", target_arch = "loongarch64")), feature = "with-alloc"))]
fn reverse_bits(n: u16) -> u16 {
    static REVERSED_BITS_LOOKUP: [u16; 512] = {
        let mut table = [0; 512];

        let mut i = 0;
        while i < 512 {
            table[i] = (i as u16).reverse_bits();
            i += 1;
        }

        table
    };

    REVERSED_BITS_LOOKUP[n as usize]
}

fn init_tree(r: &mut DecompressorOxide, l: &mut LocalVars) -> Option<Action> {
    loop {
        let bt = r.block_type as usize;

        let code_sizes = match bt {
            LITLEN_TABLE => &mut r.code_size_literal[..],
            DIST_TABLE => &mut r.code_size_dist,
            HUFFLEN_TABLE => &mut r.code_size_huffman,
            _ => return None,
        };
        let table = &mut r.tables[bt];

        let mut total_symbols = [0u16; 16];
        let mut next_code = [0u32; 17];
        const INVALID_CODE: i16 = (1 << 9) | 286;
        table.look_up.fill(INVALID_CODE);
        if bt != HUFFLEN_TABLE {
            table.tree.fill(0);
        }

        let table_size = r.table_sizes[bt] as usize;
        if table_size > code_sizes.len() {
            return None;
        }

        for &code_size in &code_sizes[..table_size] {
            let cs = code_size as usize;
            if cs >= total_symbols.len() {
                return None;
            }
            total_symbols[cs] += 1;
        }

        let mut used_symbols = 0;
        let mut total = 0u32;
        for (&ts, next) in total_symbols.iter().zip(next_code[1..].iter_mut()).skip(1) {
            used_symbols += ts;
            total += u32::from(ts);
            total <<= 1;
            *next = total;
        }

        if total != 65_536 && (used_symbols > 1 || bt == HUFFLEN_TABLE) {
            return Some(Action::Jump(BadTotalSymbols));
        }

        let mut tree_next = -1;
        for symbol_index in 0..table_size {
            let code_size = code_sizes[symbol_index] & 15;
            if code_size == 0 {
                continue;
            }

            let cur_code = next_code[code_size as usize];
            next_code[code_size as usize] += 1;

            let n = (cur_code & (u32::MAX >> (32 - code_size))) as u16;

            let mut rev_code = if n < 512 {
                reverse_bits(n)
            } else {
                n.reverse_bits()
            } >> (16 - code_size);

            if code_size <= FAST_LOOKUP_BITS {
                let k = (i16::from(code_size) << 9) | symbol_index as i16;
                while rev_code < FAST_LOOKUP_SIZE {
                    table.look_up[rev_code as usize] = k;
                    rev_code += 1 << code_size;
                }
                continue;
            }

            let mut tree_cur = table.look_up[(rev_code & (FAST_LOOKUP_SIZE - 1)) as usize];
            if tree_cur == INVALID_CODE {
                table.look_up[(rev_code & (FAST_LOOKUP_SIZE - 1)) as usize] = tree_next;
                tree_cur = tree_next;
                tree_next -= 2;
            }

            rev_code >>= FAST_LOOKUP_BITS - 1;
            for _ in FAST_LOOKUP_BITS + 1..code_size {
                rev_code >>= 1;
                tree_cur -= (rev_code & 1) as i16;
                let tree_index = (-tree_cur - 1) as usize;
                if tree_index >= table.tree.len() {
                    return None;
                }
                if table.tree[tree_index] == 0 {
                    table.tree[tree_index] = tree_next;
                    tree_cur = tree_next;
                    tree_next -= 2;
                } else {
                    tree_cur = table.tree[tree_index];
                }
            }

            rev_code >>= 1;
            tree_cur -= (rev_code & 1) as i16;
            let tree_index = (-tree_cur - 1) as usize;
            if tree_index >= table.tree.len() {
                return None;
            }
            table.tree[tree_index] = symbol_index as i16;
        }

        if r.block_type == HUFFLEN_TABLE as u8 {
            l.counter = 0;
            return Some(Action::Jump(ReadLitlenDistTablesCodeSize));
        }

        if r.block_type == LITLEN_TABLE as u8 {
            break;
        }
        r.block_type -= 1;
    }

    l.counter = 0;

    Some(Action::Jump(DecodeLitlen))
}

// As Rust doesn't have fallthrough on matches, we have to return to the match statement
macro_rules! generate_state {
    ($state: ident, $state_machine: tt, $f: expr) => {
        loop {
            match $f {
                Action::None => continue,
                Action::Jump(new_state) => {
                    $state = new_state;
                    continue $state_machine;
                },
                Action::End(result) => break $state_machine result,
            }
        }
    };
}

#[derive(Copy, Clone)]
struct LocalVars {
    pub bit_buf: BitBuffer,
    pub num_bits: u32,
    pub dist: u32,
    pub counter: u32,
    pub num_extra: u8,
}

#[inline]
fn transfer(
    out_slice: &mut [u8],
    mut source_pos: usize,
    mut out_pos: usize,
    match_len: usize,
    out_buf_size_mask: usize,
) {
    let source_diff = if source_pos > out_pos {
        source_pos - out_pos
    } else {
        out_pos - source_pos
    };

    let not_wrapping = (out_buf_size_mask == usize::MAX)
        || ((source_pos + match_len).wrapping_sub(3) < out_slice.len());

    let end_pos = ((match_len >> 2) * 4) + out_pos;
    if not_wrapping && source_diff == 1 && out_pos > source_pos {
        let end = (match_len >> 2) * 4 + out_pos;
        let init = out_slice[out_pos - 1];
        out_slice[out_pos..end].fill(init);
        out_pos = end;
        source_pos = end - 1;
    } else if not_wrapping && out_pos > source_pos && (out_pos - source_pos >= 4) {
        let end_pos = cmp::min(end_pos, out_slice.len().saturating_sub(3));
        while out_pos < end_pos {
            out_slice.copy_within(source_pos..=source_pos + 3, out_pos);
            source_pos += 4;
            out_pos += 4;
        }
    } else {
        let end_pos = cmp::min(end_pos, out_slice.len().saturating_sub(3));
        while out_pos < end_pos {
            assert!(out_pos + 3 < out_slice.len());
            assert!((source_pos + 3) & out_buf_size_mask < out_slice.len());

            out_slice[out_pos] = out_slice[source_pos & out_buf_size_mask];
            out_slice[out_pos + 1] = out_slice[(source_pos + 1) & out_buf_size_mask];
            out_slice[out_pos + 2] = out_slice[(source_pos + 2) & out_buf_size_mask];
            out_slice[out_pos + 3] = out_slice[(source_pos + 3) & out_buf_size_mask];
            source_pos += 4;
            out_pos += 4;
        }
    }

    match match_len & 3 {
        0 => (),
        1 => out_slice[out_pos] = out_slice[source_pos & out_buf_size_mask],
        2 => {
            assert!(out_pos + 1 < out_slice.len());
            assert!((source_pos + 1) & out_buf_size_mask < out_slice.len());
            out_slice[out_pos] = out_slice[source_pos & out_buf_size_mask];
            out_slice[out_pos + 1] = out_slice[(source_pos + 1) & out_buf_size_mask];
        }
        3 => {
            assert!(out_pos + 2 < out_slice.len());
            assert!((source_pos + 2) & out_buf_size_mask < out_slice.len());
            out_slice[out_pos] = out_slice[source_pos & out_buf_size_mask];
            out_slice[out_pos + 1] = out_slice[(source_pos + 1) & out_buf_size_mask];
            out_slice[out_pos + 2] = out_slice[(source_pos + 2) & out_buf_size_mask];
        }
        _ => unreachable!(),
    }
}

/// Presumes that there is at least match_len bytes in output left.
#[inline]
fn apply_match(
    out_slice: &mut [u8],
    out_pos: usize,
    dist: usize,
    match_len: usize,
    out_buf_size_mask: usize,
) {
    debug_assert!(out_pos.checked_add(match_len).unwrap() <= out_slice.len());

    let source_pos = out_pos.wrapping_sub(dist) & out_buf_size_mask;

    if match_len == 3 {
        let out_slice = Cell::from_mut(out_slice).as_slice_of_cells();
        if let Some(dst) = out_slice.get(out_pos..out_pos + 3) {
            let src = out_slice
                .get(source_pos)
                .zip(out_slice.get((source_pos + 1) & out_buf_size_mask))
                .zip(out_slice.get((source_pos + 2) & out_buf_size_mask));
            if let Some(((a, b), c)) = src {
                dst[0].set(a.get());
                dst[1].set(b.get());
                dst[2].set(c.get());
            }
        }
        return;
    }

    if cfg!(not(any(target_arch = "x86", target_arch = "x86_64"))) {
        transfer(out_slice, source_pos, out_pos, match_len, out_buf_size_mask);
        return;
    }

    if source_pos >= out_pos && (source_pos - out_pos) < match_len {
        transfer(out_slice, source_pos, out_pos, match_len, out_buf_size_mask);
    } else if match_len <= dist && source_pos + match_len < out_slice.len() {
        if source_pos < out_pos {
            let (from_slice, to_slice) = out_slice.split_at_mut(out_pos);
            to_slice[..match_len].copy_from_slice(&from_slice[source_pos..source_pos + match_len]);
        } else {
            let (to_slice, from_slice) = out_slice.split_at_mut(source_pos);
            to_slice[out_pos..out_pos + match_len].copy_from_slice(&from_slice[..match_len]);
        }
    } else {
        transfer(out_slice, source_pos, out_pos, match_len, out_buf_size_mask);
    }
}

/// Fast inner decompression loop which is run  while there is at least
/// 259 bytes left in the output buffer, and at least 6 bytes left in the input buffer
/// (The maximum one match would need + 1).
///
/// This was inspired by a similar optimization in zlib, which uses this info to do
/// faster unchecked copies of multiple bytes at a time.
/// Currently we don't do this here, but this function does avoid having to jump through the
/// big match loop on each state change(as rust does not have fallthrough or gotos at the moment),
/// and already improves decompression speed a fair bit.
fn decompress_fast(
    r: &mut DecompressorOxide,
    in_iter: &mut InputWrapper,
    out_buf: &mut OutputBuffer,
    flags: u32,
    local_vars: &mut LocalVars,
    out_buf_size_mask: usize,
) -> (TINFLStatus, State) {
    let mut l = *local_vars;
    let mut state;

    let status: TINFLStatus = 'o: loop {
        state = State::DecodeLitlen;
        loop {
            if out_buf.bytes_left() < 259 || in_iter.bytes_left() < 14 {
                state = State::DecodeLitlen;
                break 'o TINFLStatus::Done;
            }

            fill_bit_buffer(&mut l, in_iter);

            let (symbol, code_len) = r.tables[LITLEN_TABLE].lookup(l.bit_buf);
            l.counter = symbol as u32;
            l.bit_buf >>= code_len;
            l.num_bits -= code_len;

            if (l.counter & 256) != 0 {
                break;
            } else {
                if cfg!(not(target_pointer_width = "64")) {
                    fill_bit_buffer(&mut l, in_iter);
                }

                let (symbol, code_len) = r.tables[LITLEN_TABLE].lookup(l.bit_buf);
                l.bit_buf >>= code_len;
                l.num_bits -= code_len;
                out_buf.write_byte(l.counter as u8);
                if (symbol & 256) != 0 {
                    l.counter = symbol as u32;
                    break;
                } else {
                    out_buf.write_byte(symbol as u8);
                }
            }
        }

        l.counter &= 511;
        if l.counter == 256 {
            state.begin(BlockDone);
            break 'o TINFLStatus::Done;
        } else if l.counter > 285 {
            state.begin(InvalidLitlen);
            break 'o TINFLStatus::Failed;
        } else {
            l.num_extra = LENGTH_EXTRA[(l.counter - 257) as usize & BASE_EXTRA_MASK];
            l.counter = u32::from(LENGTH_BASE[(l.counter - 257) as usize & BASE_EXTRA_MASK]);

            fill_bit_buffer(&mut l, in_iter);
            if l.num_extra != 0 {
                let extra_bits = l.bit_buf & ((1 << l.num_extra) - 1);
                l.bit_buf >>= l.num_extra;
                l.num_bits -= u32::from(l.num_extra);
                l.counter += extra_bits as u32;
            }


            if cfg!(not(target_pointer_width = "64")) {
                fill_bit_buffer(&mut l, in_iter);
            }

            let (mut symbol, code_len) = r.tables[DIST_TABLE].lookup(l.bit_buf);
            symbol &= 511;
            l.bit_buf >>= code_len;
            l.num_bits -= code_len;
            if symbol > 29 {
                state.begin(InvalidDist);
                break 'o TINFLStatus::Failed;
            }

            l.num_extra = num_extra_bits_for_distance_code(symbol as u8);
            l.dist = u32::from(DIST_BASE[symbol as usize]);

            if l.num_extra != 0 {
                fill_bit_buffer(&mut l, in_iter);
                let extra_bits = l.bit_buf & ((1 << l.num_extra) - 1);
                l.bit_buf >>= l.num_extra;
                l.num_bits -= u32::from(l.num_extra);
                l.dist += extra_bits as u32;
            }

            let position = out_buf.position();
            if (l.dist as usize > out_buf.position()
                && (flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF != 0))
                || (l.dist as usize > out_buf.get_ref().len())
            {
                state.begin(DistanceOutOfBounds);
                break TINFLStatus::Failed;
            }

            apply_match(
                out_buf.get_mut(),
                position,
                l.dist as usize,
                l.counter as usize,
                out_buf_size_mask,
            );

            out_buf.set_position(position + l.counter as usize);
        }
    };

    *local_vars = l;
    (status, state)
}

/// Main decompression function. Keeps decompressing data from `in_buf` until the `in_buf` is
/// empty, `out` is full, the end of the deflate stream is hit, or there is an error in the
/// deflate stream.
///
/// # Arguments
///
/// `r` is a [`DecompressorOxide`] struct with the state of this stream.
///
/// `in_buf` is a reference to the compressed data that is to be decompressed. The decompressor will
/// start at the first byte of this buffer.
///
/// `out` is a reference to the buffer that will store the decompressed data, and that
/// stores previously decompressed data if any.
///
/// * The offset given by `out_pos` indicates where in the output buffer slice writing should start.
/// * If [`TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF`] is not set, the output buffer is used in a
///   wrapping manner, and it's size is required to be a power of 2.
/// * The decompression function normally needs access to 32KiB of the previously decompressed data
///   (or to the beginning of the decompressed data if less than 32KiB has been decompressed.)
///     - If this data is not available, decompression may fail.
///     - Some deflate compressors allow specifying a window size which limits match distances to
///       less than this, or alternatively an RLE mode where matches will only refer to the previous byte
///       and thus allows a smaller output buffer. The window size can be specified in the zlib
///       header structure, however, the header data should not be relied on to be correct.
///
/// `flags` indicates settings and status to the decompression function.
/// * The [`TINFL_FLAG_HAS_MORE_INPUT`] has to be specified if more compressed data is to be provided
///   in a subsequent call to this function.
/// * See the the [`inflate_flags`] module for details on other flags.
///
/// # Returns
///
/// Returns a tuple containing the status of the compressor, the number of input bytes read, and the
/// number of bytes output to `out`.
pub fn decompress(
    r: &mut DecompressorOxide,
    in_buf: &[u8],
    out: &mut [u8],
    out_pos: usize,
    flags: u32,
) -> (TINFLStatus, usize, usize) {
    let out_buf_size_mask = if flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF != 0 {
        usize::MAX
    } else {
        out.len().saturating_sub(1)
    };

    if (out_buf_size_mask.wrapping_add(1) & out_buf_size_mask) != 0 || out_pos > out.len() {
        return (TINFLStatus::BadParam, 0, 0);
    }

    let mut in_iter = InputWrapper::from_slice(in_buf);

    let mut state = r.state;

    let mut out_buf = OutputBuffer::from_slice_and_pos(out, out_pos);

    let mut l = LocalVars {
        bit_buf: r.bit_buf,
        num_bits: r.num_bits,
        dist: r.dist,
        counter: r.counter,
        num_extra: r.num_extra,
    };

    let mut status = 'state_machine: loop {
        match state {
            Start => generate_state!(state, 'state_machine, {
                l.bit_buf = 0;
                l.num_bits = 0;
                l.dist = 0;
                l.counter = 0;
                l.num_extra = 0;
                r.z_header0 = 0;
                r.z_header1 = 0;
                r.z_adler32 = 1;
                r.check_adler32 = 1;
                if flags & TINFL_FLAG_PARSE_ZLIB_HEADER != 0 {
                    Action::Jump(State::ReadZlibCmf)
                } else {
                    Action::Jump(State::ReadBlockHeader)
                }
            }),

            ReadZlibCmf => generate_state!(state, 'state_machine, {
                read_byte(&mut in_iter, flags, |cmf| {
                    r.z_header0 = u32::from(cmf);
                    Action::Jump(State::ReadZlibFlg)
                })
            }),

            ReadZlibFlg => generate_state!(state, 'state_machine, {
                read_byte(&mut in_iter, flags, |flg| {
                    r.z_header1 = u32::from(flg);
                    validate_zlib_header(r.z_header0, r.z_header1, flags, out_buf_size_mask)
                })
            }),

            ReadBlockHeader => generate_state!(state, 'state_machine, {
                read_bits(&mut l, 3, &mut in_iter, flags, |l, bits| {
                    r.finish = (bits & 1) as u8;
                    r.block_type = ((bits >> 1) & 3) as u8;
                    match r.block_type {
                        0 => Action::Jump(BlockTypeNoCompression),
                        1 => {
                            start_static_table(r);
                            init_tree(r, l).unwrap_or(Action::End(TINFLStatus::Failed))
                        },
                        2 => {
                            l.counter = 0;
                            Action::Jump(ReadTableSizes)
                        },
                        3 => Action::Jump(BlockTypeUnexpected),
                        _ => unreachable!()
                    }
                })
            }),

            BlockTypeNoCompression => generate_state!(state, 'state_machine, {
                pad_to_bytes(&mut l, &mut in_iter, flags, |l| {
                    l.counter = 0;
                    Action::Jump(RawHeader)
                })
            }),

            RawHeader => generate_state!(state, 'state_machine, {
                if l.counter < 4 {
                    if l.num_bits != 0 {
                        read_bits(&mut l, 8, &mut in_iter, flags, |l, bits| {
                            r.raw_header[l.counter as usize] = bits as u8;
                            l.counter += 1;
                            Action::None
                        })
                    } else {
                        read_byte(&mut in_iter, flags, |byte| {
                            r.raw_header[l.counter as usize] = byte;
                            l.counter += 1;
                            Action::None
                        })
                    }
                } else {
                    let length = u16::from(r.raw_header[0]) | (u16::from(r.raw_header[1]) << 8);
                    let check = u16::from(r.raw_header[2]) | (u16::from(r.raw_header[3]) << 8);
                    let valid = length == !check;
                    l.counter = length.into();

                    if !valid {
                        Action::Jump(BadRawLength)
                    } else if l.counter == 0 {
                        Action::Jump(BlockDone)
                    } else if l.num_bits != 0 {
                        Action::Jump(RawReadFirstByte)
                    } else {
                        Action::Jump(RawMemcpy1)
                    }
                }
            }),

            RawReadFirstByte => generate_state!(state, 'state_machine, {
                read_bits(&mut l, 8, &mut in_iter, flags, |l, bits| {
                    l.dist = bits as u32;
                    Action::Jump(RawStoreFirstByte)
                })
            }),

            RawStoreFirstByte => generate_state!(state, 'state_machine, {
                if out_buf.bytes_left() == 0 {
                    Action::End(TINFLStatus::HasMoreOutput)
                } else {
                    out_buf.write_byte(l.dist as u8);
                    l.counter -= 1;
                    if l.counter == 0 || l.num_bits == 0 {
                        Action::Jump(RawMemcpy1)
                    } else {
                        Action::Jump(RawReadFirstByte)
                    }
                }
            }),

            RawMemcpy1 => generate_state!(state, 'state_machine, {
                if l.counter == 0 {
                    Action::Jump(BlockDone)
                } else if out_buf.bytes_left() == 0 {
                    Action::End(TINFLStatus::HasMoreOutput)
                } else {
                    Action::Jump(RawMemcpy2)
                }
            }),

            RawMemcpy2 => generate_state!(state, 'state_machine, {
                if in_iter.bytes_left() > 0 {
                    let space_left = out_buf.bytes_left();
                    let bytes_to_copy = cmp::min(cmp::min(
                        space_left,
                        in_iter.bytes_left()),
                        l.counter as usize
                    );

                    out_buf.write_slice(&in_iter.as_slice()[..bytes_to_copy]);

                    in_iter.advance(bytes_to_copy);
                    l.counter -= bytes_to_copy as u32;
                    Action::Jump(RawMemcpy1)
                } else {
                    end_of_input(flags)
                }
            }),

            ReadTableSizes => generate_state!(state, 'state_machine, {
                if l.counter < 3 {
                    let num_bits = [5, 5, 4][l.counter as usize];
                    read_bits(&mut l, num_bits, &mut in_iter, flags, |l, bits| {
                        r.table_sizes[l.counter as usize] =
                            bits as u16 + MIN_TABLE_SIZES[l.counter as usize];
                        l.counter += 1;
                        Action::None
                    })
                } else {
                    r.code_size_huffman.fill(0);
                    l.counter = 0;
                    if r.table_sizes[LITLEN_TABLE] <= 286 && r.table_sizes[DIST_TABLE] <= 30 {
                        Action::Jump(ReadHufflenTableCodeSize)
                    }
                    else {
                        Action::Jump(BadDistOrLiteralTableLength)
                    }
                }
            }),

            ReadHufflenTableCodeSize => generate_state!(state, 'state_machine, {
                if l.counter < r.table_sizes[HUFFLEN_TABLE].into() {
                    read_bits(&mut l, 3, &mut in_iter, flags, |l, bits| {
                        r.code_size_huffman[HUFFMAN_LENGTH_ORDER[l.counter as usize] as usize] =
                                bits as u8;
                        l.counter += 1;
                        Action::None
                    })
                } else {
                    r.table_sizes[HUFFLEN_TABLE] = MAX_HUFF_SYMBOLS_2 as u16;
                    init_tree(r, &mut l).unwrap_or(Action::End(TINFLStatus::Failed))
                }
            }),

            ReadLitlenDistTablesCodeSize => generate_state!(state, 'state_machine, {
                if l.counter < u32::from(r.table_sizes[LITLEN_TABLE]) + u32::from(r.table_sizes[DIST_TABLE]) {
                    decode_huffman_code(
                        r, &mut l, HUFFLEN_TABLE,
                        flags, &mut in_iter, |r, l, symbol| {
                            l.dist = symbol as u32;
                            if l.dist < 16 {
                                r.len_codes[l.counter as usize & LEN_CODES_MASK] = l.dist as u8;
                                l.counter += 1;
                                Action::None
                            } else if l.dist == 16 && l.counter == 0 {
                                Action::Jump(BadCodeSizeDistPrevLookup)
                            } else {
                                l.num_extra = [2, 3, 7, 0][(l.dist as usize - 16) & 3];
                                Action::Jump(ReadExtraBitsCodeSize)
                            }
                        }
                    )
                } else if l.counter != u32::from(r.table_sizes[LITLEN_TABLE]) + u32::from(r.table_sizes[DIST_TABLE]) {
                    Action::Jump(BadCodeSizeSum)
                } else {

                    r.code_size_literal[..r.table_sizes[LITLEN_TABLE] as usize]
                        .copy_from_slice(&r.len_codes[..r.table_sizes[LITLEN_TABLE] as usize & LEN_CODES_MASK]);

                    let dist_table_start = r.table_sizes[LITLEN_TABLE] as usize;
                    debug_assert!(dist_table_start < r.len_codes.len());
                    let dist_table_end = (r.table_sizes[LITLEN_TABLE] +
                                          r.table_sizes[DIST_TABLE]) as usize;
                    let code_size_dist_end = r.table_sizes[DIST_TABLE] as usize;
                    debug_assert!(dist_table_end < r.len_codes.len());
                    debug_assert!(code_size_dist_end < r.code_size_dist.len());
                    let dist_table_start = dist_table_start & LEN_CODES_MASK;
                    let dist_table_end = dist_table_end & LEN_CODES_MASK;
                    r.code_size_dist[..code_size_dist_end & (MAX_HUFF_SYMBOLS_1 - 1)]
                        .copy_from_slice(&r.len_codes[dist_table_start..dist_table_end]);

                    r.block_type -= 1;
                    init_tree(r, &mut l).unwrap_or(Action::End(TINFLStatus::Failed))
                }
            }),

            ReadExtraBitsCodeSize => generate_state!(state, 'state_machine, {
                let num_extra = l.num_extra.into();
                read_bits(&mut l, num_extra, &mut in_iter, flags, |l, mut extra_bits| {
                    extra_bits += [3, 3, 11][(l.dist as usize - 16) & 2];
                    let val = if l.dist == 16 {
                        debug_assert!(l.counter as usize - 1 < r.len_codes.len());
                        r.len_codes[(l.counter as usize - 1) & LEN_CODES_MASK]
                    } else {
                        0
                    };

                    let fill_start = l.counter as usize;
                    let fill_end = l.counter as usize + extra_bits as usize;
                    debug_assert!(fill_start < r.len_codes.len());
                    debug_assert!(fill_end < r.len_codes.len());

                    r.len_codes[
                            fill_start & LEN_CODES_MASK..fill_end & LEN_CODES_MASK
                        ].fill(val);
                    l.counter += extra_bits as u32;
                    Action::Jump(ReadLitlenDistTablesCodeSize)
                })
            }),

            DecodeLitlen => generate_state!(state, 'state_machine, {
                if in_iter.bytes_left() < 4 || out_buf.bytes_left() < 2 {
                    decode_huffman_code(
                        r,
                        &mut l,
                        LITLEN_TABLE,
                        flags,
                        &mut in_iter,
                        |_r, l, symbol| {
                            l.counter = symbol as u32;
                            Action::Jump(WriteSymbol)
                        },
                    )
                } else if
                    out_buf.bytes_left() >= 259 &&
                    in_iter.bytes_left() >= 14
                {
                    let (status, new_state) = decompress_fast(
                        r,
                        &mut in_iter,
                        &mut out_buf,
                        flags,
                        &mut l,
                        out_buf_size_mask,
                    );

                    state = new_state;
                    if status == TINFLStatus::Done {
                        Action::Jump(new_state)
                    } else {
                        Action::End(status)
                    }
                } else {
                    fill_bit_buffer(&mut l, &mut in_iter);

                    let (symbol, code_len) = r.tables[LITLEN_TABLE].lookup(l.bit_buf);

                    l.counter = symbol as u32;
                    l.bit_buf >>= code_len;
                    l.num_bits -= code_len;

                    if (l.counter & 256) != 0 {
                        Action::Jump(HuffDecodeOuterLoop1)
                    } else {
                        if cfg!(not(target_pointer_width = "64")) {
                            fill_bit_buffer(&mut l, &mut in_iter);
                        }

                        let (symbol, code_len) = r.tables[LITLEN_TABLE].lookup(l.bit_buf);

                            l.bit_buf >>= code_len;
                            l.num_bits -= code_len;
                            out_buf.write_byte(l.counter as u8);
                            if (symbol & 256) != 0 {
                                l.counter = symbol as u32;
                                Action::Jump(HuffDecodeOuterLoop1)
                            } else {
                                out_buf.write_byte(symbol as u8);
                                Action::None
                            }

                    }

                }
            }),

            WriteSymbol => generate_state!(state, 'state_machine, {
                if l.counter >= 256 {
                    Action::Jump(HuffDecodeOuterLoop1)
                } else if out_buf.bytes_left() > 0 {
                    out_buf.write_byte(l.counter as u8);
                    Action::Jump(DecodeLitlen)
                } else {
                    Action::End(TINFLStatus::HasMoreOutput)
                }
            }),

            HuffDecodeOuterLoop1 => generate_state!(state, 'state_machine, {
                l.counter &= 511;

                if l.counter
                    == 256 {
                    Action::Jump(BlockDone)
                } else if l.counter > 285 {
                    Action::Jump(InvalidLitlen)
                } else {
                    l.num_extra =
                        LENGTH_EXTRA[(l.counter - 257) as usize & BASE_EXTRA_MASK];
                    l.counter = u32::from(LENGTH_BASE[(l.counter - 257) as usize & BASE_EXTRA_MASK]);
                    if l.num_extra != 0 {
                        Action::Jump(ReadExtraBitsLitlen)
                    } else {
                        Action::Jump(DecodeDistance)
                    }
                }
            }),

            ReadExtraBitsLitlen => generate_state!(state, 'state_machine, {
                let num_extra = l.num_extra.into();
                read_bits(&mut l, num_extra, &mut in_iter, flags, |l, extra_bits| {
                    l.counter += extra_bits as u32;
                    Action::Jump(DecodeDistance)
                })
            }),

            DecodeDistance => generate_state!(state, 'state_machine, {
                decode_huffman_code(r, &mut l, DIST_TABLE, flags, &mut in_iter, |_r, l, symbol| {
                    let symbol = symbol as usize;
                    if symbol > 29 {
                        return Action::Jump(InvalidDist)
                    }
                    l.num_extra = num_extra_bits_for_distance_code(symbol as u8);
                    l.dist = u32::from(DIST_BASE[symbol]);
                    if l.num_extra != 0 {
                        Action::Jump(ReadExtraBitsDistance)
                    } else {
                        Action::Jump(HuffDecodeOuterLoop2)
                    }
                })
            }),

            ReadExtraBitsDistance => generate_state!(state, 'state_machine, {
                let num_extra = l.num_extra.into();
                read_bits(&mut l, num_extra, &mut in_iter, flags, |l, extra_bits| {
                    l.dist += extra_bits as u32;
                    Action::Jump(HuffDecodeOuterLoop2)
                })
            }),

            HuffDecodeOuterLoop2 => generate_state!(state, 'state_machine, {
                if (l.dist as usize > out_buf.position() &&
                    (flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF != 0)) || (l.dist as usize > out_buf.get_ref().len())
                {
                    Action::Jump(DistanceOutOfBounds)
                } else {
                    let out_pos = out_buf.position();
                    let source_pos = out_buf.position()
                        .wrapping_sub(l.dist as usize) & out_buf_size_mask;

                    let out_len = out_buf.get_ref().len();
                    let match_end_pos = out_buf.position() + l.counter as usize;

                    if match_end_pos > out_len ||
                        (source_pos >= out_pos && (source_pos - out_pos) < l.counter as usize)
                    {
                        if l.counter == 0 {
                            Action::Jump(DecodeLitlen)
                        } else {
                            Action::Jump(WriteLenBytesToEnd)
                        }
                    } else {
                        apply_match(
                            out_buf.get_mut(),
                            out_pos,
                            l.dist as usize,
                            l.counter as usize,
                            out_buf_size_mask
                        );
                        out_buf.set_position(out_pos + l.counter as usize);
                        Action::Jump(DecodeLitlen)
                    }
                }
            }),

            WriteLenBytesToEnd => generate_state!(state, 'state_machine, {
                if out_buf.bytes_left() > 0 {
                    let out_pos = out_buf.position();
                    let source_pos = out_buf.position()
                        .wrapping_sub(l.dist as usize) & out_buf_size_mask;


                    let len = cmp::min(out_buf.bytes_left(), l.counter as usize);

                    transfer(out_buf.get_mut(), source_pos, out_pos, len, out_buf_size_mask);

                    out_buf.set_position(out_pos + len);
                    l.counter -= len as u32;
                    if l.counter == 0 {
                        Action::Jump(DecodeLitlen)
                    } else {
                        Action::None
                    }
                } else {
                    Action::End(TINFLStatus::HasMoreOutput)
                }
            }),

            BlockDone => generate_state!(state, 'state_machine, {
                if r.finish != 0 {
                    pad_to_bytes(&mut l, &mut in_iter, flags, |_| Action::None);

                    let in_consumed = in_buf.len() - in_iter.bytes_left();
                    let undo = undo_bytes(&mut l, in_consumed as u32) as usize;
                    in_iter = InputWrapper::from_slice(in_buf[in_consumed - undo..].iter().as_slice());

                    l.bit_buf &= ((1 as BitBuffer) << l.num_bits) - 1;
                    debug_assert_eq!(l.num_bits, 0);

                    if flags & TINFL_FLAG_PARSE_ZLIB_HEADER != 0 {
                        l.counter = 0;
                        Action::Jump(ReadAdler32)
                    } else {
                        Action::Jump(DoneForever)
                    }
                } else {
                    #[cfg(feature = "block-boundary")]
                    if flags & TINFL_FLAG_STOP_ON_BLOCK_BOUNDARY != 0 {
                        Action::End(TINFLStatus::BlockBoundary)
                    } else {
                        Action::Jump(ReadBlockHeader)
                    }
                    #[cfg(not(feature = "block-boundary"))]
                    {
                        Action::Jump(ReadBlockHeader)
                    }
                }
            }),

            ReadAdler32 => generate_state!(state, 'state_machine, {
                if l.counter < 4 {
                    if l.num_bits != 0 {
                        read_bits(&mut l, 8, &mut in_iter, flags, |l, bits| {
                            r.z_adler32 <<= 8;
                            r.z_adler32 |= bits as u32;
                            l.counter += 1;
                            Action::None
                        })
                    } else {
                        read_byte(&mut in_iter, flags, |byte| {
                            r.z_adler32 <<= 8;
                            r.z_adler32 |= u32::from(byte);
                            l.counter += 1;
                            Action::None
                        })
                    }
                } else {
                    Action::Jump(DoneForever)
                }
            }),

            DoneForever => break TINFLStatus::Done,

            _ => break TINFLStatus::Failed,
        };
    };

    let in_undo = if status != TINFLStatus::NeedsMoreInput
        && status != TINFLStatus::FailedCannotMakeProgress
    {
        undo_bytes(&mut l, (in_buf.len() - in_iter.bytes_left()) as u32) as usize
    } else {
        0
    };

    #[cfg(feature = "block-boundary")]
    if status == TINFLStatus::BlockBoundary {
        state = State::ReadBlockHeader;
    }

    if status == TINFLStatus::NeedsMoreInput
        && out_buf.bytes_left() == 0
        && state != State::ReadAdler32
    {
        status = TINFLStatus::HasMoreOutput
    }

    r.state = state;
    r.bit_buf = l.bit_buf;
    r.num_bits = l.num_bits;
    r.dist = l.dist;
    r.counter = l.counter;
    r.num_extra = l.num_extra;

    r.bit_buf &= ((1 as BitBuffer) << r.num_bits) - 1;

    let need_adler = if (flags & TINFL_FLAG_IGNORE_ADLER32) == 0 {
        flags & (TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32) != 0
    } else {
        false
    };
    if need_adler && status as i32 >= 0 {
        let out_buf_pos = out_buf.position();
        r.check_adler32 = update_adler32(r.check_adler32, &out_buf.get_ref()[out_pos..out_buf_pos]);

        if status == TINFLStatus::Done
            && flags & TINFL_FLAG_PARSE_ZLIB_HEADER != 0
            && r.check_adler32 != r.z_adler32
        {
            status = TINFLStatus::Adler32Mismatch;
        }
    }

    (
        status,
        in_buf.len() - in_iter.bytes_left() - in_undo,
        out_buf.position() - out_pos,
    )
}
