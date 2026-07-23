//! A library for [Cargo build scripts](https://doc.rust-lang.org/cargo/reference/build-scripts.html)
//! to compile a set of C/C++/assembly/CUDA files into a static archive for Cargo
//! to link into the crate being built. This crate does not compile code itself;
//! it calls out to the default compiler for the platform. This crate will
//! automatically detect situations such as cross compilation and
//! [various environment variables](#external-configuration-via-environment-variables) and will build code appropriately.
//!
//! # Example
//!
//! First, you'll want to both add a build script for your crate (`build.rs`) and
//! also add this crate to your `Cargo.toml` via:
//!
//! ```toml
//! [build-dependencies]
//! cc = "1.0"
//! ```
//!
//! Next up, you'll want to write a build script like so:
//!
//! ```rust,no_run
//! // build.rs
//! cc::Build::new()
//!     .file("foo.c")
//!     .file("bar.c")
//!     .compile("foo");
//! ```
//!
//! And that's it! Running `cargo build` should take care of the rest and your Rust
//! application will now have the C files `foo.c` and `bar.c` compiled into a file
//! named `libfoo.a`. If the C files contain
//!
//! ```c
//! void foo_function(void) { ... }
//! ```
//!
//! and
//!
//! ```c
//! int32_t bar_function(int32_t x) { ... }
//! ```
//!
//! you can call them from Rust by declaring them in
//! your Rust code like so:
//!
//! ```rust,no_run
//! extern "C" {
//!     fn foo_function();
//!     fn bar_function(x: i32) -> i32;
//! }
//!
//! pub fn call() {
//!     unsafe {
//!         foo_function();
//!         bar_function(42);
//!     }
//! }
//!
//! fn main() {
//!     call();
//! }
//! ```
//!
//! See [the Rustonomicon](https://doc.rust-lang.org/nomicon/ffi.html) for more details.
//!
//! # External configuration via environment variables
//!
//! To control the programs and flags used for building, the builder can set a
//! number of different environment variables.
//!
//! * `CFLAGS` - a series of space separated flags passed to compilers. Note that
//!   individual flags cannot currently contain spaces, so doing
//!   something like: `-L=foo\ bar` is not possible.
//! * `CC` - the actual C compiler used. Note that this is used as an exact
//!   executable name, so (for example) no extra flags can be passed inside
//!   this variable, and the builder must ensure that there aren't any
//!   trailing spaces. This compiler must understand the `-c` flag. For
//!   certain `TARGET`s, it also is assumed to know about other flags (most
//!   common is `-fPIC`).
//! * `AR` - the `ar` (archiver) executable to use to build the static library.
//! * `CRATE_CC_NO_DEFAULTS` - the default compiler flags may cause conflicts in
//!   some cross compiling scenarios. Setting this variable
//!   will disable the generation of default compiler
//!   flags.
//! * `CC_ENABLE_DEBUG_OUTPUT` - if set, compiler command invocations and exit codes will
//!   be logged to stdout. This is useful for debugging build script issues, but can be
//!   overly verbose for normal use.
//! * `CC_SHELL_ESCAPED_FLAGS` - if set, `*FLAGS` will be parsed as if they were shell
//!   arguments (similar to `make` and `cmake`) rather than splitting them on each space.
//!   For example, with `CFLAGS='a "b c"'`, the compiler will be invoked with 2 arguments -
//!   `a` and `b c` - rather than 3: `a`, `"b` and `c"`.
//! * `CXX...` - see [C++ Support](#c-support).
//! * `CC_FORCE_DISABLE` - If set, `cc` will never run any [`Command`]s, and methods that
//!   would return an [`Error`]. This is intended for use by third-party build systems
//!   which want to be absolutely sure that they are in control of building all
//!   dependencies. Note that operations that return [`Tool`]s such as
//!   [`Build::get_compiler`] may produce less accurate results as in some cases `cc` runs
//!   commands in order to locate compilers. Additionally, this does nothing to prevent
//!   users from running [`Tool::to_command`] and executing the [`Command`] themselves.//!
//!
//! Furthermore, projects using this crate may specify custom environment variables
//! to be inspected, for example via the `Build::try_flags_from_environment`
//! function. Consult the project’s own documentation or its use of the `cc` crate
//! for any additional variables it may use.
//!
//! Each of these variables can also be supplied with certain prefixes and suffixes,
//! in the following prioritized order:
//!
//!   1. `<var>_<target>` - for example, `CC_x86_64-unknown-linux-gnu`
//!   2. `<var>_<target_with_underscores>` - for example, `CC_x86_64_unknown_linux_gnu`
//!   3. `<build-kind>_<var>` - for example, `HOST_CC` or `TARGET_CFLAGS`
//!   4. `<var>` - a plain `CC`, `AR` as above.
//!
//! If none of these variables exist, cc-rs uses built-in defaults.
//!
//! In addition to the above optional environment variables, `cc-rs` has some
//! functions with hard requirements on some variables supplied by [cargo's
//! build-script driver][cargo] that it has the `TARGET`, `OUT_DIR`, `OPT_LEVEL`,
//! and `HOST` variables.
//!
//! [cargo]: https://doc.rust-lang.org/cargo/reference/build-scripts.html#inputs-to-the-build-script
//!
//! # Optional features
//!
//! ## Parallel
//!
//! Currently cc-rs supports parallel compilation (think `make -jN`) but this
//! feature is turned off by default. To enable cc-rs to compile C/C++ in parallel,
//! you can change your dependency to:
//!
//! ```toml
//! [build-dependencies]
//! cc = { version = "1.0", features = ["parallel"] }
//! ```
//!
//! By default cc-rs will limit parallelism to `$NUM_JOBS`, or if not present it
//! will limit it to the number of cpus on the machine. If you are using cargo,
//! use `-jN` option of `build`, `test` and `run` commands as `$NUM_JOBS`
//! is supplied by cargo.
//!
//! # Compile-time Requirements
//!
//! To work properly this crate needs access to a C compiler when the build script
//! is being run. This crate does not ship a C compiler with it. The compiler
//! required varies per platform, but there are three broad categories:
//!
//! * Unix platforms require `cc` to be the C compiler. This can be found by
//!   installing cc/clang on Linux distributions and Xcode on macOS, for example.
//! * Windows platforms targeting MSVC (e.g. your target name ends in `-msvc`)
//!   require Visual Studio to be installed. `cc-rs` attempts to locate it, and
//!   if it fails, `cl.exe` is expected to be available in `PATH`. This can be
//!   set up by running the appropriate developer tools shell.
//! * Windows platforms targeting MinGW (e.g. your target name ends in `-gnu`)
//!   require `cc` to be available in `PATH`. We recommend the
//!   [MinGW-w64](https://www.mingw-w64.org/) distribution.
//!   You may also acquire it via
//!   [MSYS2](https://www.msys2.org/), as explained [here][msys2-help].  Make sure
//!   to install the appropriate architecture corresponding to your installation of
//!   rustc. GCC from older [MinGW](http://www.mingw.org/) project is compatible
//!   only with 32-bit rust compiler.
//!
//! [msys2-help]: https://github.com/rust-lang/rust/blob/master/INSTALL.md#building-on-windows
//!
//! # C++ support
//!
//! `cc-rs` supports C++ libraries compilation by using the `cpp` method on
//! `Build`:
//!
//! ```rust,no_run
//! cc::Build::new()
//!     .cpp(true) // Switch to C++ library compilation.
//!     .file("foo.cpp")
//!     .compile("foo");
//! ```
//!
//! For C++ libraries, the `CXX` and `CXXFLAGS` environment variables are used instead of `CC` and `CFLAGS`.
//!
//! The C++ standard library may be linked to the crate target. By default it's `libc++` for macOS, FreeBSD, and OpenBSD, `libc++_shared` for Android, nothing for MSVC, and `libstdc++` for anything else. It can be changed in one of two ways:
//!
//! 1. by using the `cpp_link_stdlib` method on `Build`:
//! ```rust,no_run
//! cc::Build::new()
//!     .cpp(true)
//!     .file("foo.cpp")
//!     .cpp_link_stdlib("stdc++") // use libstdc++
//!     .compile("foo");
//! ```
//! 2. by setting the `CXXSTDLIB` environment variable.
//!
//! In particular, for Android you may want to [use `c++_static` if you have at most one shared library](https://developer.android.com/ndk/guides/cpp-support).
//!
//! Remember that C++ does name mangling so `extern "C"` might be required to enable Rust linker to find your functions.
//!
//! # CUDA C++ support
//!
//! `cc-rs` also supports compiling CUDA C++ libraries by using the `cuda` method
//! on `Build`:
//!
//! ```rust,no_run
//! cc::Build::new()
//!     // Switch to CUDA C++ library compilation using NVCC.
//!     .cuda(true)
//!     .cudart("static")
//!     // Generate code for Maxwell (GTX 970, 980, 980 Ti, Titan X).
//!     .flag("-gencode").flag("arch=compute_52,code=sm_52")
//!     // Generate code for Maxwell (Jetson TX1).
//!     .flag("-gencode").flag("arch=compute_53,code=sm_53")
//!     // Generate code for Pascal (GTX 1070, 1080, 1080 Ti, Titan Xp).
//!     .flag("-gencode").flag("arch=compute_61,code=sm_61")
//!     // Generate code for Pascal (Tesla P100).
//!     .flag("-gencode").flag("arch=compute_60,code=sm_60")
//!     // Generate code for Pascal (Jetson TX2).
//!     .flag("-gencode").flag("arch=compute_62,code=sm_62")
//!     // Generate code in parallel
//!     .flag("-t0")
//!     .file("bar.cu")
//!     .compile("bar");
//! ```

#![doc(html_root_url = "https://docs.rs/cc/1.0")]
#![deny(warnings)]
#![deny(missing_docs)]
#![deny(clippy::disallowed_methods)]
#![warn(clippy::doc_markdown)]

use std::borrow::Cow;
use std::collections::HashMap;
use std::env;
use std::ffi::{OsStr, OsString};
use std::fmt::{self, Display};
use std::fs;
use std::io::{self, Write};
use std::path::{Component, Path, PathBuf};
#[cfg(feature = "parallel")]
use std::process::Child;
use std::process::{Command, Stdio};
use std::sync::{
    atomic::{AtomicU8, Ordering::Relaxed},
    Arc, RwLock,
};

use shlex::Shlex;

#[cfg(feature = "parallel")]
mod parallel;
mod target;
use self::target::TargetInfo;

mod command_helpers;
use command_helpers::*;

mod tool;
pub use tool::Tool;
use tool::{CompilerFamilyLookupCache, ToolFamily};

mod tempfile;

mod utilities;
use utilities::*;

mod flags;
use flags::*;

#[derive(Debug, Eq, PartialEq, Hash)]
struct CompilerFlag {
    compiler: Box<Path>,
    flag: Box<OsStr>,
}

type Env = Option<Arc<OsStr>>;

#[derive(Debug, Default)]
struct BuildCache {
    env_cache: RwLock<HashMap<Box<str>, Env>>,
    cached_compiler_family: RwLock<CompilerFamilyLookupCache>,
    known_flag_support_status_cache: RwLock<HashMap<CompilerFlag, bool>>,
    target_info_parser: target::TargetInfoParser,
}

/// A builder for compilation of a native library.
///
/// A `Build` is the main type of the `cc` crate and is used to control all the
/// various configuration options and such of a compile. You'll find more
/// documentation on each method itself.
#[derive(Clone, Debug)]
pub struct Build {
    include_directories: Vec<Arc<Path>>,
    definitions: Vec<(Arc<str>, Option<Arc<str>>)>,
    objects: Vec<Arc<Path>>,
    flags: Vec<Arc<OsStr>>,
    flags_supported: Vec<Arc<OsStr>>,
    ar_flags: Vec<Arc<OsStr>>,
    asm_flags: Vec<Arc<OsStr>>,
    no_default_flags: bool,
    files: Vec<Arc<Path>>,
    cpp: bool,
    cpp_link_stdlib: Option<Option<Arc<str>>>,
    cpp_set_stdlib: Option<Arc<str>>,
    cuda: bool,
    cudart: Option<Arc<str>>,
    ccbin: bool,
    std: Option<Arc<str>>,
    target: Option<Arc<str>>,
    /// The host compiler.
    ///
    /// Try to not access this directly, and instead prefer `cfg!(...)`.
    host: Option<Arc<str>>,
    out_dir: Option<Arc<Path>>,
    opt_level: Option<Arc<str>>,
    debug: Option<bool>,
    force_frame_pointer: Option<bool>,
    env: Vec<(Arc<OsStr>, Arc<OsStr>)>,
    compiler: Option<Arc<Path>>,
    archiver: Option<Arc<Path>>,
    ranlib: Option<Arc<Path>>,
    cargo_output: CargoOutput,
    link_lib_modifiers: Vec<Arc<OsStr>>,
    pic: Option<bool>,
    use_plt: Option<bool>,
    static_crt: Option<bool>,
    shared_flag: Option<bool>,
    static_flag: Option<bool>,
    warnings_into_errors: bool,
    warnings: Option<bool>,
    extra_warnings: Option<bool>,
    emit_rerun_if_env_changed: bool,
    shell_escaped_flags: Option<bool>,
    build_cache: Arc<BuildCache>,
    inherit_rustflags: bool,
}

/// Represents the types of errors that may occur while using cc-rs.
#[derive(Clone, Debug)]
enum ErrorKind {
    /// Error occurred while performing I/O.
    IOError,
    /// Environment variable not found, with the var in question as extra info.
    EnvVarNotFound,
    /// Error occurred while using external tools (ie: invocation of compiler).
    ToolExecError,
    /// Error occurred due to missing external tools.
    ToolNotFound,
    /// One of the function arguments failed validation.
    InvalidArgument,
    /// No known macro is defined for the compiler when discovering tool family.
    ToolFamilyMacroNotFound,
    /// Invalid target.
    InvalidTarget,
    /// Unknown target.
    UnknownTarget,
    /// Invalid rustc flag.
    InvalidFlag,
    #[cfg(feature = "parallel")]
    /// jobserver helpthread failure
    JobserverHelpThreadError,
    /// `cc` has been disabled by an environment variable.
    Disabled,
}

/// Represents an internal error that occurred, with an explanation.
#[derive(Clone, Debug)]
pub struct Error {
    /// Describes the kind of error that occurred.
    kind: ErrorKind,
    /// More explanation of error that occurred.
    message: Cow<'static, str>,
}

impl Error {
    fn new(kind: ErrorKind, message: impl Into<Cow<'static, str>>) -> Error {
        Error {
            kind,
            message: message.into(),
        }
    }
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Error {
        Error::new(ErrorKind::IOError, format!("{e}"))
    }
}

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.kind, self.message)
    }
}

impl std::error::Error for Error {}

/// Represents an object.
///
/// This is a source file -> object file pair.
#[derive(Clone, Debug)]
struct Object {
    src: PathBuf,
    dst: PathBuf,
}

