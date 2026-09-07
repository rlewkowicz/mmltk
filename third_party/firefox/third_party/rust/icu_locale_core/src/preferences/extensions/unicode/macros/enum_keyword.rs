// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

/// Internal macro used by `enum_keyword` for nesting.
#[macro_export]
#[doc(hidden)]
macro_rules! __enum_keyword_inner {
    ($name:ident, $variant:ident) => {
        $name::$variant
    };
    ($name:ident, $variant:ident, $s:ident, $v2:ident, $($subk:expr => $subv:ident),*) => {{
        let sv = $s.get_subtag(1).and_then(|st| {
            match st.as_str() {
                $(
                    $subk => Some($v2::$subv),
                )*
                _ => None,
            }
        });
        $name::$variant(sv)
    }};
}

/// Macro used to generate a preference keyword as an enum.
///
/// The macro supports single and two subtag enums.
///
/// # Examples
///
/// ```
/// use icu::locale::preferences::extensions::unicode::enum_keyword;
///
/// enum_keyword!(
///     EmojiPresentationStyle {
///         ("emoji" => Emoji),
///         ("text" => Text),
///         ("default" => Default)
/// }, "em");
///
/// enum_keyword!(
///      MetaKeyword {
///         ("normal" => Normal),
///         ("emoji" => Emoji(EmojiPresentationStyle) {
///             ("emoji" => Emoji),
///             ("text" => Text),
///             ("default" => Default)
///         })
/// }, "mk");
/// ```
#[macro_export]
#[doc(hidden)]
macro_rules! __enum_keyword {
    (
        $(#[$doc:meta])*
        $([$derive_attrs:ty])?
        $name:ident {
            $(
                $(#[$variant_doc:meta])*
                $([$variant_attr:ty])?
                $variant:ident $($v2:ident)?
            ),*
        }
    ) => {
        #[non_exhaustive]
        #[derive(Debug, Clone, Eq, PartialEq, Copy, Hash)]
        $(#[derive($derive_attrs)])?
        $(#[$doc])*
        pub enum $name {
            $(
                $(#[$variant_doc])*
                $(#[$variant_attr])?
                $variant $((Option<$v2>))?
            ),*
        }
    };
    ($(#[$doc:meta])*
    $([$derive_attrs:ty])?
    $name:ident {
        $(
            $(#[$variant_doc:meta])*
            $([$variant_attr:ty])?
            ($key:expr => $variant:ident $(($v2:ident) {
                $(
                    ($subk:expr => $subv:ident)
                ),*
            })?)
        ),* $(,)?
    },
    $ext_key:literal
    $(, $input:ident, $aliases:stmt)?
    ) => {
        $crate::__enum_keyword!(
            $(#[$doc])*
            $([$derive_attrs])?
            $name {
                $(
                    $(#[$variant_doc])*
                    $([$variant_attr])?
                    $variant $($v2)?
                ),*
            }
        );

        impl $crate::preferences::PreferenceKey for $name {
            fn unicode_extension_key() -> Option<$crate::extensions::unicode::Key> {
                Some($crate::extensions::unicode::key!($ext_key))
            }

            fn try_from_key_value(
                key: &$crate::extensions::unicode::Key,
                value: &$crate::extensions::unicode::Value,
            ) -> Result<Option<Self>, $crate::preferences::extensions::unicode::errors::PreferencesParseError> {
                if Self::unicode_extension_key() == Some(*key) {
                    Self::try_from(value).map(Some)
                } else {
                    Ok(None)
                }
            }

            fn unicode_extension_value(&self) -> Option<$crate::extensions::unicode::Value> {
                Some((*self).into())
            }
        }

        impl TryFrom<&$crate::extensions::unicode::Value> for $name {
            type Error = $crate::preferences::extensions::unicode::errors::PreferencesParseError;

            fn try_from(s: &$crate::extensions::unicode::Value) -> Result<Self, Self::Error> {
                let subtag = s.get_subtag(0)
                                .unwrap_or(&$crate::subtags::subtag!("true"));
                #[allow(unused_imports)]
                use $crate::extensions::unicode::value;
                $(
                    let $input = s;
                    $aliases
                )?
                Ok(match subtag.as_str() {
                    $(
                        $key => {
                            $crate::__enum_keyword_inner!($name, $variant$(, s, $v2, $($subk => $subv),*)?)
                        }
                    )*
                    _ => {
                        return Err(Self::Error::InvalidKeywordValue);
                    }
                })
            }
        }

        impl From<$name>  for $crate::extensions::unicode::Value {
            fn from(input: $name) -> $crate::extensions::unicode::Value {
                let f;
                #[allow(unused_mut)]
                let mut s = None;
                match input {
                    $(
                        #[allow(non_snake_case)]
                        $name::$variant $(($v2))? => {
                            f = $crate::subtags::subtag!($key);

                            $(
                                if let Some(v2) = $v2 {
                                    match v2 {
                                        $(
                                            $v2::$subv => s = Some($crate::subtags::subtag!($subk)),
                                        )*
                                    }
                                }
                            )?
                        },
                    )*
                }
                if let Some(s) = s {
                    $crate::extensions::unicode::Value::from_two_subtags(f, s)
                } else {
                    $crate::extensions::unicode::Value::from_subtag(Some(f))
                }
            }
        }

        impl $name {
            /// A helper function for displaying as a `&str`.
            pub const fn as_str(&self) -> &'static str {
                match self {
                    $(
                        #[allow(non_snake_case)]
                        Self::$variant $(($v2))? => {
                            $(
                                if let Some(v2) = $v2 {
                                    return match v2 {
                                        $(
                                            $v2::$subv => concat!($key, '-', $subk),
                                        )*
                                    };
                                }
                            )?
                            return $key;
                        },
                    )*
                }
            }
        }
    };
}
pub use __enum_keyword as enum_keyword;
