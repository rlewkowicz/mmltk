/*!
Contains architecture independent routines.

These routines are often used as a "fallback" implementation when the more
specialized architecture dependent routines are unavailable.
*/

pub mod memchr;
pub mod packedpair;
pub mod rabinkarp;
#[cfg(feature = "alloc")]
pub mod shiftor;
pub mod twoway;

/// Returns true if and only if `needle` is a prefix of `haystack`.
///
/// This uses a latency optimized variant of `memcmp` internally which *might*
/// make this faster for very short strings.
///
/// # Inlining
///
/// This routine is marked `inline(always)`. If you want to call this function
/// in a way that is not always inlined, you'll need to wrap a call to it in
/// another function that is marked as `inline(never)` or just `inline`.
#[inline(always)]
pub fn is_prefix(haystack: &[u8], needle: &[u8]) -> bool {
    needle.len() <= haystack.len()
        && is_equal(&haystack[..needle.len()], needle)
}

/// Returns true if and only if `needle` is a suffix of `haystack`.
///
/// This uses a latency optimized variant of `memcmp` internally which *might*
/// make this faster for very short strings.
///
/// # Inlining
///
/// This routine is marked `inline(always)`. If you want to call this function
/// in a way that is not always inlined, you'll need to wrap a call to it in
/// another function that is marked as `inline(never)` or just `inline`.
#[inline(always)]
pub fn is_suffix(haystack: &[u8], needle: &[u8]) -> bool {
    needle.len() <= haystack.len()
        && is_equal(&haystack[haystack.len() - needle.len()..], needle)
}

/// Compare corresponding bytes in `x` and `y` for equality.
///
/// That is, this returns true if and only if `x.len() == y.len()` and
/// `x[i] == y[i]` for all `0 <= i < x.len()`.
///
/// # Inlining
///
/// This routine is marked `inline(always)`. If you want to call this function
/// in a way that is not always inlined, you'll need to wrap a call to it in
/// another function that is marked as `inline(never)` or just `inline`.
///
/// # Motivation
///
/// Why not use slice equality instead? Well, slice equality usually results in
/// a call out to the current platform's `libc` which might not be inlineable
/// or have other overhead. This routine isn't guaranteed to be a win, but it
/// might be in some cases.
#[inline(always)]
pub fn is_equal(x: &[u8], y: &[u8]) -> bool {
    if x.len() != y.len() {
        return false;
    }
    unsafe { is_equal_raw(x.as_ptr(), y.as_ptr(), x.len()) }
}

/// Compare `n` bytes at the given pointers for equality.
///
/// This returns true if and only if `*x.add(i) == *y.add(i)` for all
/// `0 <= i < n`.
///
/// # Inlining
///
/// This routine is marked `inline(always)`. If you want to call this function
/// in a way that is not always inlined, you'll need to wrap a call to it in
/// another function that is marked as `inline(never)` or just `inline`.
///
/// # Motivation
///
/// Why not use slice equality instead? Well, slice equality usually results in
/// a call out to the current platform's `libc` which might not be inlineable
/// or have other overhead. This routine isn't guaranteed to be a win, but it
/// might be in some cases.
///
/// # Safety
///
/// * Both `x` and `y` must be valid for reads of up to `n` bytes.
/// * Both `x` and `y` must point to an initialized value.
/// * Both `x` and `y` must each point to an allocated object and
/// must either be in bounds or at most one byte past the end of the
/// allocated object. `x` and `y` do not need to point to the same allocated
/// object, but they may.
/// * Both `x` and `y` must be _derived from_ a pointer to their respective
/// allocated objects.
/// * The distance between `x` and `x+n` must not overflow `isize`. Similarly
/// for `y` and `y+n`.
/// * The distance being in bounds must not rely on "wrapping around" the
/// address space.
#[inline(always)]
pub unsafe fn is_equal_raw(
    mut x: *const u8,
    mut y: *const u8,
    mut n: usize,
) -> bool {

    while n >= 4 {
        let vx = x.cast::<u32>().read_unaligned();
        let vy = y.cast::<u32>().read_unaligned();
        if vx != vy {
            return false;
        }
        x = x.add(4);
        y = y.add(4);
        n -= 4;
    }
    if n >= 2 {
        let vx = x.cast::<u16>().read_unaligned();
        let vy = y.cast::<u16>().read_unaligned();
        if vx != vy {
            return false;
        }
        x = x.add(2);
        y = y.add(2);
        n -= 2;
    }
    if n > 0 {
        if x.read() != y.read() {
            return false;
        }
    }
    true
}
