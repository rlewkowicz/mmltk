// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

#[macro_export]
macro_rules! tinystr {
    ($n:literal, $s:literal) => {{
        const TINYSTR_MACRO_CONST: $crate::TinyAsciiStr<$n> = {
            match $crate::TinyAsciiStr::try_from_utf8($s.as_bytes()) {
                Ok(s) => s,
                #[allow(clippy::panic)]
                Err(_) => panic!(concat!("Failed to construct tinystr from ", $s)),
            }
        };
        TINYSTR_MACRO_CONST
    }};
}