impl Object {
    /// Create a new source file -> object file pair.
    fn new(src: PathBuf, dst: PathBuf) -> Object {
        Object { src, dst }
    }
}

/// Configure the builder.
impl Build {
    /// Construct a new instance of a blank set of configuration.
    ///
    /// This builder is finished with the [`compile`] function.
    ///
    /// [`compile`]: struct.Build.html#method.compile
    pub fn new() -> Build {
        Build {
            include_directories: Vec::new(),
            definitions: Vec::new(),
            objects: Vec::new(),
            flags: Vec::new(),
            flags_supported: Vec::new(),
            ar_flags: Vec::new(),
            asm_flags: Vec::new(),
            no_default_flags: false,
            files: Vec::new(),
            shared_flag: None,
            static_flag: None,
            cpp: false,
            cpp_link_stdlib: None,
            cpp_set_stdlib: None,
            cuda: false,
            cudart: None,
            ccbin: true,
            std: None,
            target: None,
            host: None,
            out_dir: None,
            opt_level: None,
            debug: None,
            force_frame_pointer: None,
            env: Vec::new(),
            compiler: None,
            archiver: None,
            ranlib: None,
            cargo_output: CargoOutput::new(),
            link_lib_modifiers: Vec::new(),
            pic: None,
            use_plt: None,
            static_crt: None,
            warnings: None,
            extra_warnings: None,
            warnings_into_errors: false,
            emit_rerun_if_env_changed: true,
            shell_escaped_flags: None,
            build_cache: Arc::default(),
            inherit_rustflags: true,
        }
    }

    /// Add a directory to the `-I` or include path for headers
    ///
    /// # Example
    ///
    /// ```no_run
    /// use std::path::Path;
    ///
    /// let library_path = Path::new("/path/to/library");
    ///
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .include(library_path)
    ///     .include("src")
    ///     .compile("foo");
    /// ```
    pub fn include<P: AsRef<Path>>(&mut self, dir: P) -> &mut Build {
        self.include_directories.push(dir.as_ref().into());
        self
    }

    /// Add multiple directories to the `-I` include path.
    ///
    /// # Example
    ///
    /// ```no_run
    /// # use std::path::Path;
    /// # let condition = true;
    /// #
    /// let mut extra_dir = None;
    /// if condition {
    ///     extra_dir = Some(Path::new("/path/to"));
    /// }
    ///
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .includes(extra_dir)
    ///     .compile("foo");
    /// ```
    pub fn includes<P>(&mut self, dirs: P) -> &mut Build
    where
        P: IntoIterator,
        P::Item: AsRef<Path>,
    {
        for dir in dirs {
            self.include(dir);
        }
        self
    }

    /// Specify a `-D` variable with an optional value.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .define("FOO", "BAR")
    ///     .define("BAZ", None)
    ///     .compile("foo");
    /// ```
    pub fn define<'a, V: Into<Option<&'a str>>>(&mut self, var: &str, val: V) -> &mut Build {
        self.definitions
            .push((var.into(), val.into().map(Into::into)));
        self
    }

    /// Add an arbitrary object file to link in
    pub fn object<P: AsRef<Path>>(&mut self, obj: P) -> &mut Build {
        self.objects.push(obj.as_ref().into());
        self
    }

    /// Add arbitrary object files to link in
    pub fn objects<P>(&mut self, objs: P) -> &mut Build
    where
        P: IntoIterator,
        P::Item: AsRef<Path>,
    {
        for obj in objs {
            self.object(obj);
        }
        self
    }

    /// Add an arbitrary flag to the invocation of the compiler
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .flag("-ffunction-sections")
    ///     .compile("foo");
    /// ```
    pub fn flag(&mut self, flag: impl AsRef<OsStr>) -> &mut Build {
        self.flags.push(flag.as_ref().into());
        self
    }

    /// Add multiple flags to the invocation of the compiler.
    /// This is equivalent to calling [`flag`](Self::flag) for each item in the iterator.
    ///
    /// # Example
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .flags(["-Wall", "-Wextra"])
    ///     .compile("foo");
    /// ```
    pub fn flags<Iter>(&mut self, flags: Iter) -> &mut Build
    where
        Iter: IntoIterator,
        Iter::Item: AsRef<OsStr>,
    {
        for flag in flags {
            self.flag(flag);
        }
        self
    }

    /// Removes a compiler flag that was added by [`Build::flag`].
    ///
    /// Will not remove flags added by other means (default flags,
    /// flags from env, and so on).
    ///
    /// # Example
    /// ```
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .flag("unwanted_flag")
    ///     .remove_flag("unwanted_flag");
    /// ```
    pub fn remove_flag(&mut self, flag: &str) -> &mut Build {
        self.flags.retain(|other_flag| &**other_flag != flag);
        self
    }

    /// Add a flag to the invocation of the ar
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .file("src/bar.c")
    ///     .ar_flag("/NODEFAULTLIB:libc.dll")
    ///     .compile("foo");
    /// ```
    pub fn ar_flag(&mut self, flag: impl AsRef<OsStr>) -> &mut Build {
        self.ar_flags.push(flag.as_ref().into());
        self
    }

    /// Add a flag that will only be used with assembly files.
    ///
    /// The flag will be applied to input files with either a `.s` or
    /// `.asm` extension (case insensitive).
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .asm_flag("-Wa,-defsym,abc=1")
    ///     .file("src/foo.S")  // The asm flag will be applied here
    ///     .file("src/bar.c")  // The asm flag will not be applied here
    ///     .compile("foo");
    /// ```
    pub fn asm_flag(&mut self, flag: impl AsRef<OsStr>) -> &mut Build {
        self.asm_flags.push(flag.as_ref().into());
        self
    }

    /// Add an arbitrary flag to the invocation of the compiler if it supports it
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .flag_if_supported("-Wlogical-op") // only supported by GCC
    ///     .flag_if_supported("-Wunreachable-code") // only supported by clang
    ///     .compile("foo");
    /// ```
    pub fn flag_if_supported(&mut self, flag: impl AsRef<OsStr>) -> &mut Build {
        self.flags_supported.push(flag.as_ref().into());
        self
    }

    /// Add flags from the specified environment variable.
    ///
    /// Normally the `cc` crate will consult with the standard set of environment
    /// variables (such as `CFLAGS` and `CXXFLAGS`) to construct the compiler invocation. Use of
    /// this method provides additional levers for the end user to use when configuring the build
    /// process.
    ///
    /// Just like the standard variables, this method will search for an environment variable with
    /// appropriate target prefixes, when appropriate.
    ///
    /// # Examples
    ///
    /// This method is particularly beneficial in introducing the ability to specify crate-specific
    /// flags.
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .try_flags_from_environment(concat!(env!("CARGO_PKG_NAME"), "_CFLAGS"))
    ///     .expect("the environment variable must be specified and UTF-8")
    ///     .compile("foo");
    /// ```
    ///
    pub fn try_flags_from_environment(&mut self, environ_key: &str) -> Result<&mut Build, Error> {
        let flags = self.envflags(environ_key)?.ok_or_else(|| {
            Error::new(
                ErrorKind::EnvVarNotFound,
                format!("could not find environment variable {environ_key}"),
            )
        })?;
        self.flags.extend(
            flags
                .into_iter()
                .map(|flag| Arc::from(OsString::from(flag).as_os_str())),
        );
        Ok(self)
    }

    /// Set the `-shared` flag.
    ///
    /// When enabled, the compiler will produce a shared object which can
    /// then be linked with other objects to form an executable.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .shared_flag(true)
    ///     .compile("libfoo.so");
    /// ```
    pub fn shared_flag(&mut self, shared_flag: bool) -> &mut Build {
        self.shared_flag = Some(shared_flag);
        self
    }

    /// Set the `-static` flag.
    ///
    /// When enabled on systems that support dynamic linking, this prevents
    /// linking with the shared libraries.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .shared_flag(true)
    ///     .static_flag(true)
    ///     .compile("foo");
    /// ```
    pub fn static_flag(&mut self, static_flag: bool) -> &mut Build {
        self.static_flag = Some(static_flag);
        self
    }

    /// Disables the generation of default compiler flags. The default compiler
    /// flags may cause conflicts in some cross compiling scenarios.
    ///
    /// Setting the `CRATE_CC_NO_DEFAULTS` environment variable has the same
    /// effect as setting this to `true`. The presence of the environment
    /// variable and the value of `no_default_flags` will be OR'd together.
    pub fn no_default_flags(&mut self, no_default_flags: bool) -> &mut Build {
        self.no_default_flags = no_default_flags;
        self
    }

    /// Add a file which will be compiled
    pub fn file<P: AsRef<Path>>(&mut self, p: P) -> &mut Build {
        self.files.push(p.as_ref().into());
        self
    }

    /// Add files which will be compiled
    pub fn files<P>(&mut self, p: P) -> &mut Build
    where
        P: IntoIterator,
        P::Item: AsRef<Path>,
    {
        for file in p.into_iter() {
            self.file(file);
        }
        self
    }

    /// Get the files which will be compiled
    pub fn get_files(&self) -> impl Iterator<Item = &Path> {
        self.files.iter().map(AsRef::as_ref)
    }

    /// Set C++ support.
    ///
    /// The other `cpp_*` options will only become active if this is set to
    /// `true`.
    ///
    /// The name of the C++ standard library to link is decided by:
    /// 1. If [`cpp_link_stdlib`](Build::cpp_link_stdlib) is set, use its value.
    /// 2. Else if the `CXXSTDLIB` environment variable is set, use its value.
    /// 3. Else the default is `c++` for OS X and BSDs, `c++_shared` for Android,
    ///    `None` for MSVC and `stdc++` for anything else.
    pub fn cpp(&mut self, cpp: bool) -> &mut Build {
        self.cpp = cpp;
        self
    }

    /// Set CUDA C++ support.
    ///
    /// Enabling CUDA will invoke the CUDA compiler, NVCC. While NVCC accepts
    /// the most common compiler flags, e.g. `-std=c++17`, some project-specific
    /// flags might have to be prefixed with "-Xcompiler" flag, for example as
    /// `.flag("-Xcompiler").flag("-fpermissive")`. See the documentation for
    /// `nvcc`, the CUDA compiler driver, at <https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/>
    /// for more information.
    ///
    /// If enabled, this also implicitly enables C++ support.
    pub fn cuda(&mut self, cuda: bool) -> &mut Build {
        self.cuda = cuda;
        if cuda {
            self.cpp = true;
            self.cudart = Some("static".into());
        }
        self
    }

    /// Link CUDA run-time.
    ///
    /// This option mimics the `--cudart` NVCC command-line option. Just like
    /// the original it accepts `{none|shared|static}`, with default being
    /// `static`. The method has to be invoked after `.cuda(true)`, or not
    /// at all, if the default is right for the project.
    pub fn cudart(&mut self, cudart: &str) -> &mut Build {
        if self.cuda {
            self.cudart = Some(cudart.into());
        }
        self
    }

    /// Set CUDA host compiler.
    ///
    /// By default, a `-ccbin` flag will be passed to NVCC to specify the
    /// underlying host compiler. The value of `-ccbin` is the same as the
    /// chosen C++ compiler. This is not always desired, because NVCC might
    /// not support that compiler. In this case, you can remove the `-ccbin`
    /// flag so that NVCC will choose the host compiler by itself.
    pub fn ccbin(&mut self, ccbin: bool) -> &mut Build {
        self.ccbin = ccbin;
        self
    }

    /// Specify the C or C++ language standard version.
    ///
    /// These values are common to modern versions of GCC, Clang and MSVC:
    /// - `c11` for ISO/IEC 9899:2011
    /// - `c17` for ISO/IEC 9899:2018
    /// - `c++14` for ISO/IEC 14882:2014
    /// - `c++17` for ISO/IEC 14882:2017
    /// - `c++20` for ISO/IEC 14882:2020
    ///
    /// Other values have less broad support, e.g. MSVC does not support `c++11`
    /// (`c++14` is the minimum), `c89` (omit the flag instead) or `c99`.
    ///
    /// For compiling C++ code, you should also set `.cpp(true)`.
    ///
    /// The default is that no standard flag is passed to the compiler, so the
    /// language version will be the compiler's default.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/modern.cpp")
    ///     .cpp(true)
    ///     .std("c++17")
    ///     .compile("modern");
    /// ```
    pub fn std(&mut self, std: &str) -> &mut Build {
        self.std = Some(std.into());
        self
    }

    /// Set warnings into errors flag.
    ///
    /// Disabled by default.
    ///
    /// Warning: turning warnings into errors only make sense
    /// if you are a developer of the crate using cc-rs.
    /// Some warnings only appear on some architecture or
    /// specific version of the compiler. Any user of this crate,
    /// or any other crate depending on it, could fail during
    /// compile time.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .warnings_into_errors(true)
    ///     .compile("libfoo.a");
    /// ```
    pub fn warnings_into_errors(&mut self, warnings_into_errors: bool) -> &mut Build {
        self.warnings_into_errors = warnings_into_errors;
        self
    }

    /// Set warnings flags.
    ///
    /// Adds some flags:
    /// - "-Wall" for MSVC.
    /// - "-Wall", "-Wextra" for GNU and Clang.
    ///
    /// Enabled by default.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .warnings(false)
    ///     .compile("libfoo.a");
    /// ```
    pub fn warnings(&mut self, warnings: bool) -> &mut Build {
        self.warnings = Some(warnings);
        self.extra_warnings = Some(warnings);
        self
    }

    /// Set extra warnings flags.
    ///
    /// Adds some flags:
    /// - nothing for MSVC.
    /// - "-Wextra" for GNU and Clang.
    ///
    /// Enabled by default.
    ///
    /// # Example
    ///
    /// ```no_run
    /// // Disables -Wextra, -Wall remains enabled:
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .extra_warnings(false)
    ///     .compile("libfoo.a");
    /// ```
    pub fn extra_warnings(&mut self, warnings: bool) -> &mut Build {
        self.extra_warnings = Some(warnings);
        self
    }

    /// Set the standard library to link against when compiling with C++
    /// support.
    ///
    /// If the `CXXSTDLIB` environment variable is set, its value will
    /// override the default value, but not the value explicitly set by calling
    /// this function.
    ///
    /// A value of `None` indicates that no automatic linking should happen,
    /// otherwise cargo will link against the specified library.
    ///
    /// The given library name must not contain the `lib` prefix.
    ///
    /// Common values:
    /// - `stdc++` for GNU
    /// - `c++` for Clang
    /// - `c++_shared` or `c++_static` for Android
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .shared_flag(true)
    ///     .cpp_link_stdlib("stdc++")
    ///     .compile("libfoo.so");
    /// ```
    pub fn cpp_link_stdlib<'a, V: Into<Option<&'a str>>>(
        &mut self,
        cpp_link_stdlib: V,
    ) -> &mut Build {
        self.cpp_link_stdlib = Some(cpp_link_stdlib.into().map(Arc::from));
        self
    }

