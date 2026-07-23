use core::char;
use core::fmt;

/// Representation of a demangled symbol name.
pub struct Demangle<'a> {
    inner: &'a str,
    /// The number of ::-separated elements in the original name.
    elements: usize,
}

/// De-mangles a Rust symbol into a more readable version
///
/// All Rust symbols by default are mangled as they contain characters that
/// cannot be represented in all object files. The mangling mechanism is similar
/// to C++'s, but Rust has a few specifics to handle items like lifetimes in
/// symbols.
///
/// This function will take a **mangled** symbol and return a value. When printed,
/// the de-mangled version will be written. If the symbol does not look like
/// a mangled symbol, the original value will be written instead.
///
/// # Examples
///
/// ```
/// use rustc_demangle::demangle;
///
/// assert_eq!(demangle("_ZN4testE").to_string(), "test");
/// assert_eq!(demangle("_ZN3foo3barE").to_string(), "foo::bar");
/// assert_eq!(demangle("foo").to_string(), "foo");
/// ```

pub fn demangle(s: &str) -> Result<(Demangle, &str), ()> {
    let inner = if s.starts_with("_ZN") {
        &s[3..]
    } else if s.starts_with("ZN") {
        &s[2..]
    } else if s.starts_with("__ZN") {
        &s[4..]
    } else {
        return Err(());
    };

    if inner.bytes().any(|c| c & 0x80 != 0) {
        return Err(());
    }

    let mut elements = 0;
    let mut chars = inner.chars();
    let mut c = chars.next().ok_or(())?;
    while c != 'E' {
        if !c.is_digit(10) {
            return Err(());
        }
        let mut len = 0usize;
        while let Some(d) = c.to_digit(10) {
            len = len
                .checked_mul(10)
                .and_then(|len| len.checked_add(d as usize))
                .ok_or(())?;
            c = chars.next().ok_or(())?;
        }

        for _ in 0..len {
            c = chars.next().ok_or(())?;
        }

        elements += 1;
    }

    Ok((Demangle { inner, elements }, chars.as_str()))
}

fn is_rust_hash(s: &str) -> bool {
    s.starts_with('h') && s[1..].chars().all(|c| c.is_digit(16))
}

impl<'a> fmt::Display for Demangle<'a> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let mut inner = self.inner;
        for element in 0..self.elements {
            let mut rest = inner;
            while rest.chars().next().unwrap().is_digit(10) {
                rest = &rest[1..];
            }
            let i: usize = inner[..(inner.len() - rest.len())].parse().unwrap();
            inner = &rest[i..];
            rest = &rest[..i];
            if f.alternate() && element + 1 == self.elements && is_rust_hash(&rest) {
                break;
            }
            if element != 0 {
                f.write_str("::")?;
            }
            if rest.starts_with("_$") {
                rest = &rest[1..];
            }
            loop {
                if rest.starts_with('.') {
                    if let Some('.') = rest[1..].chars().next() {
                        f.write_str("::")?;
                        rest = &rest[2..];
                    } else {
                        f.write_str(".")?;
                        rest = &rest[1..];
                    }
                } else if rest.starts_with('$') {
                    let (escape, after_escape) = if let Some(end) = rest[1..].find('$') {
                        (&rest[1..=end], &rest[end + 2..])
                    } else {
                        break;
                    };

                    let unescaped = match escape {
                        "SP" => "@",
                        "BP" => "*",
                        "RF" => "&",
                        "LT" => "<",
                        "GT" => ">",
                        "LP" => "(",
                        "RP" => ")",
                        "C" => ",",

                        _ => {
                            if escape.starts_with('u') {
                                let digits = &escape[1..];
                                let all_lower_hex = digits.chars().all(|c| match c {
                                    '0'..='9' | 'a'..='f' => true,
                                    _ => false,
                                });
                                let c = u32::from_str_radix(digits, 16)
                                    .ok()
                                    .and_then(char::from_u32);
                                if let (true, Some(c)) = (all_lower_hex, c) {
                                    if !c.is_control() {
                                        c.fmt(f)?;
                                        rest = after_escape;
                                        continue;
                                    }
                                }
                            }
                            break;
                        }
                    };
                    f.write_str(unescaped)?;
                    rest = after_escape;
                } else if let Some(i) = rest.find(|c| c == '$' || c == '.') {
                    f.write_str(&rest[..i])?;
                    rest = &rest[i..];
                } else {
                    break;
                }
            }
            f.write_str(rest)?;
        }

        Ok(())
    }
}
