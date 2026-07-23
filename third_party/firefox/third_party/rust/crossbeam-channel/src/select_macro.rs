//! The `select!` macro.

/// A helper macro for `select!` to hide the long list of macro patterns from the documentation.
///
/// The macro consists of two stages:
/// 1. Parsing
/// 2. Code generation
///
/// The parsing stage consists of these subparts:
/// 1. `@list`: Turns a list of tokens into a list of cases.
/// 2. `@list_errorN`: Diagnoses the syntax error.
/// 3. `@case`: Parses a single case and verifies its argument list.
///
/// The codegen stage consists of these subparts:
/// 1. `@init`: Attempts to optimize `select!` away and initializes the list of handles.
/// 1. `@count`: Counts the listed cases.
/// 3. `@add`: Adds send/receive operations to the list of handles and starts selection.
/// 4. `@complete`: Completes the selected send/receive operation.
///
/// If the parsing stage encounters a syntax error or the codegen stage ends up with too many
/// cases to process, the macro fails with a compile-time error.
#[doc(hidden)]
#[macro_export]
macro_rules! crossbeam_channel_internal {
    (@list
        ()
        ($($head:tt)*)
    ) => {
        $crate::crossbeam_channel_internal!(
            @case
            ($($head)*)
            ()
            ()
        )
    };
    (@list
        (default => $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        $crate::crossbeam_channel_internal!(
            @list
            (default() => $($tail)*)
            ($($head)*)
        )
    };
    (@list
        (default -> $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        compile_error!(
            "expected `=>` after `default` case, found `->`"
        )
    };
    (@list
        (default $args:tt -> $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        compile_error!(
            "expected `=>` after `default` case, found `->`"
        )
    };
    (@list
        (recv($($args:tt)*) => $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        compile_error!(
            "expected `->` after `recv` case, found `=>`"
        )
    };
    (@list
        (send($($args:tt)*) => $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        compile_error!(
            "expected `->` after `send` operation, found `=>`"
        )
    };
    (@list
        ($case:ident $args:tt -> $res:tt -> $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        compile_error!("expected `=>`, found `->`")
    };
    (@list
        ($case:ident $args:tt $(-> $res:pat)* => $body:block; $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        compile_error!(
            "did you mean to put a comma instead of the semicolon after `}`?"
        )
    };
    (@list
        ($case:ident ($($args:tt)*) $(-> $res:pat)* => $body:expr, $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        $crate::crossbeam_channel_internal!(
            @list
            ($($tail)*)
            ($($head)* $case ($($args)*) $(-> $res)* => { $body },)
        )
    };
    (@list
        ($case:ident ($($args:tt)*) $(-> $res:pat)* => $body:block $($tail:tt)*)
        ($($head:tt)*)
    ) => {
        $crate::crossbeam_channel_internal!(
            @list
            ($($tail)*)
            ($($head)* $case ($($args)*) $(-> $res)* => { $body },)
        )
    };
    (@list
        ($case:ident ($($args:tt)*) $(-> $res:pat)* => $body:expr $(,)?)
        ($($head:tt)*)
    ) => {
        $crate::crossbeam_channel_internal!(
            @list
            ()
            ($($head)* $case ($($args)*) $(-> $res)* => { $body },)
        )
    };
    (@list
        ($($tail:tt)*)
        ($($head:tt)*)
    ) => {
        $crate::crossbeam_channel_internal!(@list_error1 $($tail)*)
    };
    (@list_error1 recv $($tail:tt)*) => {
        $crate::crossbeam_channel_internal!(@list_error2 recv $($tail)*)
    };
    (@list_error1 send $($tail:tt)*) => {
        $crate::crossbeam_channel_internal!(@list_error2 send $($tail)*)
    };
    (@list_error1 default $($tail:tt)*) => {
        $crate::crossbeam_channel_internal!(@list_error2 default $($tail)*)
    };
    (@list_error1 $t:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected one of `recv`, `send`, or `default`, found `",
                stringify!($t),
                "`",
            )
        )
    };
    (@list_error1 $($tail:tt)*) => {
        $crate::crossbeam_channel_internal!(@list_error2 $($tail)*);
    };
    (@list_error2 $case:ident) => {
        compile_error!(
            concat!(
                "missing argument list after `",
                stringify!($case),
                "`",
            )
        )
    };
    (@list_error2 $case:ident => $($tail:tt)*) => {
        compile_error!(
            concat!(
                "missing argument list after `",
                stringify!($case),
                "`",
            )
        )
    };
    (@list_error2 $($tail:tt)*) => {
        $crate::crossbeam_channel_internal!(@list_error3 $($tail)*)
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)*) => {
        compile_error!(
            concat!(
                "missing `=>` after `",
                stringify!($case),
                "` case",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* =>) => {
        compile_error!(
            "expected expression after `=>`"
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => $body:expr; $($tail:tt)*) => {
        compile_error!(
            concat!(
                "did you mean to put a comma instead of the semicolon after `",
                stringify!($body),
                "`?",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => recv($($a:tt)*) $($tail:tt)*) => {
        compile_error!(
            "expected an expression after `=>`"
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => send($($a:tt)*) $($tail:tt)*) => {
        compile_error!(
            "expected an expression after `=>`"
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => default($($a:tt)*) $($tail:tt)*) => {
        compile_error!(
            "expected an expression after `=>`"
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => $f:ident($($a:tt)*) $($tail:tt)*) => {
        compile_error!(
            concat!(
                "did you mean to put a comma after `",
                stringify!($f),
                "(",
                stringify!($($a)*),
                ")`?",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => $f:ident!($($a:tt)*) $($tail:tt)*) => {
        compile_error!(
            concat!(
                "did you mean to put a comma after `",
                stringify!($f),
                "!(",
                stringify!($($a)*),
                ")`?",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => $f:ident![$($a:tt)*] $($tail:tt)*) => {
        compile_error!(
            concat!(
                "did you mean to put a comma after `",
                stringify!($f),
                "![",
                stringify!($($a)*),
                "]`?",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => $f:ident!{$($a:tt)*} $($tail:tt)*) => {
        compile_error!(
            concat!(
                "did you mean to put a comma after `",
                stringify!($f),
                "!{",
                stringify!($($a)*),
                "}`?",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) $(-> $r:pat)* => $body:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "did you mean to put a comma after `",
                stringify!($body),
                "`?",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) -> => $($tail:tt)*) => {
        compile_error!("missing pattern after `->`")
    };
    (@list_error3 $case:ident($($args:tt)*) $t:tt $(-> $r:pat)* => $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected `->`, found `",
                stringify!($t),
                "`",
            )
        )
    };
    (@list_error3 $case:ident($($args:tt)*) -> $t:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected a pattern, found `",
                stringify!($t),
                "`",
            )
        )
    };
    (@list_error3 recv($($args:tt)*) $t:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected `->`, found `",
                stringify!($t),
                "`",
            )
        )
    };
    (@list_error3 send($($args:tt)*) $t:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected `->`, found `",
                stringify!($t),
                "`",
            )
        )
    };
    (@list_error3 recv $args:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected an argument list after `recv`, found `",
                stringify!($args),
                "`",
            )
        )
    };
    (@list_error3 send $args:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected an argument list after `send`, found `",
                stringify!($args),
                "`",
            )
        )
    };
    (@list_error3 default $args:tt $($tail:tt)*) => {
        compile_error!(
            concat!(
                "expected an argument list or `=>` after `default`, found `",
                stringify!($args),
                "`",
            )
        )
    };
    (@list_error3 $($tail:tt)*) => {
        $crate::crossbeam_channel_internal!(@list_error4 $($tail)*)
    };
    (@list_error4 $($tail:tt)*) => {
        compile_error!("invalid syntax")
    };

    (@case
        ()
        $cases:tt
        $default:tt
    ) => {
        $crate::crossbeam_channel_internal!(
            @init
            $cases
            $default
        )
    };

    (@case
        (recv($r:expr $(,)?) -> $res:pat => $body:tt, $($tail:tt)*)
        ($($cases:tt)*)
        $default:tt
    ) => {
        $crate::crossbeam_channel_internal!(
            @case
            ($($tail)*)
            ($($cases)* recv($r) -> $res => $body,)
            $default
        )
    };
    (@case
        (recv($($args:tt)*) -> $res:pat => $body:tt, $($tail:tt)*)
        ($($cases:tt)*)
        $default:tt
    ) => {
        compile_error!(
            concat!(
                "invalid argument list in `recv(",
                stringify!($($args)*),
                ")`",
            )
        )
    };
    (@case
        (recv $t:tt $($tail:tt)*)
        ($($cases:tt)*)
        $default:tt
    ) => {
        compile_error!(
            concat!(
                "expected an argument list after `recv`, found `",
                stringify!($t),
                "`",
            )
        )
    };

    (@case
        (send($s:expr, $m:expr $(,)?) -> $res:pat => $body:tt, $($tail:tt)*)
        ($($cases:tt)*)
        $default:tt
    ) => {
        $crate::crossbeam_channel_internal!(
            @case
            ($($tail)*)
            ($($cases)* send($s, $m) -> $res => $body,)
            $default
        )
    };
    (@case
        (send($($args:tt)*) -> $res:pat => $body:tt, $($tail:tt)*)
        ($($cases:tt)*)
        $default:tt
    ) => {
        compile_error!(
            concat!(
                "invalid argument list in `send(",
                stringify!($($args)*),
                ")`",
            )
        )
    };
    (@case
        (send $t:tt $($tail:tt)*)
        ($($cases:tt)*)
        $default:tt
    ) => {
        compile_error!(
            concat!(
                "expected an argument list after `send`, found `",
                stringify!($t),
                "`",
            )
        )
    };

    (@case
        (default() => $body:tt, $($tail:tt)*)
        $cases:tt
        ()
    ) => {
        $crate::crossbeam_channel_internal!(
            @case
            ($($tail)*)
            $cases
            (default() => $body,)
        )
    };
    (@case
        (default($timeout:expr $(,)?) => $body:tt, $($tail:tt)*)
        $cases:tt
        ()
    ) => {
        $crate::crossbeam_channel_internal!(
            @case
            ($($tail)*)
            $cases
            (default($timeout) => $body,)
        )
    };
    (@case
        (default $($tail:tt)*)
        $cases:tt
        ($($def:tt)+)
    ) => {
        compile_error!(
            "there can be only one `default` case in a `select!` block"
        )
    };
    (@case
        (default($($args:tt)*) => $body:tt, $($tail:tt)*)
        $cases:tt
        $default:tt
    ) => {
        compile_error!(
            concat!(
                "invalid argument list in `default(",
                stringify!($($args)*),
                ")`",
            )
        )
    };
    (@case
        (default $t:tt $($tail:tt)*)
        $cases:tt
        $default:tt
    ) => {
        compile_error!(
            concat!(
                "expected an argument list or `=>` after `default`, found `",
                stringify!($t),
                "`",
            )
        )
    };

    (@case
        ($case:ident $($tail:tt)*)
        $cases:tt
        $default:tt
    ) => {
        compile_error!(
            concat!(
                "expected one of `recv`, `send`, or `default`, found `",
                stringify!($case),
                "`",
            )
        )
    };

    (@init
        (recv($r:expr) -> $res:pat => $recv_body:tt,)
        (default() => $default_body:tt,)
    ) => {{
        match $r {
            ref _r => {
                let _r: &$crate::Receiver<_> = _r;
                match _r.try_recv() {
                    ::std::result::Result::Err($crate::TryRecvError::Empty) => {
                        $default_body
                    }
                    _res => {
                        let _res = _res.map_err(|_| $crate::RecvError);
                        let $res = _res;
                        $recv_body
                    }
                }
            }
        }
    }};
    (@init
        (recv($r:expr) -> $res:pat => $body:tt,)
        ()
    ) => {{
        match $r {
            ref _r => {
                let _r: &$crate::Receiver<_> = _r;
                let _res = _r.recv();
                let $res = _res;
                $body
            }
        }
    }};
    (@init
        (recv($r:expr) -> $res:pat => $recv_body:tt,)
        (default($timeout:expr) => $default_body:tt,)
    ) => {{
        match $r {
            ref _r => {
                let _r: &$crate::Receiver<_> = _r;
                match _r.recv_timeout($timeout) {
                    ::std::result::Result::Err($crate::RecvTimeoutError::Timeout) => {
                        $default_body
                    }
                    _res => {
                        let _res = _res.map_err(|_| $crate::RecvError);
                        let $res = _res;
                        $recv_body
                    }
                }
            }
        }
    }};



    (@init
        ($($cases:tt)*)
        $default:tt
    ) => {{
        const _LEN: usize = $crate::crossbeam_channel_internal!(@count ($($cases)*));
        let _handle: &dyn $crate::internal::SelectHandle = &$crate::never::<()>();

        #[allow(unused_mut, clippy::zero_repeat_side_effects)]
        let mut _sel = [(_handle, 0, ::std::ptr::null()); _LEN];

        $crate::crossbeam_channel_internal!(
            @add
            _sel
            ($($cases)*)
            $default
            (
                (0usize _oper0)
                (1usize _oper1)
                (2usize _oper2)
                (3usize _oper3)
                (4usize _oper4)
                (5usize _oper5)
                (6usize _oper6)
                (7usize _oper7)
                (8usize _oper8)
                (9usize _oper9)
                (10usize _oper10)
                (11usize _oper11)
                (12usize _oper12)
                (13usize _oper13)
                (14usize _oper14)
                (15usize _oper15)
                (16usize _oper16)
                (17usize _oper17)
                (18usize _oper18)
                (19usize _oper19)
                (20usize _oper20)
                (21usize _oper21)
                (22usize _oper22)
                (23usize _oper23)
                (24usize _oper24)
                (25usize _oper25)
                (26usize _oper26)
                (27usize _oper27)
                (28usize _oper28)
                (29usize _oper29)
                (30usize _oper30)
                (31usize _oper31)
            )
            ()
        )
    }};

    (@count ()) => {
        0
    };
    (@count ($oper:ident $args:tt -> $res:pat => $body:tt, $($cases:tt)*)) => {
        1 + $crate::crossbeam_channel_internal!(@count ($($cases)*))
    };

    (@add
        $sel:ident
        ()
        ()
        $labels:tt
        $cases:tt
    ) => {{
        let _oper: $crate::SelectedOperation<'_> = {
            let _oper = $crate::internal::select(&mut $sel, _IS_BIASED);

            unsafe { ::std::mem::transmute(_oper) }
        };

        $crate::crossbeam_channel_internal! {
            @complete
            $sel
            _oper
            $cases
        }
    }};
    (@add
        $sel:ident
        ()
        (default() => $body:tt,)
        $labels:tt
        $cases:tt
    ) => {{
        let _oper: ::std::option::Option<$crate::SelectedOperation<'_>> = {
            let _oper = $crate::internal::try_select(&mut $sel, _IS_BIASED);

            unsafe { ::std::mem::transmute(_oper) }
        };

        match _oper {
            None => {
                { $sel };
                $body
            }
            Some(_oper) => {
                $crate::crossbeam_channel_internal! {
                    @complete
                    $sel
                    _oper
                    $cases
                }
            }
        }
    }};
    (@add
        $sel:ident
        ()
        (default($timeout:expr) => $body:tt,)
        $labels:tt
        $cases:tt
    ) => {{
        let _oper: ::std::option::Option<$crate::SelectedOperation<'_>> = {
            let _oper = $crate::internal::select_timeout(&mut $sel, $timeout, _IS_BIASED);

            unsafe { ::std::mem::transmute(_oper) }
        };

        match _oper {
            ::std::option::Option::None => {
                { $sel };
                $body
            }
            ::std::option::Option::Some(_oper) => {
                $crate::crossbeam_channel_internal! {
                    @complete
                    $sel
                    _oper
                    $cases
                }
            }
        }
    }};
    (@add
        $sel:ident
        $input:tt
        $default:tt
        ()
        $cases:tt
    ) => {
        compile_error!("too many operations in a `select!` block")
    };
    (@add
        $sel:ident
        (recv($r:expr) -> $res:pat => $body:tt, $($tail:tt)*)
        $default:tt
        (($i:tt $var:ident) $($labels:tt)*)
        ($($cases:tt)*)
    ) => {{
        match $r {
            ref _r => {
                let $var: &$crate::Receiver<_> = unsafe {
                    let _r: &$crate::Receiver<_> = _r;

                    unsafe fn unbind<'a, T>(x: &T) -> &'a T {
                        ::std::mem::transmute(x)
                    }
                    unbind(_r)
                };
                $sel[$i] = ($var, $i, $var as *const $crate::Receiver<_> as *const u8);

                $crate::crossbeam_channel_internal!(
                    @add
                    $sel
                    ($($tail)*)
                    $default
                    ($($labels)*)
                    ($($cases)* [$i] recv($var) -> $res => $body,)
                )
            }
        }
    }};
    (@add
        $sel:ident
        (send($s:expr, $m:expr) -> $res:pat => $body:tt, $($tail:tt)*)
        $default:tt
        (($i:tt $var:ident) $($labels:tt)*)
        ($($cases:tt)*)
    ) => {{
        match $s {
            ref _s => {
                let $var: &$crate::Sender<_> = unsafe {
                    let _s: &$crate::Sender<_> = _s;

                    unsafe fn unbind<'a, T>(x: &T) -> &'a T {
                        ::std::mem::transmute(x)
                    }
                    unbind(_s)
                };
                $sel[$i] = ($var, $i, $var as *const $crate::Sender<_> as *const u8);

                $crate::crossbeam_channel_internal!(
                    @add
                    $sel
                    ($($tail)*)
                    $default
                    ($($labels)*)
                    ($($cases)* [$i] send($var, $m) -> $res => $body,)
                )
            }
        }
    }};

    (@complete
        $sel:ident
        $oper:ident
        ([$i:tt] recv($r:ident) -> $res:pat => $body:tt, $($tail:tt)*)
    ) => {{
        if $oper.index() == $i {
            let _res = $oper.recv($r);
            { $sel };

            let $res = _res;
            $body
        } else {
            $crate::crossbeam_channel_internal! {
                @complete
                $sel
                $oper
                ($($tail)*)
            }
        }
    }};
    (@complete
        $sel:ident
        $oper:ident
        ([$i:tt] send($s:ident, $m:expr) -> $res:pat => $body:tt, $($tail:tt)*)
    ) => {{
        if $oper.index() == $i {
            let _res = $oper.send($s, $m);
            { $sel };

            let $res = _res;
            $body
        } else {
            $crate::crossbeam_channel_internal! {
                @complete
                $sel
                $oper
                ($($tail)*)
            }
        }
    }};
    (@complete
        $sel:ident
        $oper:ident
        ()
    ) => {{
        unreachable!(
            "internal error in crossbeam-channel: invalid case"
        )
    }};

    (@$($tokens:tt)*) => {
        compile_error!(
            concat!(
                "internal error in crossbeam-channel: ",
                stringify!(@$($tokens)*),
            )
        )
    };

    () => {
        compile_error!("empty `select!` block")
    };
    ($($case:ident $(($($args:tt)*))* => $body:expr $(,)*)*) => {
        $crate::crossbeam_channel_internal!(
            @list
            ($($case $(($($args)*))* => { $body },)*)
            ()
        )
    };
    ($($tokens:tt)*) => {
        $crate::crossbeam_channel_internal!(
            @list
            ($($tokens)*)
            ()
        )
    };
}

/// Selects from a set of channel operations.
///
/// This macro allows you to define a set of channel operations, wait until any one of them becomes
/// ready, and finally execute it. If multiple operations are ready at the same time, a random one
/// among them is selected (i.e. the unbiased selection). Use `select_biased!` for the biased
/// selection.
///
/// It is also possible to define a `default` case that gets executed if none of the operations are
/// ready, either right away or for a certain duration of time.
///
/// An operation is considered to be ready if it doesn't have to block. Note that it is ready even
/// when it will simply return an error because the channel is disconnected.
///
/// The `select!` macro is a convenience wrapper around [`Select`]. However, it cannot select over a
/// dynamically created list of channel operations.
///
/// [`Select`]: super::Select
///
/// # Examples
///
/// Block until a send or a receive operation is selected:
///
/// ```
/// use crossbeam_channel::{select, unbounded};
///
/// let (s1, r1) = unbounded();
/// let (s2, r2) = unbounded();
/// s1.send(10).unwrap();
///
/// // Since both operations are initially ready, a random one will be executed.
/// select! {
///     recv(r1) -> msg => assert_eq!(msg, Ok(10)),
///     send(s2, 20) -> res => {
///         assert_eq!(res, Ok(()));
///         assert_eq!(r2.recv(), Ok(20));
///     }
/// }
/// ```
///
/// Select from a set of operations without blocking:
///
/// ```
/// use std::thread;
/// use std::time::Duration;
/// use crossbeam_channel::{select, unbounded};
///
/// let (s1, r1) = unbounded();
/// let (s2, r2) = unbounded();
///
/// thread::spawn(move || {
///     thread::sleep(Duration::from_secs(1));
///     s1.send(10).unwrap();
/// });
/// thread::spawn(move || {
///     thread::sleep(Duration::from_millis(500));
///     s2.send(20).unwrap();
/// });
///
/// // None of the operations are initially ready.
/// select! {
///     recv(r1) -> msg => panic!(),
///     recv(r2) -> msg => panic!(),
///     default => println!("not ready"),
/// }
/// ```
///
/// Select over a set of operations with a timeout:
///
/// ```
/// use std::thread;
/// use std::time::Duration;
/// use crossbeam_channel::{select, unbounded};
///
/// let (s1, r1) = unbounded();
/// let (s2, r2) = unbounded();
///
/// thread::spawn(move || {
///     thread::sleep(Duration::from_secs(1));
///     s1.send(10).unwrap();
/// });
/// thread::spawn(move || {
///     thread::sleep(Duration::from_millis(500));
///     s2.send(20).unwrap();
/// });
///
/// // None of the two operations will become ready within 100 milliseconds.
/// select! {
///     recv(r1) -> msg => panic!(),
///     recv(r2) -> msg => panic!(),
///     default(Duration::from_millis(100)) => println!("timed out"),
/// }
/// ```
///
/// Optionally add a receive operation to `select!` using [`never`]:
///
/// ```
/// use std::thread;
/// use std::time::Duration;
/// use crossbeam_channel::{select, never, unbounded};
///
/// let (s1, r1) = unbounded();
/// let (s2, r2) = unbounded();
///
/// thread::spawn(move || {
///     thread::sleep(Duration::from_secs(1));
///     s1.send(10).unwrap();
/// });
/// thread::spawn(move || {
///     thread::sleep(Duration::from_millis(500));
///     s2.send(20).unwrap();
/// });
///
/// // This receiver can be a `Some` or a `None`.
/// let r2 = Some(&r2);
///
/// // None of the two operations will become ready within 100 milliseconds.
/// select! {
///     recv(r1) -> msg => panic!(),
///     recv(r2.unwrap_or(&never())) -> msg => assert_eq!(msg, Ok(20)),
/// }
/// ```
///
/// To optionally add a timeout to `select!`, see the [example] for [`never`].
///
/// [`never`]: super::never
/// [example]: super::never#examples
#[macro_export]
macro_rules! select {
    ($($tokens:tt)*) => {
        {
            const _IS_BIASED: bool = false;

            $crate::crossbeam_channel_internal!(
                $($tokens)*
            )
        }
    };
}

/// Selects from a set of channel operations.
///
/// This macro allows you to define a list of channel operations, wait until any one of them
/// becomes ready, and finally execute it. If multiple operations are ready at the same time, the
/// operation nearest to the front of the list is always selected (i.e. the biased selection). Use
/// [`select!`] for the unbiased selection.
///
/// Otherwise, this macro's functionality is identical to [`select!`]. Refer to it for the syntax.
#[macro_export]
macro_rules! select_biased {
    ($($tokens:tt)*) => {
        {
            const _IS_BIASED: bool = true;

            $crate::crossbeam_channel_internal!(
                $($tokens)*
            )
        }
    };
}