    /// Force the C++ compiler to use the specified standard library.
    ///
    /// Setting this option will automatically set `cpp_link_stdlib` to the same
    /// value.
    ///
    /// The default value of this option is always `None`.
    ///
    /// This option has no effect when compiling for a Visual Studio based
    /// target.
    ///
    /// This option sets the `-stdlib` flag, which is only supported by some
    /// compilers (clang, icc) but not by others (gcc). The library will not
    /// detect which compiler is used, as such it is the responsibility of the
    /// caller to ensure that this option is only used in conjunction with a
    /// compiler which supports the `-stdlib` flag.
    ///
    /// A value of `None` indicates that no specific C++ standard library should
    /// be used, otherwise `-stdlib` is added to the compile invocation.
    ///
    /// The given library name must not contain the `lib` prefix.
    ///
    /// Common values:
    /// - `stdc++` for GNU
    /// - `c++` for Clang
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .cpp_set_stdlib("c++")
    ///     .compile("libfoo.a");
    /// ```
    pub fn cpp_set_stdlib<'a, V: Into<Option<&'a str>>>(
        &mut self,
        cpp_set_stdlib: V,
    ) -> &mut Build {
        let cpp_set_stdlib = cpp_set_stdlib.into().map(Arc::from);
        self.cpp_set_stdlib.clone_from(&cpp_set_stdlib);
        self.cpp_link_stdlib = Some(cpp_set_stdlib);
        self
    }

    /// Configures the `rustc` target this configuration will be compiling
    /// for.
    ///
    /// This will fail if using a target not in a pre-compiled list taken from
    /// `rustc +nightly --print target-list`. The list will be updated
    /// periodically.
    ///
    /// You should avoid setting this in build scripts, target information
    /// will instead be retrieved from the environment variables `TARGET` and
    /// `CARGO_CFG_TARGET_*` that Cargo sets.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .target("aarch64-linux-android")
    ///     .compile("foo");
    /// ```
    pub fn target(&mut self, target: &str) -> &mut Build {
        self.target = Some(target.into());
        self
    }

    /// Configures the host assumed by this configuration.
    ///
    /// This option is automatically scraped from the `HOST` environment
    /// variable by build scripts, so it's not required to call this function.
    ///
    /// # Example
    ///
    /// ```no_run
    /// cc::Build::new()
    ///     .file("src/foo.c")
    ///     .host("arm-linux-gnueabihf")
    ///     .compile("foo");
    /// ```
    pub fn host(&mut self, host: &str) -> &mut Build {
        self.host = Some(host.into());
        self
    }

    /// Configures the optimization level of the generated object files.
    ///
    /// This option is automatically scraped from the `OPT_LEVEL` environment
    /// variable by build scripts, so it's not required to call this function.
    pub fn opt_level(&mut self, opt_level: u32) -> &mut Build {
        self.opt_level = Some(opt_level.to_string().into());
        self
    }

    /// Configures the optimization level of the generated object files.
    ///
    /// This option is automatically scraped from the `OPT_LEVEL` environment
    /// variable by build scripts, so it's not required to call this function.
    pub fn opt_level_str(&mut self, opt_level: &str) -> &mut Build {
        self.opt_level = Some(opt_level.into());
        self
    }

    /// Configures whether the compiler will emit debug information when
    /// generating object files.
    ///
    /// This option is automatically scraped from the `DEBUG` environment
    /// variable by build scripts, so it's not required to call this function.
    pub fn debug(&mut self, debug: bool) -> &mut Build {
        self.debug = Some(debug);
        self
    }

    /// Configures whether the compiler will emit instructions to store
    /// frame pointers during codegen.
    ///
    /// This option is automatically enabled when debug information is emitted.
    /// Otherwise the target platform compiler's default will be used.
    /// You can use this option to force a specific setting.
    pub fn force_frame_pointer(&mut self, force: bool) -> &mut Build {
        self.force_frame_pointer = Some(force);
        self
    }

    /// Configures the output directory where all object files and static
    /// libraries will be located.
    ///
    /// This option is automatically scraped from the `OUT_DIR` environment
    /// variable by build scripts, so it's not required to call this function.
    pub fn out_dir<P: AsRef<Path>>(&mut self, out_dir: P) -> &mut Build {
        self.out_dir = Some(out_dir.as_ref().into());
        self
    }

    /// Configures the compiler to be used to produce output.
    ///
    /// This option is automatically determined from the target platform or a
    /// number of environment variables, so it's not required to call this
    /// function.
    pub fn compiler<P: AsRef<Path>>(&mut self, compiler: P) -> &mut Build {
        self.compiler = Some(compiler.as_ref().into());
        self
    }

    /// Configures the tool used to assemble archives.
    ///
    /// This option is automatically determined from the target platform or a
    /// number of environment variables, so it's not required to call this
    /// function.
    pub fn archiver<P: AsRef<Path>>(&mut self, archiver: P) -> &mut Build {
        self.archiver = Some(archiver.as_ref().into());
        self
    }

    /// Configures the tool used to index archives.
    ///
    /// This option is automatically determined from the target platform or a
    /// number of environment variables, so it's not required to call this
    /// function.
    pub fn ranlib<P: AsRef<Path>>(&mut self, ranlib: P) -> &mut Build {
        self.ranlib = Some(ranlib.as_ref().into());
        self
    }

    /// Define whether metadata should be emitted for cargo allowing it to
    /// automatically link the binary. Defaults to `true`.
    ///
    /// The emitted metadata is:
    ///
    ///  - `rustc-link-lib=static=`*compiled lib*
    ///  - `rustc-link-search=native=`*target folder*
    ///  - When target is MSVC, the ATL-MFC libs are added via `rustc-link-search=native=`
    ///  - When C++ is enabled, the C++ stdlib is added via `rustc-link-lib`
    ///  - If `emit_rerun_if_env_changed` is not `false`, `rerun-if-env-changed=`*env*
    ///
    pub fn cargo_metadata(&mut self, cargo_metadata: bool) -> &mut Build {
        self.cargo_output.metadata = cargo_metadata;
        self
    }

    /// Define whether compile warnings should be emitted for cargo. Defaults to
    /// `true`.
    ///
    /// If disabled, compiler messages will not be printed.
    /// Issues unrelated to the compilation will always produce cargo warnings regardless of this setting.
    pub fn cargo_warnings(&mut self, cargo_warnings: bool) -> &mut Build {
        self.cargo_output.warnings = cargo_warnings;
        self
    }

    /// Define whether debug information should be emitted for cargo. Defaults to whether
    /// or not the environment variable `CC_ENABLE_DEBUG_OUTPUT` is set.
    ///
    /// If enabled, the compiler will emit debug information when generating object files,
    /// such as the command invoked and the exit status.
    pub fn cargo_debug(&mut self, cargo_debug: bool) -> &mut Build {
        self.cargo_output.debug = cargo_debug;
        self
    }

    /// Define whether compiler output (to stdout) should be emitted. Defaults to `true`
    /// (forward compiler stdout to this process' stdout)
    ///
    /// Some compilers emit errors to stdout, so if you *really* need stdout to be clean
    /// you should also set this to `false`.
    pub fn cargo_output(&mut self, cargo_output: bool) -> &mut Build {
        self.cargo_output.output = if cargo_output {
            OutputKind::Forward
        } else {
            OutputKind::Discard
        };
        self
    }

    /// Adds a native library modifier that will be added to the
    /// `rustc-link-lib=static:MODIFIERS=LIBRARY_NAME` metadata line
    /// emitted for cargo if `cargo_metadata` is enabled.
    /// See <https://doc.rust-lang.org/rustc/command-line-arguments.html#-l-link-the-generated-crate-to-a-native-library>
    /// for the list of modifiers accepted by rustc.
    pub fn link_lib_modifier(&mut self, link_lib_modifier: impl AsRef<OsStr>) -> &mut Build {
        self.link_lib_modifiers
            .push(link_lib_modifier.as_ref().into());
        self
    }

    /// Configures whether the compiler will emit position independent code.
    ///
    /// This option defaults to `false` for `windows-gnu` and bare metal targets and
    /// to `true` for all other targets.
    pub fn pic(&mut self, pic: bool) -> &mut Build {
        self.pic = Some(pic);
        self
    }

    /// Configures whether the Procedure Linkage Table is used for indirect
    /// calls into shared libraries.
    ///
    /// The PLT is used to provide features like lazy binding, but introduces
    /// a small performance loss due to extra pointer indirection. Setting
    /// `use_plt` to `false` can provide a small performance increase.
    ///
    /// Note that skipping the PLT requires a recent version of GCC/Clang.
    ///
    /// This only applies to ELF targets. It has no effect on other platforms.
    pub fn use_plt(&mut self, use_plt: bool) -> &mut Build {
        self.use_plt = Some(use_plt);
        self
    }

    /// Define whether metadata should be emitted for cargo to detect environment
    /// changes that should trigger a rebuild.
    ///
    /// NOTE that cc does not emit metadata to detect changes for `PATH`, since it could
    /// be changed every comilation yet does not affect the result of compilation
    /// (i.e. rust-analyzer adds temporary directory to `PATH`).
    ///
    /// cc in general, has no way detecting changes to compiler, as there are so many ways to
    /// change it and sidestep the detection, for example the compiler might be wrapped in a script
    /// so detecting change of the file, or using checksum won't work.
    ///
    /// We recommend users to decide for themselves, if they want rebuild if the compiler has been upgraded
    /// or changed, and how to detect that.
    ///
    /// This has no effect if the `cargo_metadata` option is `false`.
    ///
    /// This option defaults to `true`.
    pub fn emit_rerun_if_env_changed(&mut self, emit_rerun_if_env_changed: bool) -> &mut Build {
        self.emit_rerun_if_env_changed = emit_rerun_if_env_changed;
        self
    }

    /// Configures whether the /MT flag or the /MD flag will be passed to msvc build tools.
    ///
    /// This option defaults to `false`, and affect only msvc targets.
    pub fn static_crt(&mut self, static_crt: bool) -> &mut Build {
        self.static_crt = Some(static_crt);
        self
    }

    /// Configure whether *FLAGS variables are parsed using `shlex`, similarly to `make` and
    /// `cmake`.
    ///
    /// This option defaults to `false`.
    pub fn shell_escaped_flags(&mut self, shell_escaped_flags: bool) -> &mut Build {
        self.shell_escaped_flags = Some(shell_escaped_flags);
        self
    }

    /// Configure whether cc should automatically inherit compatible flags passed to rustc
    /// from `CARGO_ENCODED_RUSTFLAGS`.
    ///
    /// This option defaults to `true`.
    pub fn inherit_rustflags(&mut self, inherit_rustflags: bool) -> &mut Build {
        self.inherit_rustflags = inherit_rustflags;
        self
    }

    #[doc(hidden)]
    pub fn __set_env<A, B>(&mut self, a: A, b: B) -> &mut Build
    where
        A: AsRef<OsStr>,
        B: AsRef<OsStr>,
    {
        self.env.push((a.as_ref().into(), b.as_ref().into()));
        self
    }
}

/// Invoke or fetch the compiler or archiver.
impl Build {
    /// Run the compiler to test if it accepts the given flag.
    ///
    /// For a convenience method for setting flags conditionally,
    /// see `flag_if_supported()`.
    ///
    /// It may return error if it's unable to run the compiler with a test file
    /// (e.g. the compiler is missing or a write to the `out_dir` failed).
    ///
    /// Note: Once computed, the result of this call is stored in the
    /// `known_flag_support` field. If `is_flag_supported(flag)`
    /// is called again, the result will be read from the hash table.
    pub fn is_flag_supported(&self, flag: impl AsRef<OsStr>) -> Result<bool, Error> {
        self.is_flag_supported_inner(
            flag.as_ref(),
            &self.get_base_compiler()?,
            &self.get_target()?,
        )
    }

    fn ensure_check_file(&self) -> Result<PathBuf, Error> {
        let out_dir = self.get_out_dir()?;
        let src = if self.cuda {
            assert!(self.cpp);
            out_dir.join("flag_check.cu")
        } else if self.cpp {
            out_dir.join("flag_check.cpp")
        } else {
            out_dir.join("flag_check.c")
        };

        if !src.exists() {
            let mut f = fs::File::create(&src)?;
            write!(f, "int main(void) {{ return 0; }}")?;
        }

        Ok(src)
    }

    fn is_flag_supported_inner(
        &self,
        flag: &OsStr,
        tool: &Tool,
        target: &TargetInfo<'_>,
    ) -> Result<bool, Error> {
        let compiler_flag = CompilerFlag {
            compiler: tool.path().into(),
            flag: flag.into(),
        };

        if let Some(is_supported) = self
            .build_cache
            .known_flag_support_status_cache
            .read()
            .unwrap()
            .get(&compiler_flag)
            .cloned()
        {
            return Ok(is_supported);
        }

        let out_dir = self.get_out_dir()?;
        let src = self.ensure_check_file()?;
        let obj = out_dir.join("flag_check");

        let mut compiler = {
            let mut cfg = Build::new();
            cfg.flag(flag)
                .compiler(tool.path())
                .cargo_metadata(self.cargo_output.metadata)
                .opt_level(0)
                .debug(false)
                .cpp(self.cpp)
                .cuda(self.cuda)
                .inherit_rustflags(false)
                .emit_rerun_if_env_changed(self.emit_rerun_if_env_changed);
            if let Some(target) = &self.target {
                cfg.target(target);
            }
            if let Some(host) = &self.host {
                cfg.host(host);
            }
            cfg.try_get_compiler()?
        };

        if compiler.family.verbose_stderr() {
            compiler.remove_arg("-v".into());
        }
        if compiler.is_like_clang() {
            compiler.push_cc_arg("-Wno-unused-command-line-argument".into());
        }

        let mut cmd = compiler.to_command();
        let is_arm = matches!(target.arch, "aarch64" | "arm");
        command_add_output_file(
            &mut cmd,
            &obj,
            CmdAddOutputFileArgs {
                cuda: self.cuda,
                is_assembler_msvc: false,
                msvc: compiler.is_like_msvc(),
                clang: compiler.is_like_clang(),
                gnu: compiler.is_like_gnu(),
                is_asm: false,
                is_arm,
            },
        );

        cmd.arg("-c");

        if compiler.supports_path_delimiter() {
            cmd.arg("--");
        }

        cmd.arg(&src);

        if compiler.is_like_msvc() {
            for (key, value) in &tool.env {
                if key == "LIB" {
                    cmd.env("LIB", value);
                    break;
                }
            }
        }

        let output = cmd.current_dir(out_dir).output()?;
        let is_supported = output.status.success() && output.stderr.is_empty();

        self.build_cache
            .known_flag_support_status_cache
            .write()
            .unwrap()
            .insert(compiler_flag, is_supported);

        Ok(is_supported)
    }

