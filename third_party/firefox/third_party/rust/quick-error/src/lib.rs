#![warn(missing_docs)]
//! A macro which makes errors easy to write
//!
//! Minimum type is like this:
//!
//! ```rust
//! #[macro_use] extern crate quick_error;
//! # fn main() {}
//!
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         Variant1 {}
//!     }
//! }
//! ```
//! Both ``pub`` and non-public types may be declared, and all meta attributes
//! (such as ``#[derive(Debug)]``) are forwarded as is. The `Debug` must be
//! implemented (but you may do that yourself if you like). The documentation
//! comments ``/// something`` (as well as other meta attrbiutes) on variants
//! are allowed.
//!
//! # Allowed Syntax
//!
//! You may add arbitrary parameters to any struct variant:
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         /// IO Error
//!         Io(err: std::io::Error) {}
//!         /// Utf8 Error
//!         Utf8(err: std::str::Utf8Error) {}
//!     }
//! }
//! ```
//!
//! Note unlike in normal Enum declarations you declare names of fields (which
//! are omitted from type). How they can be used is outlined below.
//!
//! Now you might have noticed trailing braces `{}`. They are used to define
//! implementations. By default:
//!
//! * `Error::cause()` returns None (even if type wraps some value)
//! * `Display` outputs debug representation
//! * No `From` implementations are defined
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         Io(err: std::io::Error) {
//!             display("{}", err)
//!         }
//!         Utf8(err: std::str::Utf8Error) {
//!             display("utf8 error")
//!         }
//!     }
//! }
//! ```
//!
//! To change `cause` method to return some error, add `cause(value)`, for
//! example:
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         Io(err: std::io::Error) {
//!             cause(err)
//!         }
//!         Utf8(err: std::str::Utf8Error) {
//!             display("utf8 error")
//!         }
//!         Other(err: Box<std::error::Error>) {
//!             cause(&**err)
//!         }
//!     }
//! }
//! ```
//! Note you don't need to wrap value in `Some`, its implicit. In case you want
//! `None` returned just omit the `cause`. You can't return `None`
//! conditionally.
//!
//! To change how each clause is `Display`ed add `display(pattern,..args)`,
//! for example:
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         Io(err: std::io::Error) {
//!             display("I/O error: {}", err)
//!         }
//!         Utf8(err: std::str::Utf8Error) {
//!             display("Utf8 error, valid up to {}", err.valid_up_to())
//!         }
//!     }
//! }
//! ```
//!
//! If you need a reference to the error when `Display`ing, you can instead use
//! `display(x) -> (pattern, ..args)`, where `x` sets the name of the reference.
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! use std::error::Error; // put methods like `source()` of this trait into scope
//!
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         Io(err: std::io::Error) {
//!             display(x) -> ("I/O: {}", err)
//!         }
//!         Utf8(err: std::str::Utf8Error) {
//!             display(self_) -> ("UTF-8 error. Valid up to {}", err.valid_up_to())
//!         }
//!     }
//! }
//! ```
//!
//! To convert to the type from any other, use one of the three forms of
//! `from` clause.
//!
//! For example, to convert simple wrapper use bare `from()`:
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         Io(err: std::io::Error) {
//!             from()
//!         }
//!     }
//! }
//! ```
//!
//! This implements ``From<io::Error>``.
//!
//! To convert to singleton enumeration type (discarding the value), use
//! the `from(type)` form:
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         FormatError {
//!             from(std::fmt::Error)
//!         }
//!     }
//! }
//! ```
//!
//! And the most powerful form is `from(var: type) -> (arguments...)`. It
//! might be used to convert to type with multiple arguments or for arbitrary
//! value conversions:
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # fn main() {}
//! #
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum SomeError {
//!         FailedOperation(s: &'static str, errno: i32) {
//!             from(errno: i32) -> ("os error", errno)
//!             from(e: std::io::Error) -> ("io error", e.raw_os_error().unwrap())
//!         }
//!         /// Converts from both kinds of utf8 errors
//!         Utf8(err: std::str::Utf8Error) {
//!             from()
//!             from(err: std::string::FromUtf8Error) -> (err.utf8_error())
//!         }
//!     }
//! }
//! ```
//! # Context
//!
//! Since quick-error 1.1 we also have a `context` declaration, which is
//! similar to (the longest form of) `from`, but allows adding some context to
//! the error. We need a longer example to demonstrate this:
//!
//! ```rust
//! # #[macro_use] extern crate quick_error;
//! # use std::io;
//! # use std::fs::File;
//! # use std::path::{Path, PathBuf};
//! #
//! use quick_error::ResultExt;
//!
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum Error {
//!         File(filename: PathBuf, err: io::Error) {
//!             context(path: &'a Path, err: io::Error)
//!                 -> (path.to_path_buf(), err)
//!         }
//!     }
//! }
//!
//! fn openfile(path: &Path) -> Result<(), Error> {
//!     try!(File::open(path).context(path));
//!
//!     // If we didn't have context, the line above would be written as;
//!     //
//!     // try!(File::open(path)
//!     //     .map_err(|err| Error::File(path.to_path_buf(), err)));
//!
//!     Ok(())
//! }
//!
//! # fn main() {
//! #     openfile(Path::new("/etc/somefile")).ok();
//! # }
//! ```
//!
//! Each `context(a: A, b: B)` clause implements
//! `From<Context<A, B>> for Error`. Which means multiple `context` clauses
//! are a subject to the normal coherence rules. Unfortunately, we can't
//! provide full support of generics for the context, but you may either use a
//! lifetime `'a` for references or `AsRef<Type>` (the latter means `A:
//! AsRef<Type>`, and `Type` must be concrete). It's also occasionally useful
//! to use a tuple as a type of the first argument.
//!
//! You also need to `use quick_error::ResultExt` extension trait to get
//! working `.context()` method.
//!
//! More info on context in [this article](http://bit.ly/1PsuxDt).
//!
//! All forms of `from`, `display`, `cause`, and `context`
//! clauses can be combined and put in arbitrary order. Only `from` and
//! `context` can be used multiple times in single variant of enumeration.
//! Docstrings are also okay.  Empty braces can be omitted as of quick_error
//! 0.1.3.
//!
//! # Private Enums
//!
//! Since quick-error 1.2.0 we  have a way to make a private enum that is
//! wrapped by public structure:
//!
//! ```rust
//! #[macro_use] extern crate quick_error;
//! # fn main() {}
//!
//! quick_error! {
//!     #[derive(Debug)]
//!     pub enum PubError wraps ErrorEnum {
//!         Variant1 {}
//!     }
//! }
//! ```
//!
//! This generates data structures like this
//!
//! ```rust
//!
//! pub struct PubError(ErrorEnum);
//!
//! enum ErrorEnum {
//!     Variant1,
//! }
//!
//! ```
//!
//! Which in turn allows you to export just `PubError` in your crate and keep
//! actual enumeration private to the crate. This is useful to keep backwards
//! compatibility for error types. Currently there is no shorcuts to define
//! error constructors for the inner type, but we consider adding some in
//! future versions.
//!
//! It's possible to declare internal enum as public too.
//!
//!


/// Main macro that does all the work
#[macro_export]
macro_rules! quick_error {

    (   $(#[$meta:meta])*
        pub enum $name:ident { $($chunks:tt)* }
    ) => {
        quick_error!(SORT [pub enum $name $(#[$meta])* ]
            items [] buf []
            queue [ $($chunks)* ]);
    };
    (   $(#[$meta:meta])*
        enum $name:ident { $($chunks:tt)* }
    ) => {
        quick_error!(SORT [enum $name $(#[$meta])* ]
            items [] buf []
            queue [ $($chunks)* ]);
    };

    (   $(#[$meta:meta])*
        pub enum $name:ident wraps $enum_name:ident { $($chunks:tt)* }
    ) => {
        quick_error!(WRAPPER $enum_name [ pub struct ] $name $(#[$meta])*);
        quick_error!(SORT [enum $enum_name $(#[$meta])* ]
            items [] buf []
            queue [ $($chunks)* ]);
    };

    (   $(#[$meta:meta])*
        pub enum $name:ident wraps pub $enum_name:ident { $($chunks:tt)* }
    ) => {
        quick_error!(WRAPPER $enum_name [ pub struct ] $name $(#[$meta])*);
        quick_error!(SORT [pub enum $enum_name $(#[$meta])* ]
            items [] buf []
            queue [ $($chunks)* ]);
    };
    (   $(#[$meta:meta])*
        enum $name:ident wraps $enum_name:ident { $($chunks:tt)* }
    ) => {
        quick_error!(WRAPPER $enum_name [ struct ] $name $(#[$meta])*);
        quick_error!(SORT [enum $enum_name $(#[$meta])* ]
            items [] buf []
            queue [ $($chunks)* ]);
    };

    (   $(#[$meta:meta])*
        enum $name:ident wraps pub $enum_name:ident { $($chunks:tt)* }
    ) => {
        quick_error!(WRAPPER $enum_name [ struct ] $name $(#[$meta])*);
        quick_error!(SORT [pub enum $enum_name $(#[$meta])* ]
            items [] buf []
            queue [ $($chunks)* ]);
    };


    (
        WRAPPER $internal:ident [ $($strdef:tt)* ] $strname:ident
        $(#[$meta:meta])*
    ) => {
        $(#[$meta])*
        $($strdef)* $strname ( $internal );

        impl ::std::fmt::Display for $strname {
            fn fmt(&self, f: &mut ::std::fmt::Formatter)
                -> ::std::fmt::Result
            {
                ::std::fmt::Display::fmt(&self.0, f)
            }
        }

        impl From<$internal> for $strname {
            fn from(err: $internal) -> Self {
                $strname(err)
            }
        }

        impl ::std::error::Error for $strname {
            #[allow(deprecated)]
            fn cause(&self) -> Option<&::std::error::Error> {
                self.0.cause()
            }
        }
    };

    (SORT [enum $name:ident $( #[$meta:meta] )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [ ]
        queue [ ]
    ) => {
        quick_error!(ENUM_DEFINITION [enum $name $( #[$meta] )*]
            body []
            queue [$($( #[$imeta] )*
                      => $iitem: $imode [$( $ivar: $ityp ),*] )*]
        );
        quick_error!(IMPLEMENTATIONS $name {$(
           $iitem: $imode [$(#[$imeta])*] [$( $ivar: $ityp ),*] {$( $ifuncs )*}
           )*});
        $(
            quick_error!(ERROR_CHECK $imode $($ifuncs)*);
        )*
    };
    (SORT [pub enum $name:ident $( #[$meta:meta] )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [ ]
        queue [ ]
    ) => {
        quick_error!(ENUM_DEFINITION [pub enum $name $( #[$meta] )*]
            body []
            queue [$($( #[$imeta] )*
                      => $iitem: $imode [$( $ivar: $ityp ),*] )*]
        );
        quick_error!(IMPLEMENTATIONS $name {$(
           $iitem: $imode [$(#[$imeta])*] [$( $ivar: $ityp ),*] {$( $ifuncs )*}
           )*});
        $(
            quick_error!(ERROR_CHECK $imode $($ifuncs)*);
        )*
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )*]
        queue [ #[$qmeta:meta] $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])* => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*]
            buf [$( #[$bmeta] )* #[$qmeta] ]
            queue [$( $tail )*]);
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )*]
        queue [ $qitem:ident $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])*
                      => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*]
            buf [$(#[$bmeta])* => $qitem : UNIT [ ] ]
            queue [$( $tail )*]);
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )*
            => $bitem:ident: $bmode:tt [$( $bvar:ident: $btyp:ty ),*] ]
        queue [ #[$qmeta:meta] $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            enum [$( $(#[$emeta])* => $eitem $(( $($etyp),* ))* )*
                     $(#[$bmeta])* => $bitem: $bmode $(( $($btyp),* ))*]
            items [$($( #[$imeta:meta] )*
                      => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*
                     $bitem: $bmode [$( $bvar:$btyp ),*] {} ]
            buf [ #[$qmeta] ]
            queue [$( $tail )*]);
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )* => $bitem:ident: UNIT [ ] ]
        queue [($( $qvar:ident: $qtyp:ty ),+) $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])* => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*]
            buf [$( #[$bmeta] )* => $bitem: TUPLE [$( $qvar:$qtyp ),*] ]
            queue [$( $tail )*]
        );
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )* => $bitem:ident: UNIT [ ] ]
        queue [{ $( $qvar:ident: $qtyp:ty ),+} $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])* => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*]
            buf [$( #[$bmeta] )* => $bitem: STRUCT [$( $qvar:$qtyp ),*] ]
            queue [$( $tail )*]);
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )* => $bitem:ident: UNIT [ ] ]
        queue [{$( $qvar:ident: $qtyp:ty ),+ ,} $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])* => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*]
            buf [$( #[$bmeta] )* => $bitem: STRUCT [$( $qvar:$qtyp ),*] ]
            queue [$( $tail )*]);
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )*
                 => $bitem:ident: $bmode:tt [$( $bvar:ident: $btyp:ty ),*] ]
        queue [ {$( $qfuncs:tt )*} $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])* => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*
                      $(#[$bmeta])* => $bitem: $bmode [$( $bvar:$btyp ),*] {$( $qfuncs )*} ]
            buf [ ]
            queue [$( $tail )*]);
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )*
                 => $bitem:ident: $bmode:tt [$( $bvar:ident: $btyp:ty ),*] ]
        queue [ $qitem:ident $( $tail:tt )*]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])* => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*
                     $(#[$bmeta])* => $bitem: $bmode [$( $bvar:$btyp ),*] {} ]
            buf [ => $qitem : UNIT [ ] ]
            queue [$( $tail )*]);
    };
    (SORT [$( $def:tt )*]
        items [$($( #[$imeta:meta] )*
                  => $iitem:ident: $imode:tt [$( $ivar:ident: $ityp:ty ),*]
                                {$( $ifuncs:tt )*} )* ]
        buf [$( #[$bmeta:meta] )*
            => $bitem:ident: $bmode:tt [$( $bvar:ident: $btyp:ty ),*] ]
        queue [ ]
    ) => {
        quick_error!(SORT [$( $def )*]
            items [$( $(#[$imeta])* => $iitem: $imode [$( $ivar:$ityp ),*] {$( $ifuncs )*} )*
                     $(#[$bmeta])* => $bitem: $bmode [$( $bvar:$btyp ),*] {} ]
            buf [ ]
            queue [ ]);
    };
    (ENUM_DEFINITION [pub enum $name:ident $( #[$meta:meta] )*]
        body [$($( #[$imeta:meta] )*
            => $iitem:ident ($(($( $ttyp:ty ),+))*) {$({$( $svar:ident: $styp:ty ),*})*} )* ]
        queue [ ]
    ) => {
        #[allow(unknown_lints)]  
        #[allow(renamed_and_removed_lints)]
        #[allow(unused_doc_comment)]
        #[allow(unused_doc_comments)]
        $(#[$meta])*
        pub enum $name {
            $(
                $(#[$imeta])*
                $iitem $(($( $ttyp ),*))* $({$( $svar: $styp ),*})*,
            )*
        }
    };
    (ENUM_DEFINITION [enum $name:ident $( #[$meta:meta] )*]
        body [$($( #[$imeta:meta] )*
            => $iitem:ident ($(($( $ttyp:ty ),+))*) {$({$( $svar:ident: $styp:ty ),*})*} )* ]
        queue [ ]
    ) => {
        #[allow(unknown_lints)]  
        #[allow(renamed_and_removed_lints)]
        #[allow(unused_doc_comment)]
        #[allow(unused_doc_comments)]
        $(#[$meta])*
        enum $name {
            $(
                $(#[$imeta])*
                $iitem $(($( $ttyp ),*))* $({$( $svar: $styp ),*})*,
            )*
        }
    };
    (ENUM_DEFINITION [$( $def:tt )*]
        body [$($( #[$imeta:meta] )*
            => $iitem:ident ($(($( $ttyp:ty ),+))*) {$({$( $svar:ident: $styp:ty ),*})*} )* ]
        queue [$( #[$qmeta:meta] )*
            => $qitem:ident: UNIT [ ] $( $queue:tt )*]
    ) => {
        quick_error!(ENUM_DEFINITION [ $($def)* ]
            body [$($( #[$imeta] )* => $iitem ($(($( $ttyp ),+))*) {$({$( $svar: $styp ),*})*} )*
                    $( #[$qmeta] )* => $qitem () {} ]
            queue [ $($queue)* ]
        );
    };
    (ENUM_DEFINITION [$( $def:tt )*]
        body [$($( #[$imeta:meta] )*
            => $iitem:ident ($(($( $ttyp:ty ),+))*) {$({$( $svar:ident: $styp:ty ),*})*} )* ]
        queue [$( #[$qmeta:meta] )*
            => $qitem:ident: TUPLE [$( $qvar:ident: $qtyp:ty ),+] $( $queue:tt )*]
    ) => {
        quick_error!(ENUM_DEFINITION [ $($def)* ]
            body [$($( #[$imeta] )* => $iitem ($(($( $ttyp ),+))*) {$({$( $svar: $styp ),*})*} )*
                    $( #[$qmeta] )* => $qitem (($( $qtyp ),*)) {} ]
            queue [ $($queue)* ]
        );
    };
    (ENUM_DEFINITION [$( $def:tt )*]
        body [$($( #[$imeta:meta] )*
            => $iitem:ident ($(($( $ttyp:ty ),+))*) {$({$( $svar:ident: $styp:ty ),*})*} )* ]
        queue [$( #[$qmeta:meta] )*
            => $qitem:ident: STRUCT [$( $qvar:ident: $qtyp:ty ),*] $( $queue:tt )*]
    ) => {
        quick_error!(ENUM_DEFINITION [ $($def)* ]
            body [$($( #[$imeta] )* => $iitem ($(($( $ttyp ),+))*) {$({$( $svar: $styp ),*})*} )*
                    $( #[$qmeta] )* => $qitem () {{$( $qvar: $qtyp ),*}} ]
            queue [ $($queue)* ]
        );
    };
    (IMPLEMENTATIONS
        $name:ident {$(
            $item:ident: $imode:tt [$(#[$imeta:meta])*] [$( $var:ident: $typ:ty ),*] {$( $funcs:tt )*}
        )*}
    ) => {
        #[allow(unused)]
        #[allow(unknown_lints)]  
        #[allow(renamed_and_removed_lints)]
        #[allow(unused_doc_comment)]
        #[allow(unused_doc_comments)]
        impl ::std::fmt::Display for $name {
            fn fmt(&self, fmt: &mut ::std::fmt::Formatter)
                -> ::std::fmt::Result
            {
                match *self {
                    $(
                        $(#[$imeta])*
                        quick_error!(ITEM_PATTERN
                            $name $item: $imode [$( ref $var ),*]
                        ) => {
                            let display_fn = quick_error!(FIND_DISPLAY_IMPL
                                $name $item: $imode
                                {$( $funcs )*});

                            display_fn(self, fmt)
                        }
                    )*
                }
            }
        }
        #[allow(unused)]
        #[allow(unknown_lints)]  
        #[allow(renamed_and_removed_lints)]
        #[allow(unused_doc_comment)]
        #[allow(unused_doc_comments)]
        impl ::std::error::Error for $name {
            fn cause(&self) -> Option<&::std::error::Error> {
                match *self {
                    $(
                        $(#[$imeta])*
                        quick_error!(ITEM_PATTERN
                            $name $item: $imode [$( ref $var ),*]
                        ) => {
                            quick_error!(FIND_CAUSE_IMPL
                                $item: $imode [$( $var ),*]
                                {$( $funcs )*})
                        }
                    )*
                }
            }
        }
        $(
            quick_error!(FIND_FROM_IMPL
                $name $item: $imode [$( $var:$typ ),*]
                {$( $funcs )*});
        )*
        $(
            quick_error!(FIND_CONTEXT_IMPL
                $name $item: $imode [$( $var:$typ ),*]
                {$( $funcs )*});
        )*
    };
    (FIND_DISPLAY_IMPL $name:ident $item:ident: $imode:tt
        { display($self_:tt) -> ($( $exprs:tt )*) $( $tail:tt )*}
    ) => {
        |quick_error!(IDENT $self_): &$name, f: &mut ::std::fmt::Formatter| { write!(f, $( $exprs )*) }
    };
    (FIND_DISPLAY_IMPL $name:ident $item:ident: $imode:tt
        { display($pattern:expr) $( $tail:tt )*}
    ) => {
        |_, f: &mut ::std::fmt::Formatter| { write!(f, $pattern) }
    };
    (FIND_DISPLAY_IMPL $name:ident $item:ident: $imode:tt
        { display($pattern:expr, $( $exprs:tt )*) $( $tail:tt )*}
    ) => {
        |_, f: &mut ::std::fmt::Formatter| { write!(f, $pattern, $( $exprs )*) }
    };
    (FIND_DISPLAY_IMPL $name:ident $item:ident: $imode:tt
        { $t:tt $( $tail:tt )*}
    ) => {
        quick_error!(FIND_DISPLAY_IMPL
            $name $item: $imode
            {$( $tail )*})
    };
    (FIND_DISPLAY_IMPL $name:ident $item:ident: $imode:tt
        { }
    ) => {
        |self_: &$name, f: &mut ::std::fmt::Formatter| {
            write!(f, "{:?}", self_)
        }
    };
    (FIND_DESCRIPTION_IMPL $item:ident: $imode:tt $me:ident $fmt:ident
        [$( $var:ident ),*]
        { description($expr:expr) $( $tail:tt )*}
    ) => {};
    (FIND_DESCRIPTION_IMPL $item:ident: $imode:tt $me:ident $fmt:ident
        [$( $var:ident ),*]
        { $t:tt $( $tail:tt )*}
    ) => {};
    (FIND_DESCRIPTION_IMPL $item:ident: $imode:tt $me:ident $fmt:ident
        [$( $var:ident ),*]
        { }
    ) => {};
    (FIND_CAUSE_IMPL $item:ident: $imode:tt
        [$( $var:ident ),*]
        { cause($expr:expr) $( $tail:tt )*}
    ) => {
        Some($expr)
    };
    (FIND_CAUSE_IMPL $item:ident: $imode:tt
        [$( $var:ident ),*]
        { $t:tt $( $tail:tt )*}
    ) => {
        quick_error!(FIND_CAUSE_IMPL
            $item: $imode [$( $var ),*]
            { $($tail)* })
    };
    (FIND_CAUSE_IMPL $item:ident: $imode:tt
        [$( $var:ident ),*]
        { }
    ) => {
        None
    };
    (FIND_FROM_IMPL $name:ident $item:ident: $imode:tt
        [$( $var:ident: $typ:ty ),*]
        { from() $( $tail:tt )*}
    ) => {
        $(
            impl From<$typ> for $name {
                fn from($var: $typ) -> $name {
                    $name::$item($var)
                }
            }
        )*
        quick_error!(FIND_FROM_IMPL
            $name $item: $imode [$( $var:$typ ),*]
            {$( $tail )*});
    };
    (FIND_FROM_IMPL $name:ident $item:ident: UNIT
        [ ]
        { from($ftyp:ty) $( $tail:tt )*}
    ) => {
        impl From<$ftyp> for $name {
            fn from(_discarded_error: $ftyp) -> $name {
                $name::$item
            }
        }
        quick_error!(FIND_FROM_IMPL
            $name $item: UNIT [  ]
            {$( $tail )*});
    };
    (FIND_FROM_IMPL $name:ident $item:ident: TUPLE
        [$( $var:ident: $typ:ty ),*]
        { from($fvar:ident: $ftyp:ty) -> ($( $texpr:expr ),*) $( $tail:tt )*}
    ) => {
        impl From<$ftyp> for $name {
            fn from($fvar: $ftyp) -> $name {
                $name::$item($( $texpr ),*)
            }
        }
        quick_error!(FIND_FROM_IMPL
            $name $item: TUPLE [$( $var:$typ ),*]
            { $($tail)* });
    };
    (FIND_FROM_IMPL $name:ident $item:ident: STRUCT
        [$( $var:ident: $typ:ty ),*]
        { from($fvar:ident: $ftyp:ty) -> {$( $tvar:ident: $texpr:expr ),*} $( $tail:tt )*}
    ) => {
        impl From<$ftyp> for $name {
            fn from($fvar: $ftyp) -> $name {
                $name::$item {
                    $( $tvar: $texpr ),*
                }
            }
        }
        quick_error!(FIND_FROM_IMPL
            $name $item: STRUCT [$( $var:$typ ),*]
            { $($tail)* });
    };
    (FIND_FROM_IMPL $name:ident $item:ident: $imode:tt
        [$( $var:ident: $typ:ty ),*]
        { $t:tt $( $tail:tt )*}
    ) => {
        quick_error!(FIND_FROM_IMPL
            $name $item: $imode [$( $var:$typ ),*]
            {$( $tail )*}
        );
    };
    (FIND_FROM_IMPL $name:ident $item:ident: $imode:tt
        [$( $var:ident: $typ:ty ),*]
        { }
    ) => {
    };
    (FIND_CONTEXT_IMPL $name:ident $item:ident: TUPLE
        [$( $var:ident: $typ:ty ),*]
        { context($cvar:ident: AsRef<$ctyp:ty>, $fvar:ident: $ftyp:ty)
            -> ($( $texpr:expr ),*) $( $tail:tt )* }
    ) => {
        impl<T: AsRef<$ctyp>> From<$crate::Context<T, $ftyp>> for $name {
            fn from(
                $crate::Context($cvar, $fvar): $crate::Context<T, $ftyp>)
                -> $name
            {
                $name::$item($( $texpr ),*)
            }
        }
        quick_error!(FIND_CONTEXT_IMPL
            $name $item: TUPLE [$( $var:$typ ),*]
            { $($tail)* });
    };
    (FIND_CONTEXT_IMPL $name:ident $item:ident: TUPLE
        [$( $var:ident: $typ:ty ),*]
        { context($cvar:ident: $ctyp:ty, $fvar:ident: $ftyp:ty)
            -> ($( $texpr:expr ),*) $( $tail:tt )* }
    ) => {
        impl<'a> From<$crate::Context<$ctyp, $ftyp>> for $name {
            fn from(
                $crate::Context($cvar, $fvar): $crate::Context<$ctyp, $ftyp>)
                -> $name
            {
                $name::$item($( $texpr ),*)
            }
        }
        quick_error!(FIND_CONTEXT_IMPL
            $name $item: TUPLE [$( $var:$typ ),*]
            { $($tail)* });
    };
    (FIND_CONTEXT_IMPL $name:ident $item:ident: STRUCT
        [$( $var:ident: $typ:ty ),*]
        { context($cvar:ident: AsRef<$ctyp:ty>, $fvar:ident: $ftyp:ty)
            -> {$( $tvar:ident: $texpr:expr ),*} $( $tail:tt )* }
    ) => {
        impl<T: AsRef<$ctyp>> From<$crate::Context<T, $ftyp>> for $name {
            fn from(
                $crate::Context($cvar, $fvar): $crate::Context<$ctyp, $ftyp>)
                -> $name
            {
                $name::$item {
                    $( $tvar: $texpr ),*
                }
            }
        }
        quick_error!(FIND_CONTEXT_IMPL
            $name $item: STRUCT [$( $var:$typ ),*]
            { $($tail)* });
    };
    (FIND_CONTEXT_IMPL $name:ident $item:ident: STRUCT
        [$( $var:ident: $typ:ty ),*]
        { context($cvar:ident: $ctyp:ty, $fvar:ident: $ftyp:ty)
            -> {$( $tvar:ident: $texpr:expr ),*} $( $tail:tt )* }
    ) => {
        impl<'a> From<$crate::Context<$ctyp, $ftyp>> for $name {
            fn from(
                $crate::Context($cvar, $fvar): $crate::Context<$ctyp, $ftyp>)
                -> $name
            {
                $name::$item {
                    $( $tvar: $texpr ),*
                }
            }
        }
        quick_error!(FIND_CONTEXT_IMPL
            $name $item: STRUCT [$( $var:$typ ),*]
            { $($tail)* });
    };
    (FIND_CONTEXT_IMPL $name:ident $item:ident: $imode:tt
        [$( $var:ident: $typ:ty ),*]
        { $t:tt $( $tail:tt )*}
    ) => {
        quick_error!(FIND_CONTEXT_IMPL
            $name $item: $imode [$( $var:$typ ),*]
            {$( $tail )*}
        );
    };
    (FIND_CONTEXT_IMPL $name:ident $item:ident: $imode:tt
        [$( $var:ident: $typ:ty ),*]
        { }
    ) => {
    };
    (ITEM_BODY $(#[$imeta:meta])* $item:ident: UNIT
    ) => { };
    (ITEM_BODY $(#[$imeta:meta])* $item:ident: TUPLE
        [$( $typ:ty ),*]
    ) => {
        ($( $typ ),*)
    };
    (ITEM_BODY $(#[$imeta:meta])* $item:ident: STRUCT
        [$( $var:ident: $typ:ty ),*]
    ) => {
        {$( $var:$typ ),*}
    };
    (ITEM_PATTERN $name:ident $item:ident: UNIT []
    ) => {
        $name::$item
    };
    (ITEM_PATTERN $name:ident $item:ident: TUPLE
        [$( ref $var:ident ),*]
    ) => {
        $name::$item ($( ref $var ),*)
    };
    (ITEM_PATTERN $name:ident $item:ident: STRUCT
        [$( ref $var:ident ),*]
    ) => {
        $name::$item {$( ref $var ),*}
    };
    (ERROR_CHECK $imode:tt display($self_:tt) -> ($( $exprs:tt )*) $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK $imode $($tail)*); };
    (ERROR_CHECK $imode:tt display($pattern: expr) $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK $imode $($tail)*); };
    (ERROR_CHECK $imode:tt display($pattern: expr, $( $exprs:tt )*) $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK $imode $($tail)*); };
    (ERROR_CHECK $imode:tt description($expr:expr) $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK $imode $($tail)*); };
    (ERROR_CHECK $imode:tt cause($expr:expr) $($tail:tt)*)
    => { quick_error!(ERROR_CHECK $imode $($tail)*); };
    (ERROR_CHECK $imode:tt from() $($tail:tt)*)
    => { quick_error!(ERROR_CHECK $imode $($tail)*); };
    (ERROR_CHECK $imode:tt from($ftyp:ty) $($tail:tt)*)
    => { quick_error!(ERROR_CHECK $imode $($tail)*); };
    (ERROR_CHECK TUPLE from($fvar:ident: $ftyp:ty) -> ($( $e:expr ),*) $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK TUPLE $($tail)*); };
    (ERROR_CHECK STRUCT from($fvar:ident: $ftyp:ty) -> {$( $v:ident: $e:expr ),*} $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK STRUCT $($tail)*); };

    (ERROR_CHECK TUPLE context($cvar:ident: $ctyp:ty, $fvar:ident: $ftyp:ty)
        -> ($( $e:expr ),*) $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK TUPLE $($tail)*); };
    (ERROR_CHECK STRUCT context($cvar:ident: $ctyp:ty, $fvar:ident: $ftyp:ty)
        -> {$( $v:ident: $e:expr ),*} $( $tail:tt )*)
    => { quick_error!(ERROR_CHECK STRUCT $($tail)*); };

    (ERROR_CHECK $imode:tt ) => {};
    (IDENT $ident:ident) => { $ident }
}


/// Generic context type
///
/// Used mostly as a transport for `ResultExt::context` method
#[derive(Debug)]
pub struct Context<X, E>(pub X, pub E);

/// Result extension trait adding a `context` method
pub trait ResultExt<T, E> {
    /// The method is use to add context information to current operation
    ///
    /// The context data is then used in error constructor to store additional
    /// information within error. For example, you may add a filename as a
    /// context for file operation. See crate documentation for the actual
    /// example.
    fn context<X>(self, x: X) -> Result<T, Context<X, E>>;
}

impl<T, E> ResultExt<T, E> for Result<T, E> {
    fn context<X>(self, x: X) -> Result<T, Context<X, E>> {
        self.map_err(|e| Context(x, e))
    }
}
