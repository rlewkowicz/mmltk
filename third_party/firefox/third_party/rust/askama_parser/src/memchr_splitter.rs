pub(crate) trait Splitter: Copy {
    /// If any of the needles was found in the haystack, then split the haystack at the first hit.
    ///
    /// Since only the first byte of a needle is inspected, be aware that there can be
    /// false-positives. Always compare the latter string of the output if it fits the expected
    /// prefix.
    fn split<'a>(&self, haystack: &'a str) -> Option<(&'a str, &'a str)>;
}

impl<T: Splitter> Splitter for &T {
    #[inline]
    fn split<'a>(&self, haystack: &'a str) -> Option<(&'a str, &'a str)> {
        T::split(self, haystack)
    }
}

macro_rules! new_memchr_type {
    ($struct:ident $split_unchecked:ident $memchr:ident $($field:ident)*) => {
        #[derive(Debug, Clone, Copy)]
        pub(crate) struct $struct {
            $($field: u8,)*
        }

        impl $struct {
            #[track_caller]
            pub(crate) fn new($($field: &str),*) -> Self {
                Self {
                    $($field: $field.as_bytes()[0],)*
                }
            }

            #[inline]
            pub(crate) fn split<'a>(&self, haystack: &'a str) -> Option<(&'a str, &'a str)> {
                unsafe { $split_unchecked($(self.$field,)* haystack) }
            }
        }

        impl Splitter for $struct {
            #[inline]
            fn split<'a>(&self, haystack: &'a str) -> Option<(&'a str, &'a str)> {
                self.split(haystack)
            }
        }

        /// SAFETY: caller has to ensure that the needle is at a char boundary
        pub(crate) unsafe fn $split_unchecked(
            $($field: u8,)*
            haystack: &str,
        ) -> Option<(&str, &str)> {
            let idx = memchr::$memchr($($field,)* haystack.as_bytes())?;
            Some((haystack.get_unchecked(..idx), haystack.get_unchecked(idx..)))
        }
    };
}

new_memchr_type!(Splitter1 split1_unchecked memchr a);
new_memchr_type!(Splitter2 split2_unchecked memchr2 a b);
new_memchr_type!(Splitter3 split3_unchecked memchr3 a b c);