    /// Run the compiler, generating the file `output`
    ///
    /// This will return a result instead of panicking; see [`Self::compile()`] for
    /// the complete description.
    pub fn try_compile(&self, output: &str) -> Result<(), Error> {
        let mut output_components = Path::new(output).components();
        match (output_components.next(), output_components.next()) {
            (Some(Component::Normal(_)), None) => {}
            _ => {
                return Err(Error::new(
                    ErrorKind::InvalidArgument,
                    "argument of `compile` must be a single normal path component",
                ));
            }
        }

        let (lib_name, gnu_lib_name) = if output.starts_with("lib") && output.ends_with(".a") {
            (&output[3..output.len() - 2], output.to_owned())
        } else {
            let mut gnu = String::with_capacity(5 + output.len());
            gnu.push_str("lib");
            gnu.push_str(output);
            gnu.push_str(".a");
            (output, gnu)
        };
        let dst = self.get_out_dir()?;

        let objects = objects_from_files(&self.files, &dst)?;

        self.compile_objects(&objects)?;
        self.assemble(lib_name, &dst.join(gnu_lib_name), &objects)?;

        let target = self.get_target()?;
        if target.env == "msvc" {
            let compiler = self.get_base_compiler()?;
            let atlmfc_lib = compiler
                .env()
                .iter()
                .find(|&(var, _)| var.as_os_str() == OsStr::new("LIB"))
                .and_then(|(_, lib_paths)| {
                    env::split_paths(lib_paths).find(|path| {
                        let sub = Path::new("atlmfc/lib");
                        path.ends_with(sub) || path.parent().map_or(false, |p| p.ends_with(sub))
                    })
                });

            if let Some(atlmfc_lib) = atlmfc_lib {
                self.cargo_output.print_metadata(&format_args!(
                    "cargo:rustc-link-search=native={}",
                    atlmfc_lib.display()
                ));
            }
        }

        if self.link_lib_modifiers.is_empty() {
            self.cargo_output
                .print_metadata(&format_args!("cargo:rustc-link-lib=static={lib_name}"));
        } else {
            self.cargo_output.print_metadata(&format_args!(
                "cargo:rustc-link-lib=static:{}={}",
                JoinOsStrs {
                    slice: &self.link_lib_modifiers,
                    delimiter: ','
                },
                lib_name
            ));
        }
        self.cargo_output.print_metadata(&format_args!(
            "cargo:rustc-link-search=native={}",
            dst.display()
        ));

        if self.cpp {
            if let Some(stdlib) = self.get_cpp_link_stdlib()? {
                self.cargo_output
                    .print_metadata(&format_args!("cargo:rustc-link-lib={}", stdlib.display()));
            }
            if target.arch == "wasm32" {
                if target.os == "wasi" {
                    if let Ok(wasi_sysroot) = self.wasi_sysroot() {
                        self.cargo_output.print_metadata(&format_args!(
                            "cargo:rustc-flags=-L {}/lib/{} -lstatic=c++ -lstatic=c++abi",
                            Path::new(&wasi_sysroot).display(),
                            self.get_raw_target()?
                        ));
                    }
                } else if target.os == "linux" {
                    let musl_sysroot = self.wasm_musl_sysroot().unwrap();
                    self.cargo_output.print_metadata(&format_args!(
                        "cargo:rustc-flags=-L {}/lib -lstatic=c++ -lstatic=c++abi",
                        Path::new(&musl_sysroot).display(),
                    ));
                }
            }
        }

        let cudart = match &self.cudart {
            Some(opt) => opt, 
            None => "none",
        };
        if cudart != "none" {
            if let Some(nvcc) = self.which(&self.get_compiler().path, None) {
                let mut libtst = false;
                let mut libdir = nvcc;
                libdir.pop(); 
                libdir.push("..");
                if cfg!(target_os = "linux") {
                    libdir.push("targets");
                    libdir.push(format!("{}-linux", target.arch));
                    libdir.push("lib");
                    libtst = true;
                } else if cfg!(target_env = "msvc") {
                    libdir.push("lib");
                    match target.arch {
                        "x86_64" => {
                            libdir.push("x64");
                            libtst = true;
                        }
                        "x86" => {
                            libdir.push("Win32");
                            libtst = true;
                        }
                        _ => libtst = false,
                    }
                }
                if libtst && libdir.is_dir() {
                    self.cargo_output.print_metadata(&format_args!(
                        "cargo:rustc-link-search=native={}",
                        libdir.to_str().unwrap()
                    ));
                }

                let lib = match cudart {
                    "shared" => "cudart",
                    "static" => "cudart_static",
                    bad => panic!("unsupported cudart option: {}", bad),
                };
                self.cargo_output
                    .print_metadata(&format_args!("cargo:rustc-link-lib={lib}"));
            }
        }

        Ok(())
    }

    /// Run the compiler, generating the file `output`
    ///
    /// # Library name
    ///
    /// The `output` string argument determines the file name for the compiled
    /// library. The Rust compiler will create an assembly named "lib"+output+".a".
    /// MSVC will create a file named output+".lib".
    ///
    /// The choice of `output` is close to arbitrary, but:
    ///
    /// - must be nonempty,
    /// - must not contain a path separator (`/`),
    /// - must be unique across all `compile` invocations made by the same build
    ///   script.
    ///
    /// If your build script compiles a single source file, the base name of
    /// that source file would usually be reasonable:
    ///
    /// ```no_run
    /// cc::Build::new().file("blobstore.c").compile("blobstore");
    /// ```
    ///
    /// Compiling multiple source files, some people use their crate's name, or
    /// their crate's name + "-cc".
    ///
    /// Otherwise, please use your imagination.
    ///
    /// For backwards compatibility, if `output` starts with "lib" *and* ends
    /// with ".a", a second "lib" prefix and ".a" suffix do not get added on,
    /// but this usage is deprecated; please omit `lib` and `.a` in the argument
    /// that you pass.
    ///
    /// # Panics
    ///
    /// Panics if `output` is not formatted correctly or if one of the underlying
    /// compiler commands fails. It can also panic if it fails reading file names
    /// or creating directories.
    pub fn compile(&self, output: &str) {
        if let Err(e) = self.try_compile(output) {
            fail(&e.message);
        }
    }

    /// Run the compiler, generating intermediate files, but without linking
    /// them into an archive file.
    ///
    /// This will return a list of compiled object files, in the same order
    /// as they were passed in as `file`/`files` methods.
    pub fn compile_intermediates(&self) -> Vec<PathBuf> {
        match self.try_compile_intermediates() {
            Ok(v) => v,
            Err(e) => fail(&e.message),
        }
    }

    /// Run the compiler, generating intermediate files, but without linking
    /// them into an archive file.
    ///
    /// This will return a result instead of panicking; see `compile_intermediates()` for the complete description.
    pub fn try_compile_intermediates(&self) -> Result<Vec<PathBuf>, Error> {
        let dst = self.get_out_dir()?;
        let objects = objects_from_files(&self.files, &dst)?;

        self.compile_objects(&objects)?;

        Ok(objects.into_iter().map(|v| v.dst).collect())
    }

    #[cfg(feature = "parallel")]
    fn compile_objects(&self, objs: &[Object]) -> Result<(), Error> {
        use std::cell::Cell;

        use parallel::async_executor::{block_on, YieldOnce};

        check_disabled()?;

        if objs.len() <= 1 {
            for obj in objs {
                let mut cmd = self.create_compile_object_cmd(obj)?;
                run(&mut cmd, &self.cargo_output)?;
            }

            return Ok(());
        }

        let mut tokens = parallel::job_token::ActiveJobTokenServer::new();


        let pendings =
            Cell::new(Vec::<(Command, KillOnDrop, parallel::job_token::JobToken)>::new());
        let is_disconnected = Cell::new(false);
        let has_made_progress = Cell::new(false);

        let wait_future = async {
            let mut error = None;
            let mut stdout = io::BufWriter::with_capacity(128, io::stdout());

            loop {

                let mut pendings_is_empty = false;

                cell_update(&pendings, |mut pendings| {
                    pendings.retain_mut(|(cmd, child, _token)| {
                        match try_wait_on_child(cmd, &mut child.0, &mut stdout, &mut child.1) {
                            Ok(Some(())) => {
                                has_made_progress.set(true);
                                false
                            }
                            Ok(None) => true, 
                            Err(err) => {
                                has_made_progress.set(true);

                                if self.cargo_output.warnings {
                                    let _ = writeln!(stdout, "cargo:warning={}", err);
                                }
                                error = Some(err);

                                false
                            }
                        }
                    });
                    pendings_is_empty = pendings.is_empty();
                    pendings
                });

                if pendings_is_empty && is_disconnected.get() {
                    break if let Some(err) = error {
                        Err(err)
                    } else {
                        Ok(())
                    };
                }

                YieldOnce::default().await;
            }
        };
        let spawn_future = async {
            for obj in objs {
                let mut cmd = self.create_compile_object_cmd(obj)?;
                let token = tokens.acquire().await?;
                let mut child = spawn(&mut cmd, &self.cargo_output)?;
                let mut stderr_forwarder = StderrForwarder::new(&mut child);
                stderr_forwarder.set_non_blocking()?;

                cell_update(&pendings, |mut pendings| {
                    pendings.push((cmd, KillOnDrop(child, stderr_forwarder), token));
                    pendings
                });

                has_made_progress.set(true);
            }
            is_disconnected.set(true);

            Ok::<_, Error>(())
        };

        return block_on(wait_future, spawn_future, &has_made_progress);

        struct KillOnDrop(Child, StderrForwarder);

        impl Drop for KillOnDrop {
            fn drop(&mut self) {
                let child = &mut self.0;

                child.kill().ok();
            }
        }

        fn cell_update<T, F>(cell: &Cell<T>, f: F)
        where
            T: Default,
            F: FnOnce(T) -> T,
        {
            let old = cell.take();
            let new = f(old);
            cell.set(new);
        }
    }

    #[cfg(not(feature = "parallel"))]
    fn compile_objects(&self, objs: &[Object]) -> Result<(), Error> {
        check_disabled()?;

        for obj in objs {
            let mut cmd = self.create_compile_object_cmd(obj)?;
            run(&mut cmd, &self.cargo_output)?;
        }

        Ok(())
    }

    fn create_compile_object_cmd(&self, obj: &Object) -> Result<Command, Error> {
        let asm_ext = AsmFileExt::from_path(&obj.src);
        let is_asm = asm_ext.is_some();
        let target = self.get_target()?;
        let msvc = target.env == "msvc";
        let compiler = self.try_get_compiler()?;

        let is_assembler_msvc = msvc && asm_ext == Some(AsmFileExt::DotAsm);
        let mut cmd = if is_assembler_msvc {
            self.msvc_macro_assembler()?
        } else {
            let mut cmd = compiler.to_command();
            for (a, b) in self.env.iter() {
                cmd.env(a, b);
            }
            cmd
        };
        let is_arm = matches!(target.arch, "aarch64" | "arm");
        command_add_output_file(
            &mut cmd,
            &obj.dst,
            CmdAddOutputFileArgs {
                cuda: self.cuda,
                is_assembler_msvc,
                msvc: compiler.is_like_msvc(),
                clang: compiler.is_like_clang(),
                gnu: compiler.is_like_gnu(),
                is_asm,
                is_arm,
            },
        );
        if !is_assembler_msvc || !is_arm {
            cmd.arg("-c");
        }
        if self.cuda && self.cuda_file_count() > 1 {
            cmd.arg("--device-c");
        }
        if is_asm {
            cmd.args(self.asm_flags.iter().map(std::ops::Deref::deref));
        }

        if compiler.supports_path_delimiter() && !is_assembler_msvc {
            cmd.arg("--");
        }
        cmd.arg(&obj.src);

        Ok(cmd)
    }

    /// This will return a result instead of panicking; see [`Self::expand()`] for
    /// the complete description.
    pub fn try_expand(&self) -> Result<Vec<u8>, Error> {
        let compiler = self.try_get_compiler()?;
        let mut cmd = compiler.to_command();
        for (a, b) in self.env.iter() {
            cmd.env(a, b);
        }
        cmd.arg("-E");

        assert!(
            self.files.len() <= 1,
            "Expand may only be called for a single file"
        );

        let is_asm = self
            .files
            .iter()
            .map(std::ops::Deref::deref)
            .find_map(AsmFileExt::from_path)
            .is_some();

        if compiler.family == (ToolFamily::Msvc { clang_cl: true }) && !is_asm {
            cmd.arg("--");
        }

        cmd.args(self.files.iter().map(std::ops::Deref::deref));

        run_output(&mut cmd, &self.cargo_output)
    }

    /// Run the compiler, returning the macro-expanded version of the input files.
    ///
    /// This is only relevant for C and C++ files.
    ///
    /// # Panics
    /// Panics if more than one file is present in the config, or if compiler
    /// path has an invalid file name.
    ///
    /// # Example
    /// ```no_run
    /// let out = cc::Build::new().file("src/foo.c").expand();
    /// ```
    pub fn expand(&self) -> Vec<u8> {
        match self.try_expand() {
            Err(e) => fail(&e.message),
            Ok(v) => v,
        }
    }

    /// Get the compiler that's in use for this configuration.
    ///
    /// This function will return a `Tool` which represents the culmination
    /// of this configuration at a snapshot in time. The returned compiler can
    /// be inspected (e.g. the path, arguments, environment) to forward along to
    /// other tools, or the `to_command` method can be used to invoke the
    /// compiler itself.
    ///
    /// This method will take into account all configuration such as debug
    /// information, optimization level, include directories, defines, etc.
    /// Additionally, the compiler binary in use follows the standard
    /// conventions for this path, e.g. looking at the explicitly set compiler,
    /// environment variables (a number of which are inspected here), and then
    /// falling back to the default configuration.
    ///
    /// # Panics
    ///
    /// Panics if an error occurred while determining the architecture.
    pub fn get_compiler(&self) -> Tool {
        match self.try_get_compiler() {
            Ok(tool) => tool,
            Err(e) => fail(&e.message),
        }
    }

