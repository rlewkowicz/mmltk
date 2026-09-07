// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.
// The C++ implementation preserved here in comments is licensed as follows:
// Copyright (C) 2015 THL A29 Limited, a Tencent company, and Milo Yip. All
// Licensed under the MIT License (the "License"); you may not use this file
// except in compliance with the License. You may obtain a copy of the License
// http://opensource.org/licenses/MIT
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// License for the specific language governing permissions and limitations under
// the License.

#[doc(hidden)]
#[macro_export]
macro_rules! dtoa {(
    floating_type: $fty:ty,
    significand_type: $sigty:ty,
    exponent_type: $expty:ty,
    $($diyfp_param:ident: $diyfp_value:tt,)*
) => {

diyfp! {
    floating_type: $fty,
    significand_type: $sigty,
    exponent_type: $expty,
    $($diyfp_param: $diyfp_value,)*
};


#[inline]
unsafe fn grisu_round(buffer: *mut u8, len: isize, delta: $sigty, mut rest: $sigty, ten_kappa: $sigty, wp_w: $sigty) {
    while rest < wp_w && delta - rest >= ten_kappa &&
           (rest + ten_kappa < wp_w || 
            wp_w - rest > rest + ten_kappa - wp_w) {
        *buffer.offset(len - 1) -= 1;
        rest += ten_kappa;
    }
}


#[inline]
fn count_decimal_digit32(n: u32) -> usize {
    if n < 10 { 1 }
    else if n < 100 { 2 }
    else if n < 1000 { 3 }
    else if n < 10000 { 4 }
    else if n < 100000 { 5 }
    else if n < 1000000 { 6 }
    else if n < 10000000 { 7 }
    else if n < 100000000 { 8 }
    else { 9 }
}


#[inline]
unsafe fn digit_gen(w: DiyFp, mp: DiyFp, mut delta: $sigty, buffer: *mut u8, mut k: isize) -> (isize, isize) {
    static POW10: [$sigty; 10] = [ 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 ];
    let one = DiyFp::new(1 << -mp.e, mp.e);
    let wp_w = mp - w;
    let mut p1 = (mp.f >> -one.e) as u32;
    let mut p2 = mp.f & (one.f - 1);
    let mut kappa = count_decimal_digit32(p1); 
    let mut len = 0;

    while kappa > 0 {
        let mut d = 0u32;
        match kappa {
            9 => { d = p1 /  100000000; p1 %=  100000000; }
            8 => { d = p1 /   10000000; p1 %=   10000000; }
            7 => { d = p1 /    1000000; p1 %=    1000000; }
            6 => { d = p1 /     100000; p1 %=     100000; }
            5 => { d = p1 /      10000; p1 %=      10000; }
            4 => { d = p1 /       1000; p1 %=       1000; }
            3 => { d = p1 /        100; p1 %=        100; }
            2 => { d = p1 /         10; p1 %=         10; }
            1 => { d = p1;              p1 =           0; }
            _ => {}
        }
        if d != 0 || len != 0 {
            *buffer.offset(len) = b'0' + d as u8;
            len += 1;
        }
        kappa -= 1;
        let tmp = (p1 as $sigty << -one.e) + p2;
        if tmp <= delta {
            k += kappa as isize;
            grisu_round(buffer, len, delta, tmp, POW10[kappa] << -one.e, wp_w.f);
            return (len, k);
        }
    }

    loop {
        p2 *= 10;
        delta *= 10;
        let d = (p2 >> -one.e) as u8;
        if d != 0 || len != 0 {
            *buffer.offset(len) = b'0' + d;
            len += 1;
        }
        p2 &= one.f - 1;
        kappa = kappa.wrapping_sub(1);
        if p2 < delta {
            k += kappa as isize;
            let index = -(kappa as isize);
            grisu_round(buffer, len, delta, p2, one.f, wp_w.f * if index < 9 { POW10[-(kappa as isize) as usize] } else { 0 });
            return (len, k);
        }
    }
}


#[inline]
unsafe fn grisu2(value: $fty, buffer: *mut u8) -> (isize, isize) {
    let v = DiyFp::from(value);
    let (w_m, w_p) = v.normalized_boundaries();

    let (c_mk, k) = get_cached_power(w_p.e);
    let w = v.normalize() * c_mk;
    let mut wp = w_p * c_mk;
    let mut wm = w_m * c_mk;
    wm.f += 1;
    wp.f -= 1;
    digit_gen(w, wp, wp.f - wm.f, buffer, k)
}


#[inline]
unsafe fn write_exponent(mut k: isize, mut buffer: *mut u8) -> *mut u8 {
    if k < 0 {
        *buffer = b'-';
        buffer = buffer.offset(1);
        k = -k;
    }

    if k >= 100 {
        *buffer = b'0' + (k / 100) as u8;
        k %= 100;
        let d = DEC_DIGITS_LUT.get_unchecked(k as usize * 2);
        ptr::copy_nonoverlapping(d, buffer.offset(1), 2);
        buffer.offset(3)
    } else if k >= 10 {
        let d = DEC_DIGITS_LUT.get_unchecked(k as usize * 2);
        ptr::copy_nonoverlapping(d, buffer, 2);
        buffer.offset(2)
    } else {
        *buffer = b'0' + k as u8;
        buffer.offset(1)
    }
}


#[inline]
unsafe fn prettify(buffer: *mut u8, length: isize, k: isize) -> *mut u8 {
    let kk = length + k; 

    if 0 <= k && kk <= 21 {
        for i in length..kk {
            *buffer.offset(i) = b'0';
        }
        *buffer.offset(kk) = b'.';
        *buffer.offset(kk + 1) = b'0';
        buffer.offset(kk + 2)
    }

    else if 0 < kk && kk <= 21 {
        ptr::copy(buffer.offset(kk), buffer.offset(kk + 1), (length - kk) as usize);
        *buffer.offset(kk) = b'.';
        if 0 > k + MAX_DECIMAL_PLACES {
            for i in (kk + 2 .. kk + MAX_DECIMAL_PLACES + 1).rev() {
                if *buffer.offset(i) != b'0' {
                    return buffer.offset(i + 1);
                }
            }
            buffer.offset(kk + 2) 
        } else {
            buffer.offset(length + 1)
        }
    }

    else if -6 < kk && kk <= 0 {
        let offset = 2 - kk;
        ptr::copy(buffer, buffer.offset(offset), length as usize);
        *buffer = b'0';
        *buffer.offset(1) = b'.';
        for i in 2..offset {
            *buffer.offset(i) = b'0';
        }
        if length - kk > MAX_DECIMAL_PLACES {
            for i in (3 .. MAX_DECIMAL_PLACES + 2).rev() {
                if *buffer.offset(i) != b'0' {
                    return buffer.offset(i + 1);
                }
            }
            buffer.offset(3) 
        } else {
            buffer.offset(length + offset)
        }
    }

    else if kk < -MAX_DECIMAL_PLACES {
        *buffer = b'0';
        *buffer.offset(1) = b'.';
        *buffer.offset(2) = b'0';
        buffer.offset(3)
    }

    else if length == 1 {
        *buffer.offset(1) = b'e';
        write_exponent(kk - 1, buffer.offset(2))
    }

    else {
        ptr::copy(buffer.offset(1), buffer.offset(2), (length - 1) as usize);
        *buffer.offset(1) = b'.';
        *buffer.offset(length + 1) = b'e';
        write_exponent(kk - 1, buffer.offset(length + 2))
    }
}


#[allow(deprecated)]
#[inline]
unsafe fn dtoa<W: io::Write>(mut wr: W, mut value: $fty) -> io::Result<usize> {
    if value == 0.0 {
        if value.is_sign_negative() {
            match wr.write_all(b"-0.0") {
                Ok(()) => Ok(4),
                Err(e) => Err(e),
            }
        } else {
            match wr.write_all(b"0.0") {
                Ok(()) => Ok(3),
                Err(e) => Err(e),
            }
        }
    } else {
        let negative = value < 0.0;
        if negative {
            if let Err(e) = wr.write_all(b"-") {
                return Err(e);
            }
            value = -value;
        }
        let mut buffer: [u8; 24] = mem::uninitialized();
        let buf_ptr = buffer.as_mut_ptr();
        let (length, k) = grisu2(value, buf_ptr);
        let end = prettify(buf_ptr, length, k);
        let len = end as usize - buf_ptr as usize;
        if let Err(e) = wr.write_all(slice::from_raw_parts(buf_ptr, len)) {
            return Err(e);
        }
        if negative {
            Ok(len + 1)
        } else {
            Ok(len)
        }
    }
}

}}