    /// Get the compiler that's in use for this configuration.
    ///
    /// This will return a result instead of panicking; see
    /// [`get_compiler()`](Self::get_compiler) for the complete description.
    pub fn try_get_compiler(&self) -> Result<Tool, Error> {
        let opt_level = self.get_opt_level()?;
        let target = self.get_target()?;

        let mut cmd = self.get_base_compiler()?;


        if matches!(cmd.family, ToolFamily::Msvc { clang_cl: false }) {
            cmd.env.push(("VSLANG".into(), "1033".into()));
        } else {
            cmd.env.push(("LC_ALL".into(), "C".into()));
        }

        let no_defaults = self.no_default_flags || self.getenv_boolean("CRATE_CC_NO_DEFAULTS");
        if !no_defaults {
            self.add_default_flags(&mut cmd, &target, &opt_level)?;
        }

        if let Some(ref std) = self.std {
            let separator = match cmd.family {
                ToolFamily::Msvc { .. } => ':',
                ToolFamily::Gnu | ToolFamily::Clang { .. } => '=',
            };
            cmd.push_cc_arg(format!("-std{separator}{std}").into());
        }
        for directory in self.include_directories.iter() {
            cmd.args.push("-I".into());
            cmd.args.push(directory.as_os_str().into());
        }
        if self.warnings_into_errors {
            let warnings_to_errors_flag = cmd.family.warnings_to_errors_flag().into();
            cmd.push_cc_arg(warnings_to_errors_flag);
        }

        let envflags = self.envflags(if self.cpp { "CXXFLAGS" } else { "CFLAGS" })?;
        if self.warnings.unwrap_or(envflags.is_none()) {
            let wflags = cmd.family.warnings_flags().into();
            cmd.push_cc_arg(wflags);
        }
        if self.extra_warnings.unwrap_or(envflags.is_none()) {
            if let Some(wflags) = cmd.family.extra_warnings_flags() {
                cmd.push_cc_arg(wflags.into());
            }
        }

        if self.inherit_rustflags {
            self.add_inherited_rustflags(&mut cmd, &target)?;
        }

        for flag in self.flags.iter() {
            cmd.args.push((**flag).into());
        }
        for flag in self.flags_supported.iter() {
            if self
                .is_flag_supported_inner(flag, &cmd, &target)
                .unwrap_or(false)
            {
                cmd.push_cc_arg((**flag).into());
            }
        }
        for (key, value) in self.definitions.iter() {
            if let Some(ref value) = *value {
                cmd.args.push(format!("-D{key}={value}").into());
            } else {
                cmd.args.push(format!("-D{key}").into());
            }
        }

        if let Some(flags) = &envflags {
            for arg in flags {
                cmd.push_cc_arg(arg.into());
            }
        }

        Ok(cmd)
    }

    fn add_default_flags(
        &self,
        cmd: &mut Tool,
        target: &TargetInfo<'_>,
        opt_level: &str,
    ) -> Result<(), Error> {
        let raw_target = self.get_raw_target()?;
        match cmd.family {
            ToolFamily::Msvc { .. } => {
                cmd.push_cc_arg("-nologo".into());

                let crt_flag = match self.static_crt {
                    Some(true) => "-MT",
                    Some(false) => "-MD",
                    None => {
                        let features = self.getenv("CARGO_CFG_TARGET_FEATURE");
                        let features = features.as_deref().unwrap_or_default();
                        if features.to_string_lossy().contains("crt-static") {
                            "-MT"
                        } else {
                            "-MD"
                        }
                    }
                };
                cmd.push_cc_arg(crt_flag.into());

                match opt_level {
                    "z" | "s" | "1" => cmd.push_opt_unless_duplicate("-O1".into()),
                    "2" | "3" => cmd.push_opt_unless_duplicate("-O2".into()),
                    _ => {}
                }
            }
            ToolFamily::Gnu | ToolFamily::Clang { .. } => {
                if opt_level == "z" && !cmd.is_like_clang() {
                    cmd.push_opt_unless_duplicate("-Os".into());
                } else {
                    cmd.push_opt_unless_duplicate(format!("-O{opt_level}").into());
                }

                if cmd.is_like_clang() && target.os == "android" {
                    cmd.push_opt_unless_duplicate("-DANDROID".into());
                }

                if target.os != "ios"
                    && target.os != "watchos"
                    && target.os != "tvos"
                    && target.os != "visionos"
                {
                    cmd.push_cc_arg("-ffunction-sections".into());
                    cmd.push_cc_arg("-fdata-sections".into());
                }
                if self.pic.unwrap_or(
                    target.os != "windows"
                        && target.os != "none"
                        && target.os != "uefi"
                        && target.arch != "wasm32"
                        && target.arch != "wasm64",
                ) {
                    cmd.push_cc_arg("-fPIC".into());
                    if (target.os == "linux" || target.os == "android")
                        && !self.use_plt.unwrap_or(true)
                    {
                        cmd.push_cc_arg("-fno-plt".into());
                    }
                }
                if target.arch == "wasm32" || target.arch == "wasm64" {
                    cmd.push_cc_arg("-fno-exceptions".into());
                }

                if target.os == "wasi" {
                    if let Ok(wasi_sysroot) = self.wasi_sysroot() {
                        cmd.push_cc_arg(
                            format!("--sysroot={}", Path::new(&wasi_sysroot).display()).into(),
                        );
                    }

                    if raw_target.contains("threads") {
                        cmd.push_cc_arg("-pthread".into());
                    }
                }

                if target.os == "nto" {
                    let arg = match target.full_arch {
                        "x86" | "i586" => "-Vgcc_ntox86_cxx",
                        "aarch64" => "-Vgcc_ntoaarch64le_cxx",
                        "x86_64" => "-Vgcc_ntox86_64_cxx",
                        _ => {
                            return Err(Error::new(
                                ErrorKind::InvalidTarget,
                                format!("Unknown architecture for Neutrino QNX: {}", target.arch),
                            ))
                        }
                    };
                    cmd.push_cc_arg(arg.into());
                }
            }
        }

        if self.get_debug() {
            if self.cuda {
                cmd.args.push("-G".into());
            }
            let family = cmd.family;
            family.add_debug_flags(cmd, self.get_dwarf_version());
        }

        if self.get_force_frame_pointer() {
            let family = cmd.family;
            family.add_force_frame_pointer(cmd);
        }

        if !cmd.is_like_msvc() {
            if target.arch == "x86" {
                cmd.args.push("-m32".into());
            } else if target.abi == "x32" {
                cmd.args.push("-mx32".into());
            } else if target.os == "aix" {
                if cmd.family == ToolFamily::Gnu {
                    cmd.args.push("-maix64".into());
                } else {
                    cmd.args.push("-m64".into());
                }
            } else if target.arch == "x86_64" || target.arch == "powerpc64" {
                cmd.args.push("-m64".into());
            }
        }

        match cmd.family {
            ToolFamily::Clang { .. } => {
                if !(cmd.has_internal_target_arg
                    || (target.os == "android"
                        && android_clang_compiler_uses_target_arg_internally(&cmd.path)))
                {
                    if target.os == "freebsd" {
                        if self.cpp && self.cpp_set_stdlib.is_none() {
                            cmd.push_cc_arg("-stdlib=libc++".into());
                        }
                    } else if target.arch == "wasm32" && target.os == "linux" {
                        for x in &[
                            "atomics",
                            "bulk-memory",
                            "mutable-globals",
                            "sign-ext",
                            "exception-handling",
                        ] {
                            cmd.push_cc_arg(format!("-m{x}").into());
                        }
                        for x in &["wasm-exceptions", "declspec"] {
                            cmd.push_cc_arg(format!("-f{x}").into());
                        }
                        let musl_sysroot = self.wasm_musl_sysroot().unwrap();
                        cmd.push_cc_arg(
                            format!("--sysroot={}", Path::new(&musl_sysroot).display()).into(),
                        );
                        cmd.push_cc_arg("-pthread".into());
                    }
                    let clang_target = target.llvm_target(&self.get_raw_target()?, None);
                    cmd.push_cc_arg(format!("--target={clang_target}").into());
                }
            }
            ToolFamily::Msvc { clang_cl } => {
                cmd.push_cc_arg("-Brepro".into());

                if clang_cl {
                    if target.arch == "x86_64" {
                        cmd.push_cc_arg("-m64".into());
                    } else if target.arch == "x86" {
                        cmd.push_cc_arg("-m32".into());
                        cmd.push_cc_arg("-arch:SSE2".into());
                    } else {
                        cmd.push_cc_arg(
                            format!(
                                "--target={}",
                                target.llvm_target(&self.get_raw_target()?, None)
                            )
                            .into(),
                        );
                    }
                } else if target.full_arch == "i586" {
                    cmd.push_cc_arg("-arch:IA32".into());
                } else if target.full_arch == "arm64ec" {
                    cmd.push_cc_arg("-arm64EC".into());
                }
                if target.arch == "arm" {
                    cmd.args
                        .push("-D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1".into());
                }
            }
            ToolFamily::Gnu => {
                if target.vendor == "kmc" {
                    cmd.args.push("-finput-charset=utf-8".into());
                }

                if self.static_flag.is_none() {
                    let features = self.getenv("CARGO_CFG_TARGET_FEATURE");
                    let features = features.as_deref().unwrap_or_default();
                    if features.to_string_lossy().contains("crt-static") {
                        cmd.args.push("-static".into());
                    }
                }

                if (target.full_arch.starts_with("armv7")
                    || target.full_arch.starts_with("thumbv7"))
                    && (target.os == "linux" || target.vendor == "kmc")
                {
                    cmd.args.push("-march=armv7-a".into());

                    if target.abi == "eabihf" {
                        cmd.args.push("-mfpu=vfpv3-d16".into());
                        cmd.args.push("-mfloat-abi=hard".into());
                    }
                }

                if target.os == "android" && target.full_arch.contains("v7") {
                    cmd.args.push("-march=armv7-a".into());
                    cmd.args.push("-mthumb".into());
                    if !target.full_arch.contains("neon") {
                        cmd.args.push("-mfpu=vfpv3-d16".into());
                    }
                    cmd.args.push("-mfloat-abi=softfp".into());
                }

                if target.full_arch.contains("neon") {
                    cmd.args.push("-mfpu=neon-vfpv4".into());
                }

                if target.full_arch == "armv4t" && target.os == "linux" {
                    cmd.args.push("-march=armv4t".into());
                    cmd.args.push("-marm".into());
                    cmd.args.push("-mfloat-abi=soft".into());
                }

                if target.full_arch == "armv5te" && target.os == "linux" {
                    cmd.args.push("-march=armv5te".into());
                    cmd.args.push("-marm".into());
                    cmd.args.push("-mfloat-abi=soft".into());
                }

                if target.full_arch == "arm" && target.os == "linux" {
                    cmd.args.push("-march=armv6".into());
                    cmd.args.push("-marm".into());
                    if target.abi == "eabihf" {
                        cmd.args.push("-mfpu=vfp".into());
                    } else {
                        cmd.args.push("-mfloat-abi=soft".into());
                    }
                }

                if target.full_arch == "i586" && target.os == "linux" {
                    cmd.args.push("-march=pentium".into());
                }

                if target.full_arch == "i686" && target.os == "linux" {
                    cmd.args.push("-march=i686".into());
                }

                if target.arch == "x86" && target.env == "musl" {
                    cmd.args.push("-Wl,-melf_i386".into());
                }

                if target.arch == "arm" && target.os == "none" && target.abi == "eabihf" {
                    cmd.args.push("-mfloat-abi=hard".into())
                }
                if target.full_arch.starts_with("thumb") {
                    cmd.args.push("-mthumb".into());
                }
                if target.full_arch.starts_with("thumbv6m") {
                    cmd.args.push("-march=armv6s-m".into());
                }
                if target.full_arch.starts_with("thumbv7em") {
                    cmd.args.push("-march=armv7e-m".into());

                    if target.abi == "eabihf" {
                        cmd.args.push("-mfpu=fpv4-sp-d16".into())
                    }
                }
                if target.full_arch.starts_with("thumbv7m") {
                    cmd.args.push("-march=armv7-m".into());
                }
                if target.full_arch.starts_with("thumbv8m.base") {
                    cmd.args.push("-march=armv8-m.base".into());
                }
                if target.full_arch.starts_with("thumbv8m.main") {
                    cmd.args.push("-march=armv8-m.main".into());

                    if target.abi == "eabihf" {
                        cmd.args.push("-mfpu=fpv5-sp-d16".into())
                    }
                }
                if target.full_arch.starts_with("armebv7r") | target.full_arch.starts_with("armv7r")
                {
                    if target.full_arch.starts_with("armeb") {
                        cmd.args.push("-mbig-endian".into());
                    } else {
                        cmd.args.push("-mlittle-endian".into());
                    }

                    cmd.args.push("-marm".into());

                    cmd.args.push("-march=armv7-r".into());

                    if target.abi == "eabihf" {
                        cmd.args.push("-mfpu=vfpv3-d16".into())
                    }
                }
                if target.full_arch.starts_with("armv7a") {
                    cmd.args.push("-march=armv7-a".into());

                    if target.abi == "eabihf" {
                        cmd.args.push("-mfpu=vfpv3-d16".into());
                    }
                }
                if target.arch == "riscv32" || target.arch == "riscv64" {
                    let arch = &target.full_arch[5..];
                    if arch.starts_with("64") {
                        if matches!(target.os, "linux" | "freebsd" | "netbsd") {
                            cmd.args.push(("-march=rv64gc").into());
                            cmd.args.push("-mabi=lp64d".into());
                        } else {
                            cmd.args.push(("-march=rv".to_owned() + arch).into());
                            cmd.args.push("-mabi=lp64".into());
                        }
                    } else if arch.starts_with("32") {
                        if target.os == "linux" {
                            cmd.args.push(("-march=rv32gc").into());
                            cmd.args.push("-mabi=ilp32d".into());
                        } else {
                            cmd.args.push(("-march=rv".to_owned() + arch).into());
                            cmd.args.push("-mabi=ilp32".into());
                        }
                    } else {
                        cmd.args.push("-mcmodel=medany".into());
                    }
                }
            }
        }

        if target.os == "solaris" || target.os == "illumos" {
            cmd.args.push("-D_REENTRANT".into());
        }

        if self.static_flag.unwrap_or(false) {
            cmd.args.push("-static".into());
        }
        if self.shared_flag.unwrap_or(false) {
            cmd.args.push("-shared".into());
        }

        if self.cpp {
            match (self.cpp_set_stdlib.as_ref(), cmd.family) {
                (None, _) => {}
                (Some(stdlib), ToolFamily::Gnu) | (Some(stdlib), ToolFamily::Clang { .. }) => {
                    cmd.push_cc_arg(format!("-stdlib=lib{stdlib}").into());
                }
                _ => {
                    self.cargo_output.print_warning(&format_args!("cpp_set_stdlib is specified, but the {:?} compiler does not support this option, ignored", cmd.family));
                }
            }
        }

        Ok(())
    }

    fn add_inherited_rustflags(
        &self,
        cmd: &mut Tool,
        target: &TargetInfo<'_>,
    ) -> Result<(), Error> {
        let env_os = match self.getenv("CARGO_ENCODED_RUSTFLAGS") {
            Some(env) => env,
            None => return Ok(()),
        };

        let env = env_os.to_string_lossy();
        let codegen_flags = RustcCodegenFlags::parse(&env)?;
        codegen_flags.cc_flags(self, cmd, target);
        Ok(())
    }

    fn msvc_macro_assembler(&self) -> Result<Command, Error> {
        let target = self.get_target()?;
        let tool = if target.arch == "x86_64" {
            "ml64.exe"
        } else if target.arch == "arm" {
            "armasm.exe"
        } else if target.arch == "aarch64" {
            "armasm64.exe"
        } else {
            "ml.exe"
        };
        let mut cmd = self.cmd(tool);
        cmd.arg("-nologo"); 
        for directory in self.include_directories.iter() {
            cmd.arg("-I").arg(&**directory);
        }
        if target.arch == "aarch64" || target.arch == "arm" {
            if self.get_debug() {
                cmd.arg("-g");
            }

            for (key, value) in self.definitions.iter() {
                cmd.arg("-PreDefine");
                if let Some(ref value) = *value {
                    if let Ok(i) = value.parse::<i32>() {
                        cmd.arg(format!("{key} SETA {i}"));
                    } else if value.starts_with('"') && value.ends_with('"') {
                        cmd.arg(format!("{key} SETS {value}"));
                    } else {
                        cmd.arg(format!("{key} SETS \"{value}\""));
                    }
                } else {
                    cmd.arg(format!("{} SETL {}", key, "{TRUE}"));
                }
            }
        } else {
            if self.get_debug() {
                cmd.arg("-Zi");
            }

            for (key, value) in self.definitions.iter() {
                if let Some(ref value) = *value {
                    cmd.arg(format!("-D{key}={value}"));
                } else {
                    cmd.arg(format!("-D{key}"));
                }
            }
        }

        if target.arch == "x86" {
            cmd.arg("-safeseh");
        }

        Ok(cmd)
    }

    fn assemble(&self, lib_name: &str, dst: &Path, objs: &[Object]) -> Result<(), Error> {
        let _ = fs::remove_file(dst);

        let objs: Vec<_> = objs
            .iter()
            .map(|o| o.dst.as_path())
            .chain(self.objects.iter().map(std::ops::Deref::deref))
            .collect();
        for chunk in objs.chunks(100) {
            self.assemble_progressive(dst, chunk)?;
        }

        if self.cuda && self.cuda_file_count() > 0 {

            let out_dir = self.get_out_dir()?;
            let dlink = out_dir.join(lib_name.to_owned() + "_dlink.o");
            let mut nvcc = self.get_compiler().to_command();
            nvcc.arg("--device-link").arg("-o").arg(&dlink).arg(dst);
            run(&mut nvcc, &self.cargo_output)?;
            self.assemble_progressive(dst, &[dlink.as_path()])?;
        }

        let target = self.get_target()?;
        if target.env == "msvc" {

            let lib_dst = dst.with_file_name(format!("{lib_name}.lib"));
            let _ = fs::remove_file(&lib_dst);
            match fs::hard_link(dst, &lib_dst).or_else(|_| {
                fs::copy(dst, &lib_dst).map(|_| ())
            }) {
                Ok(_) => (),
                Err(_) => {
                    return Err(Error::new(
                        ErrorKind::IOError,
                        "Could not copy or create a hard-link to the generated lib file.",
                    ));
                }
            };
        } else {
            let mut ar = self.try_get_archiver()?;

            run(ar.arg("s").arg(dst), &self.cargo_output)?;
        }

        Ok(())
    }

    fn assemble_progressive(&self, dst: &Path, objs: &[&Path]) -> Result<(), Error> {
        let target = self.get_target()?;

        let (mut cmd, program, any_flags) = self.try_get_archiver_and_flags()?;
        if target.env == "msvc" && !program.to_string_lossy().contains("llvm-ar") {
            let mut out = OsString::from("-out:");
            out.push(dst);
            cmd.arg(out);
            if !any_flags {
                cmd.arg("-nologo");
            }
            if dst.exists() {
                cmd.arg(dst);
            }
            cmd.args(objs);
            run(&mut cmd, &self.cargo_output)?;
        } else {
            cmd.env("ZERO_AR_DATE", "1");

            run(cmd.arg("cq").arg(dst).args(objs), &self.cargo_output)?;
        }

        Ok(())
    }

    fn cmd<P: AsRef<OsStr>>(&self, prog: P) -> Command {
        let mut cmd = Command::new(prog);
        for (a, b) in self.env.iter() {
            cmd.env(a, b);
        }
        cmd
    }

    fn get_base_compiler(&self) -> Result<Tool, Error> {
        let out_dir = self.get_out_dir().ok();
        let out_dir = out_dir.as_deref();

        if let Some(c) = &self.compiler {
            return Ok(Tool::new(
                (**c).to_owned(),
                &self.build_cache.cached_compiler_family,
                &self.cargo_output,
                out_dir,
            ));
        }
        let target = self.get_target()?;
        let raw_target = self.get_raw_target()?;
        let (env, msvc, gnu, traditional, clang) = if self.cpp {
            ("CXX", "cl.exe", "g++", "c++", "clang++")
        } else {
            ("CC", "cl.exe", "gcc", "cc", "clang")
        };

        let default = if cfg!(target_os = "solaris") || cfg!(target_os = "illumos") {
            gnu
        } else {
            traditional
        };

        let cl_exe: Option<Tool> = None;

        let tool_opt: Option<Tool> = self
            .env_tool(env)
            .map(|(tool, wrapper, args)| {
                let mut t = Tool::with_args(
                    tool,
                    args.clone(),
                    &self.build_cache.cached_compiler_family,
                    &self.cargo_output,
                    out_dir,
                );
                if let Some(cc_wrapper) = wrapper {
                    t.cc_wrapper_path = Some(Path::new(&cc_wrapper).to_owned());
                }
                for arg in args {
                    t.cc_wrapper_args.push(arg.into());
                }
                t
            })
            .or_else(|| {
                if target.os == "emscripten" {
                    let tool = if self.cpp { "em++" } else { "emcc" };
                    if cfg!(windows) {
                        let mut t = Tool::with_family(
                            PathBuf::from("cmd"),
                            ToolFamily::Clang { zig_cc: false },
                        );
                        t.args.push("/c".into());
                        t.args.push(format!("{tool}.bat").into());
                        Some(t)
                    } else {
                        Some(Tool::new(
                            PathBuf::from(tool),
                            &self.build_cache.cached_compiler_family,
                            &self.cargo_output,
                            out_dir,
                        ))
                    }
                } else {
                    None
                }
            })
            .or_else(|| cl_exe.clone());

        let tool = match tool_opt {
            Some(t) => t,
            None => {
                let compiler = if cfg!(windows) && target.os == "windows" {
                    if target.env == "msvc" {
                        msvc.to_string()
                    } else {
                        let cc = if target.abi == "llvm" { clang } else { gnu };
                        format!("{cc}.exe")
                    }
                } else if target.os == "ios"
                    || target.os == "watchos"
                    || target.os == "tvos"
                    || target.os == "visionos"
                {
                    clang.to_string()
                } else if target.os == "android" {
                    autodetect_android_compiler(&raw_target, gnu, clang)
                } else if target.os == "cloudabi" {
                    format!(
                        "{}-{}-{}-{}",
                        target.full_arch, target.vendor, target.os, traditional
                    )
                } else if target.arch == "wasm32" || target.arch == "wasm64" {
                    clang.to_string()
                } else if target.os == "vxworks" {
                    if self.cpp {
                        "wr-c++".to_string()
                    } else {
                        "wr-cc".to_string()
                    }
                } else if target.arch == "arm" && target.vendor == "kmc" {
                    format!("arm-kmc-eabi-{gnu}")
                } else if target.arch == "aarch64" && target.vendor == "kmc" {
                    format!("aarch64-kmc-elf-{gnu}")
                } else if target.os == "nto" {
                    if self.cpp {
                        "q++".to_string()
                    } else {
                        "qcc".to_string()
                    }
                } else if self.get_is_cross_compile()? {
                    let prefix = self.prefix_for_target(&raw_target);
                    match prefix {
                        Some(prefix) => {
                            let cc = if target.abi == "llvm" { clang } else { gnu };
                            format!("{prefix}-{cc}")
                        }
                        None => default.to_string(),
                    }
                } else {
                    default.to_string()
                };

                let mut t = Tool::new(
                    PathBuf::from(compiler),
                    &self.build_cache.cached_compiler_family,
                    &self.cargo_output,
                    out_dir,
                );
                if let Some(cc_wrapper) = self.rustc_wrapper_fallback() {
                    t.cc_wrapper_path = Some(Path::new(&cc_wrapper).to_owned());
                }
                t
            }
        };

        let mut tool = if self.cuda {
            assert!(
                tool.args.is_empty(),
                "CUDA compilation currently assumes empty pre-existing args"
            );
            let nvcc = match self.getenv_with_target_prefixes("NVCC") {
                Err(_) => PathBuf::from("nvcc"),
                Ok(nvcc) => PathBuf::from(&*nvcc),
            };
            let mut nvcc_tool = Tool::with_features(
                nvcc,
                vec![],
                self.cuda,
                &self.build_cache.cached_compiler_family,
                &self.cargo_output,
                out_dir,
            );
            if self.ccbin {
                nvcc_tool
                    .args
                    .push(format!("-ccbin={}", tool.path.display()).into());
            }
            if let Some(cc_wrapper) = self.rustc_wrapper_fallback() {
                nvcc_tool.cc_wrapper_path = Some(Path::new(&cc_wrapper).to_owned());
            }
            nvcc_tool.family = tool.family;
            nvcc_tool
        } else {
            tool
        };

        if cfg!(windows) && android_clang_compiler_uses_target_arg_internally(&tool.path) {
            if let Some(path) = tool.path.file_name() {
                let file_name = path.to_str().unwrap().to_owned();
                let (target, clang) = file_name.split_at(file_name.rfind('-').unwrap());

                tool.has_internal_target_arg = true;
                tool.path.set_file_name(clang.trim_start_matches('-'));
                tool.path.set_extension("exe");
                tool.args.push(format!("--target={target}").into());

                if target.contains("i686-linux-android") {
                    let (_, version) = target.split_at(target.rfind('d').unwrap() + 1);
                    if let Ok(version) = version.parse::<u32>() {
                        if version > 15 && version < 25 {
                            tool.args.push("-mstackrealign".into());
                        }
                    }
                }
            };
        }

        if let Some(cl_exe) = cl_exe {
            if tool.family == (ToolFamily::Msvc { clang_cl: true })
                && tool.env.is_empty()
                && target.env == "msvc"
            {
                for (k, v) in cl_exe.env.iter() {
                    tool.env.push((k.to_owned(), v.to_owned()));
                }
            }
        }

        if target.env == "msvc" && tool.family == ToolFamily::Gnu {
            self.cargo_output
                .print_warning(&"GNU compiler is not supported for this target");
        }

        Ok(tool)
    }

    /// Returns a fallback `cc_compiler_wrapper` by introspecting `RUSTC_WRAPPER`
    fn rustc_wrapper_fallback(&self) -> Option<Arc<OsStr>> {
        const VALID_WRAPPERS: &[&str] = &["sccache", "cachepot", "buildcache"];

        let rustc_wrapper = self.getenv("RUSTC_WRAPPER")?;
        let wrapper_path = Path::new(&rustc_wrapper);
        let wrapper_stem = wrapper_path.file_stem()?;

        if VALID_WRAPPERS.contains(&wrapper_stem.to_str()?) {
            Some(rustc_wrapper)
        } else {
            None
        }
    }

    /// Returns compiler path, optional modifier name from whitelist, and arguments vec
    fn env_tool(&self, name: &str) -> Option<(PathBuf, Option<Arc<OsStr>>, Vec<String>)> {
        let tool = self.getenv_with_target_prefixes(name).ok()?;
        let tool = tool.to_string_lossy();
        let tool = tool.trim();

        if tool.is_empty() {
            return None;
        }

        if Path::new(tool).exists() {
            return Some((
                PathBuf::from(tool),
                self.rustc_wrapper_fallback(),
                Vec::new(),
            ));
        }

        let mut known_wrappers = vec![
            "ccache",
            "distcc",
            "sccache",
            "icecc",
            "cachepot",
            "buildcache",
        ];
        let custom_wrapper = self.getenv("CC_KNOWN_WRAPPER_CUSTOM");
        if custom_wrapper.is_some() {
            known_wrappers.push(custom_wrapper.as_deref().unwrap().to_str().unwrap());
        }

        let mut parts = tool.split_whitespace();
        let maybe_wrapper = parts.next()?;

        let file_stem = Path::new(maybe_wrapper).file_stem()?.to_str()?;
        if known_wrappers.contains(&file_stem) {
            if let Some(compiler) = parts.next() {
                return Some((
                    compiler.into(),
                    Some(Arc::<OsStr>::from(OsStr::new(&maybe_wrapper))),
                    parts.map(|s| s.to_string()).collect(),
                ));
            }
        }

        Some((
            maybe_wrapper.into(),
            self.rustc_wrapper_fallback(),
            parts.map(|s| s.to_string()).collect(),
        ))
    }

    /// Returns the C++ standard library:
    /// 1. If [`cpp_link_stdlib`](cc::Build::cpp_link_stdlib) is set, uses its value.
    /// 2. Else if the `CXXSTDLIB` environment variable is set, uses its value.
    /// 3. Else the default is `c++` for OS X and BSDs, `c++_shared` for Android,
    ///    `None` for MSVC and `stdc++` for anything else.
    fn get_cpp_link_stdlib(&self) -> Result<Option<Cow<'_, Path>>, Error> {
        match &self.cpp_link_stdlib {
            Some(s) => Ok(s.as_deref().map(Path::new).map(Cow::Borrowed)),
            None => {
                if let Ok(stdlib) = self.getenv_with_target_prefixes("CXXSTDLIB") {
                    if stdlib.is_empty() {
                        Ok(None)
                    } else {
                        Ok(Some(Cow::Owned(Path::new(&stdlib).to_owned())))
                    }
                } else {
                    let target = self.get_target()?;
                    if target.env == "msvc" {
                        Ok(None)
                    } else if target.vendor == "apple"
                        || target.os == "freebsd"
                        || target.os == "openbsd"
                        || target.os == "aix"
                        || (target.os == "linux" && target.env == "ohos")
                        || target.os == "wasi"
                    {
                        Ok(Some(Cow::Borrowed(Path::new("c++"))))
                    } else if target.os == "android" {
                        Ok(Some(Cow::Borrowed(Path::new("c++_shared"))))
                    } else {
                        Ok(Some(Cow::Borrowed(Path::new("stdc++"))))
                    }
                }
            }
        }
    }

    /// Get the archiver (ar) that's in use for this configuration.
    ///
    /// You can use [`Command::get_program`] to get just the path to the command.
    ///
    /// This method will take into account all configuration such as debug
    /// information, optimization level, include directories, defines, etc.
    /// Additionally, the compiler binary in use follows the standard
    /// conventions for this path, e.g. looking at the explicitly set compiler,
    /// environment variables (a number of which are inspected here), and then
    /// falling back to the default configuration.
    ///
    /// # Panics
    ///
    /// Panics if an error occurred while determining the architecture.
    pub fn get_archiver(&self) -> Command {
        match self.try_get_archiver() {
            Ok(tool) => tool,
            Err(e) => fail(&e.message),
        }
    }

    /// Get the archiver that's in use for this configuration.
    ///
    /// This will return a result instead of panicking;
    /// see [`Self::get_archiver`] for the complete description.
    pub fn try_get_archiver(&self) -> Result<Command, Error> {
        Ok(self.try_get_archiver_and_flags()?.0)
    }

    fn try_get_archiver_and_flags(&self) -> Result<(Command, PathBuf, bool), Error> {
        let (mut cmd, name) = self.get_base_archiver()?;
        let mut any_flags = false;
        if let Some(flags) = self.envflags("ARFLAGS")? {
            any_flags = true;
            cmd.args(flags);
        }
        for flag in &self.ar_flags {
            any_flags = true;
            cmd.arg(&**flag);
        }
        Ok((cmd, name, any_flags))
    }

    fn get_base_archiver(&self) -> Result<(Command, PathBuf), Error> {
        if let Some(ref a) = self.archiver {
            let archiver = &**a;
            return Ok((self.cmd(archiver), archiver.into()));
        }

        self.get_base_archiver_variant("AR", "ar")
    }

    /// Get the ranlib that's in use for this configuration.
    ///
    /// You can use [`Command::get_program`] to get just the path to the command.
    ///
    /// This method will take into account all configuration such as debug
    /// information, optimization level, include directories, defines, etc.
    /// Additionally, the compiler binary in use follows the standard
    /// conventions for this path, e.g. looking at the explicitly set compiler,
    /// environment variables (a number of which are inspected here), and then
    /// falling back to the default configuration.
    ///
    /// # Panics
    ///
    /// Panics if an error occurred while determining the architecture.
    pub fn get_ranlib(&self) -> Command {
        match self.try_get_ranlib() {
            Ok(tool) => tool,
            Err(e) => fail(&e.message),
        }
    }

    /// Get the ranlib that's in use for this configuration.
    ///
    /// This will return a result instead of panicking;
    /// see [`Self::get_ranlib`] for the complete description.
    pub fn try_get_ranlib(&self) -> Result<Command, Error> {
        let mut cmd = self.get_base_ranlib()?;
        if let Some(flags) = self.envflags("RANLIBFLAGS")? {
            cmd.args(flags);
        }
        Ok(cmd)
    }

    fn get_base_ranlib(&self) -> Result<Command, Error> {
        if let Some(ref r) = self.ranlib {
            return Ok(self.cmd(&**r));
        }

        Ok(self.get_base_archiver_variant("RANLIB", "ranlib")?.0)
    }

    fn get_base_archiver_variant(
        &self,
        env: &str,
        tool: &str,
    ) -> Result<(Command, PathBuf), Error> {
        let target = self.get_target()?;
        let mut name = PathBuf::new();
        let tool_opt: Option<Command> = self
            .env_tool(env)
            .map(|(tool, _wrapper, args)| {
                name.clone_from(&tool);
                let mut cmd = self.cmd(tool);
                cmd.args(args);
                cmd
            })
            .or_else(|| {
                if target.os == "emscripten" {
                    if cfg!(windows) {
                        let mut cmd = self.cmd("cmd");
                        name = format!("em{tool}.bat").into();
                        cmd.arg("/c").arg(&name);
                        Some(cmd)
                    } else {
                        name = format!("em{tool}").into();
                        Some(self.cmd(&name))
                    }
                } else if target.arch == "wasm32" || target.arch == "wasm64" {
                    let compiler = self.get_base_compiler().ok()?;
                    if compiler.is_like_clang() {
                        name = format!("llvm-{tool}").into();
                        self.search_programs(
                            &mut self.cmd(&compiler.path),
                            &name,
                            &self.cargo_output,
                        )
                        .map(|name| self.cmd(name))
                    } else {
                        None
                    }
                } else {
                    None
                }
            });

        let tool = match tool_opt {
            Some(t) => t,
            None => {
                if target.os == "android" {
                    name = format!("llvm-{tool}").into();
                    match Command::new(&name).arg("--version").status() {
                        Ok(status) if status.success() => (),
                        _ => {
                            let raw_target = self.get_raw_target()?;
                            name = format!("{}-{}", raw_target.replace("armv7", "arm"), tool).into()
                        }
                    }
                    self.cmd(&name)
                } else if target.env == "msvc" {

                    let compiler = self.get_base_compiler()?;
                    let mut lib = String::new();
                    if compiler.family == (ToolFamily::Msvc { clang_cl: true }) {
                        if let Some(mut cmd) = self.which(&compiler.path, None) {
                            cmd.pop();
                            cmd.push("llvm-lib.exe");
                            if let Some(llvm_lib) = self.which(&cmd, None) {
                                llvm_lib.to_str().unwrap().clone_into(&mut lib);
                            }
                        }
                    }

                    if lib.is_empty() {
                        name = PathBuf::from("lib.exe");
                        let mut cmd = self.cmd("lib.exe");
                        if target.full_arch == "arm64ec" {
                            cmd.arg("/machine:arm64ec");
                        }
                        cmd
                    } else {
                        name = lib.into();
                        self.cmd(&name)
                    }
                } else if target.os == "illumos" {
                    name = format!("g{tool}").into();
                    self.cmd(&name)
                } else if target.os == "vxworks" {
                    name = format!("wr-{tool}").into();
                    self.cmd(&name)
                } else if target.os == "nto" {
                    name = match target.full_arch {
                        "i586" => format!("ntox86-{tool}").into(),
                        "x86" | "aarch64" | "x86_64" => {
                            format!("nto{}-{}", target.arch, tool).into()
                        }
                        _ => {
                            return Err(Error::new(
                                ErrorKind::InvalidTarget,
                                format!("Unknown architecture for Neutrino QNX: {}", target.arch),
                            ))
                        }
                    };
                    self.cmd(&name)
                } else if self.get_is_cross_compile()? {
                    match self.prefix_for_target(&self.get_raw_target()?) {
                        Some(prefix) => {
                            let chosen = ["", "-gcc"]
                                .iter()
                                .filter_map(|infix| {
                                    let target_p = format!("{prefix}{infix}-{tool}");
                                    let status = Command::new(&target_p)
                                        .arg("--version")
                                        .stdin(Stdio::null())
                                        .stdout(Stdio::null())
                                        .stderr(Stdio::null())
                                        .status()
                                        .ok()?;
                                    status.success().then_some(target_p)
                                })
                                .next()
                                .unwrap_or_else(|| tool.to_string());
                            name = chosen.into();
                            self.cmd(&name)
                        }
                        None => {
                            name = tool.into();
                            self.cmd(&name)
                        }
                    }
                } else {
                    name = tool.into();
                    self.cmd(&name)
                }
            }
        };

        Ok((tool, name))
    }

    fn prefix_for_target(&self, target: &str) -> Option<Cow<'static, str>> {
        self.getenv("CROSS_COMPILE")
            .as_deref()
            .map(|s| s.to_string_lossy().trim_end_matches('-').to_owned())
            .map(Cow::Owned)
            .or_else(|| {
                self.getenv("RUSTC_LINKER").and_then(|var| {
                    var.to_string_lossy()
                        .strip_suffix("-gcc")
                        .map(str::to_string)
                        .map(Cow::Owned)
                })
            })
            .or_else(|| {
                match target {
                    "aarch64-pc-windows-gnullvm" => Some("aarch64-w64-mingw32"),
                    "aarch64-uwp-windows-gnu" => Some("aarch64-w64-mingw32"),
                    "aarch64-unknown-linux-gnu" => Some("aarch64-linux-gnu"),
                    "aarch64-unknown-linux-musl" => Some("aarch64-linux-musl"),
                    "aarch64-unknown-netbsd" => Some("aarch64--netbsd"),
                    "arm-unknown-linux-gnueabi" => Some("arm-linux-gnueabi"),
                    "armv4t-unknown-linux-gnueabi" => Some("arm-linux-gnueabi"),
                    "armv5te-unknown-linux-gnueabi" => Some("arm-linux-gnueabi"),
                    "armv5te-unknown-linux-musleabi" => Some("arm-linux-gnueabi"),
                    "arm-unknown-linux-gnueabihf" => Some("arm-linux-gnueabihf"),
                    "arm-unknown-linux-musleabi" => Some("arm-linux-musleabi"),
                    "arm-unknown-linux-musleabihf" => Some("arm-linux-musleabihf"),
                    "arm-unknown-netbsd-eabi" => Some("arm--netbsdelf-eabi"),
                    "armv6-unknown-netbsd-eabihf" => Some("armv6--netbsdelf-eabihf"),
                    "armv7-unknown-linux-gnueabi" => Some("arm-linux-gnueabi"),
                    "armv7-unknown-linux-gnueabihf" => Some("arm-linux-gnueabihf"),
                    "armv7-unknown-linux-musleabihf" => Some("arm-linux-musleabihf"),
                    "armv7neon-unknown-linux-gnueabihf" => Some("arm-linux-gnueabihf"),
                    "armv7neon-unknown-linux-musleabihf" => Some("arm-linux-musleabihf"),
                    "thumbv7-unknown-linux-gnueabihf" => Some("arm-linux-gnueabihf"),
                    "thumbv7-unknown-linux-musleabihf" => Some("arm-linux-musleabihf"),
                    "thumbv7neon-unknown-linux-gnueabihf" => Some("arm-linux-gnueabihf"),
                    "thumbv7neon-unknown-linux-musleabihf" => Some("arm-linux-musleabihf"),
                    "armv7-unknown-netbsd-eabihf" => Some("armv7--netbsdelf-eabihf"),
                    "hexagon-unknown-linux-musl" => Some("hexagon-linux-musl"),
                    "i586-unknown-linux-musl" => Some("musl"),
                    "i686-pc-windows-gnu" => Some("i686-w64-mingw32"),
                    "i686-pc-windows-gnullvm" => Some("i686-w64-mingw32"),
                    "i686-uwp-windows-gnu" => Some("i686-w64-mingw32"),
                    "i686-unknown-linux-gnu" => self.find_working_gnu_prefix(&[
                        "i686-linux-gnu",
                        "x86_64-linux-gnu", 
                    ]), 
                    "i686-unknown-linux-musl" => Some("musl"),
                    "i686-unknown-netbsd" => Some("i486--netbsdelf"),
                    "loongarch64-unknown-linux-gnu" => Some("loongarch64-linux-gnu"),
                    "mips-unknown-linux-gnu" => Some("mips-linux-gnu"),
                    "mips-unknown-linux-musl" => Some("mips-linux-musl"),
                    "mipsel-unknown-linux-gnu" => Some("mipsel-linux-gnu"),
                    "mipsel-unknown-linux-musl" => Some("mipsel-linux-musl"),
                    "mips64-unknown-linux-gnuabi64" => Some("mips64-linux-gnuabi64"),
                    "mips64el-unknown-linux-gnuabi64" => Some("mips64el-linux-gnuabi64"),
                    "mipsisa32r6-unknown-linux-gnu" => Some("mipsisa32r6-linux-gnu"),
                    "mipsisa32r6el-unknown-linux-gnu" => Some("mipsisa32r6el-linux-gnu"),
                    "mipsisa64r6-unknown-linux-gnuabi64" => Some("mipsisa64r6-linux-gnuabi64"),
                    "mipsisa64r6el-unknown-linux-gnuabi64" => Some("mipsisa64r6el-linux-gnuabi64"),
                    "powerpc-unknown-linux-gnu" => Some("powerpc-linux-gnu"),
                    "powerpc-unknown-linux-gnuspe" => Some("powerpc-linux-gnuspe"),
                    "powerpc-unknown-netbsd" => Some("powerpc--netbsd"),
                    "powerpc64-unknown-linux-gnu" => Some("powerpc64-linux-gnu"),
                    "powerpc64le-unknown-linux-gnu" => Some("powerpc64le-linux-gnu"),
                    "riscv32i-unknown-none-elf" => self.find_working_gnu_prefix(&[
                        "riscv32-unknown-elf",
                        "riscv64-unknown-elf",
                        "riscv-none-embed",
                    ]),
                    "riscv32imac-esp-espidf" => Some("riscv32-esp-elf"),
                    "riscv32imac-unknown-none-elf" => self.find_working_gnu_prefix(&[
                        "riscv32-unknown-elf",
                        "riscv64-unknown-elf",
                        "riscv-none-embed",
                    ]),
                    "riscv32imac-unknown-xous-elf" => self.find_working_gnu_prefix(&[
                        "riscv32-unknown-elf",
                        "riscv64-unknown-elf",
                        "riscv-none-embed",
                    ]),
                    "riscv32imc-esp-espidf" => Some("riscv32-esp-elf"),
                    "riscv32imc-unknown-none-elf" => self.find_working_gnu_prefix(&[
                        "riscv32-unknown-elf",
                        "riscv64-unknown-elf",
                        "riscv-none-embed",
                    ]),
                    "riscv64gc-unknown-none-elf" => self.find_working_gnu_prefix(&[
                        "riscv64-unknown-elf",
                        "riscv32-unknown-elf",
                        "riscv-none-embed",
                    ]),
                    "riscv64imac-unknown-none-elf" => self.find_working_gnu_prefix(&[
                        "riscv64-unknown-elf",
                        "riscv32-unknown-elf",
                        "riscv-none-embed",
                    ]),
                    "riscv64gc-unknown-linux-gnu" => Some("riscv64-linux-gnu"),
                    "riscv32gc-unknown-linux-gnu" => Some("riscv32-linux-gnu"),
                    "riscv64gc-unknown-linux-musl" => Some("riscv64-linux-musl"),
                    "riscv32gc-unknown-linux-musl" => Some("riscv32-linux-musl"),
                    "riscv64gc-unknown-netbsd" => Some("riscv64--netbsd"),
                    "s390x-unknown-linux-gnu" => Some("s390x-linux-gnu"),
                    "sparc-unknown-linux-gnu" => Some("sparc-linux-gnu"),
                    "sparc64-unknown-linux-gnu" => Some("sparc64-linux-gnu"),
                    "sparc64-unknown-netbsd" => Some("sparc64--netbsd"),
                    "sparcv9-sun-solaris" => Some("sparcv9-sun-solaris"),
                    "armv7a-none-eabi" => Some("arm-none-eabi"),
                    "armv7a-none-eabihf" => Some("arm-none-eabi"),
                    "armebv7r-none-eabi" => Some("arm-none-eabi"),
                    "armebv7r-none-eabihf" => Some("arm-none-eabi"),
                    "armv7r-none-eabi" => Some("arm-none-eabi"),
                    "armv7r-none-eabihf" => Some("arm-none-eabi"),
                    "armv8r-none-eabihf" => Some("arm-none-eabi"),
                    "thumbv6m-none-eabi" => Some("arm-none-eabi"),
                    "thumbv7em-none-eabi" => Some("arm-none-eabi"),
                    "thumbv7em-none-eabihf" => Some("arm-none-eabi"),
                    "thumbv7m-none-eabi" => Some("arm-none-eabi"),
                    "thumbv8m.base-none-eabi" => Some("arm-none-eabi"),
                    "thumbv8m.main-none-eabi" => Some("arm-none-eabi"),
                    "thumbv8m.main-none-eabihf" => Some("arm-none-eabi"),
                    "x86_64-pc-windows-gnu" => Some("x86_64-w64-mingw32"),
                    "x86_64-pc-windows-gnullvm" => Some("x86_64-w64-mingw32"),
                    "x86_64-uwp-windows-gnu" => Some("x86_64-w64-mingw32"),
                    "x86_64-rumprun-netbsd" => Some("x86_64-rumprun-netbsd"),
                    "x86_64-unknown-linux-gnu" => self.find_working_gnu_prefix(&[
                        "x86_64-linux-gnu", // rustfmt wrap
                    ]), 
                    "x86_64-unknown-linux-musl" => {
                        self.find_working_gnu_prefix(&["x86_64-linux-musl", "musl"])
                    }
                    "x86_64-unknown-netbsd" => Some("x86_64--netbsd"),
                    _ => None,
                }
                .map(Cow::Borrowed)
            })
    }

    /// Some platforms have multiple, compatible, canonical prefixes. Look through
    /// each possible prefix for a compiler that exists and return it. The prefixes
    /// should be ordered from most-likely to least-likely.
    fn find_working_gnu_prefix(&self, prefixes: &[&'static str]) -> Option<&'static str> {
        let suffix = if self.cpp { "-g++" } else { "-gcc" };
        let extension = std::env::consts::EXE_SUFFIX;

        self.getenv("PATH")
            .as_ref()
            .and_then(|path_entries| {
                env::split_paths(path_entries).find_map(|path_entry| {
                    for prefix in prefixes {
                        let target_compiler = format!("{prefix}{suffix}{extension}");
                        if path_entry.join(&target_compiler).exists() {
                            return Some(prefix);
                        }
                    }
                    None
                })
            })
            .copied()
            .or_else(|| prefixes.first().copied())
    }

    fn get_target(&self) -> Result<TargetInfo<'_>, Error> {
        match &self.target {
            Some(t) if Some(&**t) != self.getenv_unwrap_str("TARGET").ok().as_deref() => {
                TargetInfo::from_rustc_target(t)
            }
            _ => self
                .build_cache
                .target_info_parser
                .parse_from_cargo_environment_variables(),
        }
    }

    fn get_raw_target(&self) -> Result<Cow<'_, str>, Error> {
        match &self.target {
            Some(t) => Ok(Cow::Borrowed(t)),
            None => self.getenv_unwrap_str("TARGET").map(Cow::Owned),
        }
    }

    fn get_is_cross_compile(&self) -> Result<bool, Error> {
        let target = self.get_raw_target()?;
        let host: Cow<'_, str> = match &self.host {
            Some(h) => Cow::Borrowed(h),
            None => Cow::Owned(self.getenv_unwrap_str("HOST")?),
        };
        Ok(host != target)
    }

    fn get_opt_level(&self) -> Result<Cow<'_, str>, Error> {
        match &self.opt_level {
            Some(ol) => Ok(Cow::Borrowed(ol)),
            None => self.getenv_unwrap_str("OPT_LEVEL").map(Cow::Owned),
        }
    }

    fn get_debug(&self) -> bool {
        self.debug.unwrap_or_else(|| self.getenv_boolean("DEBUG"))
    }

    fn get_shell_escaped_flags(&self) -> bool {
        self.shell_escaped_flags
            .unwrap_or_else(|| self.getenv_boolean("CC_SHELL_ESCAPED_FLAGS"))
    }

    fn get_dwarf_version(&self) -> Option<u32> {
        let target = self.get_target().ok()?;
        if matches!(
            target.os,
            "android" | "dragonfly" | "freebsd" | "netbsd" | "openbsd"
        ) || target.vendor == "apple"
            || (target.os == "windows" && target.env == "gnu")
        {
            Some(2)
        } else if target.os == "linux" {
            Some(4)
        } else {
            None
        }
    }

    fn get_force_frame_pointer(&self) -> bool {
        self.force_frame_pointer.unwrap_or_else(|| self.get_debug())
    }

    fn get_out_dir(&self) -> Result<Cow<'_, Path>, Error> {
        match &self.out_dir {
            Some(p) => Ok(Cow::Borrowed(&**p)),
            None => self
                .getenv("OUT_DIR")
                .as_deref()
                .map(PathBuf::from)
                .map(Cow::Owned)
                .ok_or_else(|| {
                    Error::new(
                        ErrorKind::EnvVarNotFound,
                        "Environment variable OUT_DIR not defined.",
                    )
                }),
        }
    }

    #[allow(clippy::disallowed_methods)]
    fn getenv(&self, v: &str) -> Option<Arc<OsStr>> {
        fn provided_by_cargo(envvar: &str) -> bool {
            match envvar {
                v if v.starts_with("CARGO") || v.starts_with("RUSTC") => true,
                "HOST" | "TARGET" | "RUSTDOC" | "OUT_DIR" | "OPT_LEVEL" | "DEBUG" | "PROFILE"
                | "NUM_JOBS" | "RUSTFLAGS" => true,
                _ => false,
            }
        }
        if let Some(val) = self.build_cache.env_cache.read().unwrap().get(v).cloned() {
            return val;
        }
        if self.emit_rerun_if_env_changed && !provided_by_cargo(v) && v != "PATH" {
            self.cargo_output
                .print_metadata(&format_args!("cargo:rerun-if-env-changed={v}"));
        }
        let r = env::var_os(v).map(Arc::from);
        self.cargo_output.print_metadata(&format_args!(
            "{} = {}",
            v,
            OptionOsStrDisplay(r.as_deref())
        ));
        self.build_cache
            .env_cache
            .write()
            .unwrap()
            .insert(v.into(), r.clone());
        r
    }

    /// get boolean flag that is either true or false
    fn getenv_boolean(&self, v: &str) -> bool {
        match self.getenv(v) {
            Some(s) => &*s != "0" && &*s != "false" && !s.is_empty(),
            None => false,
        }
    }

    fn getenv_unwrap(&self, v: &str) -> Result<Arc<OsStr>, Error> {
        match self.getenv(v) {
            Some(s) => Ok(s),
            None => Err(Error::new(
                ErrorKind::EnvVarNotFound,
                format!("Environment variable {v} not defined."),
            )),
        }
    }

    fn getenv_unwrap_str(&self, v: &str) -> Result<String, Error> {
        let env = self.getenv_unwrap(v)?;
        env.to_str().map(String::from).ok_or_else(|| {
            Error::new(
                ErrorKind::EnvVarNotFound,
                format!("Environment variable {v} is not valid utf-8."),
            )
        })
    }

    /// The list of environment variables to check for a given env, in order of priority.
    fn target_envs(&self, env: &str) -> Result<[String; 4], Error> {
        let target = self.get_raw_target()?;
        let kind = if self.get_is_cross_compile()? {
            "TARGET"
        } else {
            "HOST"
        };
        let target_u = target.replace('-', "_");

        Ok([
            format!("{env}_{target}"),
            format!("{env}_{target_u}"),
            format!("{kind}_{env}"),
            env.to_string(),
        ])
    }

    /// Get a single-valued environment variable with target variants.
    fn getenv_with_target_prefixes(&self, env: &str) -> Result<Arc<OsStr>, Error> {
        let res = self
            .target_envs(env)?
            .iter()
            .filter_map(|env| self.getenv(env))
            .next();

        match res {
            Some(res) => Ok(res),
            None => Err(Error::new(
                ErrorKind::EnvVarNotFound,
                format!("could not find environment variable {env}"),
            )),
        }
    }

    /// Get values from CFLAGS-style environment variable.
    fn envflags(&self, env: &str) -> Result<Option<Vec<String>>, Error> {
        let mut any_set = false;
        let mut res = vec![];
        for env in self.target_envs(env)?.iter().rev() {
            if let Some(var) = self.getenv(env) {
                any_set = true;

                let var = var.to_string_lossy();
                if self.get_shell_escaped_flags() {
                    res.extend(Shlex::new(&var));
                } else {
                    res.extend(var.split_ascii_whitespace().map(ToString::to_string));
                }
            }
        }

        Ok(if any_set { Some(res) } else { None })
    }

    fn wasm_musl_sysroot(&self) -> Result<Arc<OsStr>, Error> {
        if let Some(musl_sysroot_path) = self.getenv("WASM_MUSL_SYSROOT") {
            Ok(musl_sysroot_path)
        } else {
            Err(Error::new(
                ErrorKind::EnvVarNotFound,
                "Environment variable WASM_MUSL_SYSROOT not defined for wasm32. Download sysroot from GitHub & setup environment variable MUSL_SYSROOT targeting the folder.",
            ))
        }
    }

    fn wasi_sysroot(&self) -> Result<Arc<OsStr>, Error> {
        if let Some(wasi_sysroot_path) = self.getenv("WASI_SYSROOT") {
            Ok(wasi_sysroot_path)
        } else {
            Err(Error::new(
                ErrorKind::EnvVarNotFound,
                "Environment variable WASI_SYSROOT not defined. Download sysroot from GitHub & setup environment variable WASI_SYSROOT targeting the folder.",
            ))
        }
    }

    fn cuda_file_count(&self) -> usize {
        self.files
            .iter()
            .filter(|file| file.extension() == Some(OsStr::new("cu")))
            .count()
    }

    fn which(&self, tool: &Path, path_entries: Option<&OsStr>) -> Option<PathBuf> {
        fn check_exe(mut exe: PathBuf) -> Option<PathBuf> {
            let exe_ext = std::env::consts::EXE_EXTENSION;
            let check =
                exe.exists() || (!exe_ext.is_empty() && exe.set_extension(exe_ext) && exe.exists());
            check.then_some(exe)
        }

        let find_exe_in_path = |path_entries: &OsStr| -> Option<PathBuf> {
            env::split_paths(path_entries).find_map(|path_entry| check_exe(path_entry.join(tool)))
        };

        if tool.components().count() > 1 {
            check_exe(PathBuf::from(tool))
        } else {
            path_entries
                .and_then(find_exe_in_path)
                .or_else(|| find_exe_in_path(&self.getenv("PATH")?))
        }
    }

    /// search for |prog| on 'programs' path in '|cc| -print-search-dirs' output
    fn search_programs(
        &self,
        cc: &mut Command,
        prog: &Path,
        cargo_output: &CargoOutput,
    ) -> Option<PathBuf> {
        let search_dirs = run_output(
            cc.arg("-print-search-dirs"),
            cargo_output,
        )
        .ok()?;
        let search_dirs = std::str::from_utf8(&search_dirs).ok()?;
        for dirs in search_dirs.split(['\r', '\n']) {
            if let Some(path) = dirs.strip_prefix("programs: =") {
                return self.which(prog, Some(OsStr::new(path)));
            }
        }
        None
    }

}

impl Default for Build {
    fn default() -> Build {
        Build::new()
    }
}

fn fail(s: &str) -> ! {
    eprintln!("\n\nerror occurred in cc-rs: {s}\n\n");
    std::process::exit(1);
}

static NEW_STANDALONE_ANDROID_COMPILERS: [&str; 4] = [
    "aarch64-linux-android21-clang",
    "armv7a-linux-androideabi16-clang",
    "i686-linux-android16-clang",
    "x86_64-linux-android21-clang",
];

fn android_clang_compiler_uses_target_arg_internally(clang_path: &Path) -> bool {
    if let Some(filename) = clang_path.file_name() {
        if let Some(filename_str) = filename.to_str() {
            if let Some(idx) = filename_str.rfind('-') {
                return filename_str.split_at(idx).0.contains("android");
            }
        }
    }
    false
}

fn autodetect_android_compiler(raw_target: &str, gnu: &str, clang: &str) -> String {
    let new_clang_key = match raw_target {
        "aarch64-linux-android" => Some("aarch64"),
        "armv7-linux-androideabi" => Some("armv7a"),
        "i686-linux-android" => Some("i686"),
        "x86_64-linux-android" => Some("x86_64"),
        _ => None,
    };

    let new_clang = new_clang_key
        .map(|key| {
            NEW_STANDALONE_ANDROID_COMPILERS
                .iter()
                .find(|x| x.starts_with(key))
        })
        .unwrap_or(None);

    if let Some(new_clang) = new_clang {
        if Command::new(new_clang).output().is_ok() {
            return (*new_clang).into();
        }
    }

    let target = raw_target
        .replace("armv7neon", "arm")
        .replace("armv7", "arm")
        .replace("thumbv7neon", "arm")
        .replace("thumbv7", "arm");
    let gnu_compiler = format!("{target}-{gnu}");
    let clang_compiler = format!("{target}-{clang}");

    let clang_compiler_cmd = format!("{target}-{clang}.cmd");

    if Command::new(&gnu_compiler).output().is_ok() {
        gnu_compiler
    } else if cfg!(windows) && Command::new(&clang_compiler_cmd).output().is_ok() {
        clang_compiler_cmd
    } else {
        clang_compiler
    }
}

#[derive(Clone, Copy, PartialEq)]
enum AsmFileExt {
    /// `.asm` files. On MSVC targets, we assume these should be passed to MASM
    /// (`ml{,64}.exe`).
    DotAsm,
    /// `.s` or `.S` files, which do not have the special handling on MSVC targets.
    DotS,
}

impl AsmFileExt {
    fn from_path(file: &Path) -> Option<Self> {
        if let Some(ext) = file.extension() {
            if let Some(ext) = ext.to_str() {
                let ext = ext.to_lowercase();
                match &*ext {
                    "asm" => return Some(AsmFileExt::DotAsm),
                    "s" => return Some(AsmFileExt::DotS),
                    _ => return None,
                }
            }
        }
        None
    }
}

/// Returns true if `cc` has been disabled by `CC_FORCE_DISABLE`.
fn is_disabled() -> bool {
    static CACHE: AtomicU8 = AtomicU8::new(0);

    let val = CACHE.load(Relaxed);
    #[allow(clippy::disallowed_methods)]
    fn compute_is_disabled() -> bool {
        match std::env::var_os("CC_FORCE_DISABLE") {
            None => false,
            Some(v) => &*v != "0" && &*v != "false" && &*v != "no",
        }
    }
    match val {
        2 => true,
        1 => false,
        0 => {
            let truth = compute_is_disabled();
            let encoded_truth = if truth { 2u8 } else { 1 };
            CACHE.store(encoded_truth, Relaxed);
            truth
        }
        _ => unreachable!(),
    }
}

/// Automates the `if is_disabled() { return error }` check and ensures
/// we produce a consistent error message for it.
fn check_disabled() -> Result<(), Error> {
    if is_disabled() {
        return Err(Error::new(
            ErrorKind::Disabled,
            "the `cc` crate's functionality has been disabled by the `CC_FORCE_DISABLE` environment variable."
        ));
    }
    Ok(())
}
