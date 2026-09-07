/*!
This crate provides a cross platform abstraction for writing colored text to
a terminal. Colors are written using either ANSI escape sequences or by
communicating with a Windows console. Much of this API was motivated by use
inside command line applications, where colors or styles can be configured
by the end user and/or the environment.

This crate also provides platform independent support for writing colored text
to an in memory buffer. While this is easy to do with ANSI escape sequences
(because they are in the buffer themselves), it is trickier to do with the
Windows console API, which requires synchronous communication.

In ANSI mode, this crate also provides support for writing hyperlinks.

# Organization

The `WriteColor` trait extends the `io::Write` trait with methods for setting
colors or resetting them.

`StandardStream` and `StandardStreamLock` both satisfy `WriteColor` and are
analogous to `std::io::Stdout` and `std::io::StdoutLock`, or `std::io::Stderr`
and `std::io::StderrLock`.

`Buffer` is an in memory buffer that supports colored text. In a parallel
program, each thread might write to its own buffer. A buffer can be printed to
using a `BufferWriter`. The advantage of this design is that each thread can
work in parallel on a buffer without having to synchronize access to global
resources such as the Windows console. Moreover, this design also prevents
interleaving of buffer output.

`Ansi` and `NoColor` both satisfy `WriteColor` for arbitrary implementors of
`io::Write`. These types are useful when you know exactly what you need. An
analogous type for the Windows console is not provided since it cannot exist.

# Example: using `StandardStream`

The `StandardStream` type in this crate works similarly to `std::io::Stdout`,
except it is augmented with methods for coloring by the `WriteColor` trait.
For example, to write some green text:

```rust,no_run
# fn test() -> Result<(), Box<::std::error::Error>> {
use std::io::Write;
use termcolor::{Color, ColorChoice, ColorSpec, StandardStream, WriteColor};

let mut stdout = StandardStream::stdout(ColorChoice::Always);
stdout.set_color(ColorSpec::new().set_fg(Some(Color::Green)))?;
writeln!(&mut stdout, "green text!")?;
# Ok(()) }
```

Note that any text written to the terminal now will be colored
green when using ANSI escape sequences, even if it is written via
stderr, and even if stderr had previously been set to `Color::Red`.
Users will need to manage any color changes themselves by calling
[`WriteColor::set_color`](trait.WriteColor.html#tymethod.set_color), and this
may include calling [`WriteColor::reset`](trait.WriteColor.html#tymethod.reset)
before the program exits to a shell.

# Example: using `BufferWriter`

A `BufferWriter` can create buffers and write buffers to stdout or stderr. It
does *not* implement `io::Write` or `WriteColor` itself. Instead, `Buffer`
implements `io::Write` and `io::WriteColor`.

This example shows how to print some green text to stderr.

```rust,no_run
# fn test() -> Result<(), Box<::std::error::Error>> {
use std::io::Write;
use termcolor::{BufferWriter, Color, ColorChoice, ColorSpec, WriteColor};

let mut bufwtr = BufferWriter::stderr(ColorChoice::Always);
let mut buffer = bufwtr.buffer();
buffer.set_color(ColorSpec::new().set_fg(Some(Color::Green)))?;
writeln!(&mut buffer, "green text!")?;
bufwtr.print(&buffer)?;
# Ok(()) }
```

# Detecting presence of a terminal

In many scenarios when using color, one often wants to enable colors
automatically when writing to a terminal and disable colors automatically when
writing to anything else. The typical way to achieve this in Unix environments
is via libc's
[`isatty`](https://man7.org/linux/man-pages/man3/isatty.3.html)
function.
Unfortunately, this notoriously does not work well in Windows environments. To
work around that, the recommended solution is to use the standard library's
[`IsTerminal`](https://doc.rust-lang.org/std/io/trait.IsTerminal.html) trait.
It goes out of its way to get it as right as possible in Windows environments.

For example, in a command line application that exposes a `--color` flag,
your logic for how to enable colors might look like this:

```ignore
use std::io::IsTerminal;

use termcolor::{ColorChoice, StandardStream};

let preference = argv.get_flag("color").unwrap_or("auto");
let mut choice = preference.parse::<ColorChoice>()?;
if choice == ColorChoice::Auto && !std::io::stdin().is_terminal() {
    choice = ColorChoice::Never;
}
let stdout = StandardStream::stdout(choice);
// ... write to stdout
```

Currently, `termcolor` does not provide anything to do this for you.
*/

#![deny(missing_debug_implementations, missing_docs)]


use std::env;
use std::error;
use std::fmt;
use std::io::{self, Write};
use std::str::FromStr;
use std::sync::atomic::{AtomicBool, Ordering};


/// This trait describes the behavior of writers that support colored output.
pub trait WriteColor: io::Write {
    /// Returns true if and only if the underlying writer supports colors.
    fn supports_color(&self) -> bool;

    /// Set the color settings of the writer.
    ///
    /// Subsequent writes to this writer will use these settings until either
    /// `reset` is called or new color settings are set.
    ///
    /// If there was a problem setting the color settings, then an error is
    /// returned.
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()>;

    /// Reset the current color settings to their original settings.
    ///
    /// If there was a problem resetting the color settings, then an error is
    /// returned.
    ///
    /// Note that this does not reset hyperlinks. Those need to be
    /// reset on their own, e.g., by calling `set_hyperlink` with
    /// [`HyperlinkSpec::none`].
    fn reset(&mut self) -> io::Result<()>;

    /// Returns true if and only if the underlying writer must synchronously
    /// interact with an end user's device in order to control colors. By
    /// default, this always returns `false`.
    ///
    /// In practice, this should return `true` if the underlying writer is
    /// manipulating colors using the Windows console APIs.
    ///
    /// This is useful for writing generic code (such as a buffered writer)
    /// that can perform certain optimizations when the underlying writer
    /// doesn't rely on synchronous APIs. For example, ANSI escape sequences
    /// can be passed through to the end user's device as is.
    fn is_synchronous(&self) -> bool {
        false
    }

    /// Set the current hyperlink of the writer.
    ///
    /// The typical way to use this is to first call it with a
    /// [`HyperlinkSpec::open`] to write the actual URI to a tty that supports
    /// [OSC-8]. At this point, the caller can now write the label for the
    /// hyperlink. This may include coloring or other styles. Once the caller
    /// has finished writing the label, one should call this method again with
    /// [`HyperlinkSpec::close`].
    ///
    /// If there was a problem setting the hyperlink, then an error is
    /// returned.
    ///
    /// This defaults to doing nothing.
    ///
    /// [OSC8]: https://github.com/Alhadis/OSC8-Adoption/
    fn set_hyperlink(&mut self, _link: &HyperlinkSpec) -> io::Result<()> {
        Ok(())
    }

    /// Returns true if and only if the underlying writer supports hyperlinks.
    ///
    /// This can be used to avoid generating hyperlink URIs unnecessarily.
    ///
    /// This defaults to `false`.
    fn supports_hyperlinks(&self) -> bool {
        false
    }
}

impl<'a, T: ?Sized + WriteColor> WriteColor for &'a mut T {
    fn supports_color(&self) -> bool {
        (&**self).supports_color()
    }
    fn supports_hyperlinks(&self) -> bool {
        (&**self).supports_hyperlinks()
    }
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        (&mut **self).set_color(spec)
    }
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        (&mut **self).set_hyperlink(link)
    }
    fn reset(&mut self) -> io::Result<()> {
        (&mut **self).reset()
    }
    fn is_synchronous(&self) -> bool {
        (&**self).is_synchronous()
    }
}

impl<T: ?Sized + WriteColor> WriteColor for Box<T> {
    fn supports_color(&self) -> bool {
        (&**self).supports_color()
    }
    fn supports_hyperlinks(&self) -> bool {
        (&**self).supports_hyperlinks()
    }
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        (&mut **self).set_color(spec)
    }
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        (&mut **self).set_hyperlink(link)
    }
    fn reset(&mut self) -> io::Result<()> {
        (&mut **self).reset()
    }
    fn is_synchronous(&self) -> bool {
        (&**self).is_synchronous()
    }
}

/// ColorChoice represents the color preferences of an end user.
///
/// The `Default` implementation for this type will select `Auto`, which tries
/// to do the right thing based on the current environment.
///
/// The `FromStr` implementation for this type converts a lowercase kebab-case
/// string of the variant name to the corresponding variant. Any other string
/// results in an error.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ColorChoice {
    /// Try very hard to emit colors. This includes emitting ANSI colors
    /// on Windows if the console API is unavailable.
    Always,
    /// AlwaysAnsi is like Always, except it never tries to use anything other
    /// than emitting ANSI color codes.
    AlwaysAnsi,
    /// Try to use colors, but don't force the issue. If the console isn't
    /// available on Windows, or if TERM=dumb, or if `NO_COLOR` is defined, for
    /// example, then don't use colors.
    Auto,
    /// Never emit colors.
    Never,
}

/// The default is `Auto`.
impl Default for ColorChoice {
    fn default() -> ColorChoice {
        ColorChoice::Auto
    }
}

impl FromStr for ColorChoice {
    type Err = ColorChoiceParseError;

    fn from_str(s: &str) -> Result<ColorChoice, ColorChoiceParseError> {
        match s.to_lowercase().as_str() {
            "always" => Ok(ColorChoice::Always),
            "always-ansi" => Ok(ColorChoice::AlwaysAnsi),
            "never" => Ok(ColorChoice::Never),
            "auto" => Ok(ColorChoice::Auto),
            unknown => Err(ColorChoiceParseError {
                unknown_choice: unknown.to_string(),
            }),
        }
    }
}

impl ColorChoice {
    /// Returns true if we should attempt to write colored output.
    fn should_attempt_color(&self) -> bool {
        match *self {
            ColorChoice::Always => true,
            ColorChoice::AlwaysAnsi => true,
            ColorChoice::Never => false,
            ColorChoice::Auto => self.env_allows_color(),
        }
    }

fn env_allows_color(&self) -> bool {
        match env::var_os("TERM") {
            None => return false,
            Some(k) => {
                if k == "dumb" {
                    return false;
                }
            }
        }
        if env::var_os("NO_COLOR").is_some() {
            return false;
        }
        true
    }


}

/// An error that occurs when parsing a `ColorChoice` fails.
#[derive(Clone, Debug)]
pub struct ColorChoiceParseError {
    unknown_choice: String,
}

impl std::error::Error for ColorChoiceParseError {}

impl fmt::Display for ColorChoiceParseError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(
            f,
            "unrecognized color choice '{}': valid choices are: \
             always, always-ansi, never, auto",
            self.unknown_choice,
        )
    }
}

/// `std::io` implements `Stdout` and `Stderr` (and their `Lock` variants) as
/// separate types, which makes it difficult to abstract over them. We use
/// some simple internal enum types to work around this.

enum StandardStreamType {
    Stdout,
    Stderr,
    StdoutBuffered,
    StderrBuffered,
}

#[derive(Debug)]
enum IoStandardStream {
    Stdout(io::Stdout),
    Stderr(io::Stderr),
    StdoutBuffered(io::BufWriter<io::Stdout>),
    StderrBuffered(io::BufWriter<io::Stderr>),
}

impl IoStandardStream {
    fn new(sty: StandardStreamType) -> IoStandardStream {
        match sty {
            StandardStreamType::Stdout => {
                IoStandardStream::Stdout(io::stdout())
            }
            StandardStreamType::Stderr => {
                IoStandardStream::Stderr(io::stderr())
            }
            StandardStreamType::StdoutBuffered => {
                let wtr = io::BufWriter::new(io::stdout());
                IoStandardStream::StdoutBuffered(wtr)
            }
            StandardStreamType::StderrBuffered => {
                let wtr = io::BufWriter::new(io::stderr());
                IoStandardStream::StderrBuffered(wtr)
            }
        }
    }

    fn lock(&self) -> IoStandardStreamLock<'_> {
        match *self {
            IoStandardStream::Stdout(ref s) => {
                IoStandardStreamLock::StdoutLock(s.lock())
            }
            IoStandardStream::Stderr(ref s) => {
                IoStandardStreamLock::StderrLock(s.lock())
            }
            IoStandardStream::StdoutBuffered(_)
            | IoStandardStream::StderrBuffered(_) => {
                panic!("cannot lock a buffered standard stream")
            }
        }
    }
}

impl io::Write for IoStandardStream {
    #[inline(always)]
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        match *self {
            IoStandardStream::Stdout(ref mut s) => s.write(b),
            IoStandardStream::Stderr(ref mut s) => s.write(b),
            IoStandardStream::StdoutBuffered(ref mut s) => s.write(b),
            IoStandardStream::StderrBuffered(ref mut s) => s.write(b),
        }
    }

    #[inline(always)]
    fn flush(&mut self) -> io::Result<()> {
        match *self {
            IoStandardStream::Stdout(ref mut s) => s.flush(),
            IoStandardStream::Stderr(ref mut s) => s.flush(),
            IoStandardStream::StdoutBuffered(ref mut s) => s.flush(),
            IoStandardStream::StderrBuffered(ref mut s) => s.flush(),
        }
    }
}


#[derive(Debug)]
enum IoStandardStreamLock<'a> {
    StdoutLock(io::StdoutLock<'a>),
    StderrLock(io::StderrLock<'a>),
}

impl<'a> io::Write for IoStandardStreamLock<'a> {
    #[inline(always)]
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        match *self {
            IoStandardStreamLock::StdoutLock(ref mut s) => s.write(b),
            IoStandardStreamLock::StderrLock(ref mut s) => s.write(b),
        }
    }

    #[inline(always)]
    fn flush(&mut self) -> io::Result<()> {
        match *self {
            IoStandardStreamLock::StdoutLock(ref mut s) => s.flush(),
            IoStandardStreamLock::StderrLock(ref mut s) => s.flush(),
        }
    }
}

/// Satisfies `io::Write` and `WriteColor`, and supports optional coloring
/// to either of the standard output streams, stdout and stderr.
#[derive(Debug)]
pub struct StandardStream {
    wtr: LossyStandardStream<WriterInner<IoStandardStream>>,
}

/// `StandardStreamLock` is a locked reference to a `StandardStream`.
///
/// This implements the `io::Write` and `WriteColor` traits, and is constructed
/// via the `Write::lock` method.
///
/// The lifetime `'a` refers to the lifetime of the corresponding
/// `StandardStream`.
#[derive(Debug)]
pub struct StandardStreamLock<'a> {
    wtr: LossyStandardStream<WriterInnerLock<'a, IoStandardStreamLock<'a>>>,
}

/// Like `StandardStream`, but does buffered writing.
#[derive(Debug)]
pub struct BufferedStandardStream {
    wtr: LossyStandardStream<WriterInner<IoStandardStream>>,
}

/// WriterInner is a (limited) generic representation of a writer. It is
/// limited because W should only ever be stdout/stderr on Windows.
#[derive(Debug)]
enum WriterInner<W> {
    NoColor(NoColor<W>),
    Ansi(Ansi<W>),
#[cfg(any())]










    Windows {
        wtr: W,
        console: Mutex<wincon::Console>,
    },
}

/// WriterInnerLock is a (limited) generic representation of a writer. It is
/// limited because W should only ever be stdout/stderr on Windows.
#[derive(Debug)]
enum WriterInnerLock<'a, W> {
    NoColor(NoColor<W>),
    Ansi(Ansi<W>),
    /// What a gross hack. On Windows, we need to specify a lifetime for the
    /// console when in a locked state, but obviously don't need to do that
    /// on Unix, which makes the `'a` unused. To satisfy the compiler, we need
    /// a PhantomData.
    #[allow(dead_code)]
    Unreachable(::std::marker::PhantomData<&'a ()>),
#[cfg(any())]










    Windows {
        wtr: W,
        console: MutexGuard<'a, wincon::Console>,
    },
}

impl StandardStream {
    /// Create a new `StandardStream` with the given color preferences that
    /// writes to standard output.
    ///
    /// On Windows, if coloring is desired and a Windows console could not be
    /// found, then ANSI escape sequences are used instead.
    ///
    /// The specific color/style settings can be configured when writing via
    /// the `WriteColor` trait.
    pub fn stdout(choice: ColorChoice) -> StandardStream {
        let wtr = WriterInner::create(StandardStreamType::Stdout, choice);
        StandardStream { wtr: LossyStandardStream::new(wtr) }
    }

    /// Create a new `StandardStream` with the given color preferences that
    /// writes to standard error.
    ///
    /// On Windows, if coloring is desired and a Windows console could not be
    /// found, then ANSI escape sequences are used instead.
    ///
    /// The specific color/style settings can be configured when writing via
    /// the `WriteColor` trait.
    pub fn stderr(choice: ColorChoice) -> StandardStream {
        let wtr = WriterInner::create(StandardStreamType::Stderr, choice);
        StandardStream { wtr: LossyStandardStream::new(wtr) }
    }

    /// Lock the underlying writer.
    ///
    /// The lock guard returned also satisfies `io::Write` and
    /// `WriteColor`.
    ///
    /// This method is **not reentrant**. It may panic if `lock` is called
    /// while a `StandardStreamLock` is still alive.
    pub fn lock(&self) -> StandardStreamLock<'_> {
        StandardStreamLock::from_stream(self)
    }
}

impl<'a> StandardStreamLock<'a> {
fn from_stream(stream: &StandardStream) -> StandardStreamLock<'_> {
        let locked = match *stream.wtr.get_ref() {
            WriterInner::NoColor(ref w) => {
                WriterInnerLock::NoColor(NoColor(w.0.lock()))
            }
            WriterInner::Ansi(ref w) => {
                WriterInnerLock::Ansi(Ansi(w.0.lock()))
            }
        };
        StandardStreamLock { wtr: stream.wtr.wrap(locked) }
    }

}

impl BufferedStandardStream {
    /// Create a new `BufferedStandardStream` with the given color preferences
    /// that writes to standard output via a buffered writer.
    ///
    /// On Windows, if coloring is desired and a Windows console could not be
    /// found, then ANSI escape sequences are used instead.
    ///
    /// The specific color/style settings can be configured when writing via
    /// the `WriteColor` trait.
    pub fn stdout(choice: ColorChoice) -> BufferedStandardStream {
        let wtr =
            WriterInner::create(StandardStreamType::StdoutBuffered, choice);
        BufferedStandardStream { wtr: LossyStandardStream::new(wtr) }
    }

    /// Create a new `BufferedStandardStream` with the given color preferences
    /// that writes to standard error via a buffered writer.
    ///
    /// On Windows, if coloring is desired and a Windows console could not be
    /// found, then ANSI escape sequences are used instead.
    ///
    /// The specific color/style settings can be configured when writing via
    /// the `WriteColor` trait.
    pub fn stderr(choice: ColorChoice) -> BufferedStandardStream {
        let wtr =
            WriterInner::create(StandardStreamType::StderrBuffered, choice);
        BufferedStandardStream { wtr: LossyStandardStream::new(wtr) }
    }
}

impl WriterInner<IoStandardStream> {
    /// Create a new inner writer for a standard stream with the given color
    /// preferences.
fn create(
        sty: StandardStreamType,
        choice: ColorChoice,
    ) -> WriterInner<IoStandardStream> {
        if choice.should_attempt_color() {
            WriterInner::Ansi(Ansi(IoStandardStream::new(sty)))
        } else {
            WriterInner::NoColor(NoColor(IoStandardStream::new(sty)))
        }
    }

}

impl io::Write for StandardStream {
    #[inline]
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        self.wtr.write(b)
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        self.wtr.flush()
    }
}

impl WriteColor for StandardStream {
    #[inline]
    fn supports_color(&self) -> bool {
        self.wtr.supports_color()
    }

    #[inline]
    fn supports_hyperlinks(&self) -> bool {
        self.wtr.supports_hyperlinks()
    }

    #[inline]
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        self.wtr.set_color(spec)
    }

    #[inline]
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        self.wtr.set_hyperlink(link)
    }

    #[inline]
    fn reset(&mut self) -> io::Result<()> {
        self.wtr.reset()
    }

    #[inline]
    fn is_synchronous(&self) -> bool {
        self.wtr.is_synchronous()
    }
}

impl<'a> io::Write for StandardStreamLock<'a> {
    #[inline]
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        self.wtr.write(b)
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        self.wtr.flush()
    }
}

impl<'a> WriteColor for StandardStreamLock<'a> {
    #[inline]
    fn supports_color(&self) -> bool {
        self.wtr.supports_color()
    }

    #[inline]
    fn supports_hyperlinks(&self) -> bool {
        self.wtr.supports_hyperlinks()
    }

    #[inline]
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        self.wtr.set_color(spec)
    }

    #[inline]
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        self.wtr.set_hyperlink(link)
    }

    #[inline]
    fn reset(&mut self) -> io::Result<()> {
        self.wtr.reset()
    }

    #[inline]
    fn is_synchronous(&self) -> bool {
        self.wtr.is_synchronous()
    }
}

impl io::Write for BufferedStandardStream {
    #[inline]
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        self.wtr.write(b)
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        self.wtr.flush()
    }
}

impl WriteColor for BufferedStandardStream {
    #[inline]
    fn supports_color(&self) -> bool {
        self.wtr.supports_color()
    }

    #[inline]
    fn supports_hyperlinks(&self) -> bool {
        self.wtr.supports_hyperlinks()
    }

    #[inline]
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        if self.is_synchronous() {
            self.wtr.flush()?;
        }
        self.wtr.set_color(spec)
    }

    #[inline]
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        if self.is_synchronous() {
            self.wtr.flush()?;
        }
        self.wtr.set_hyperlink(link)
    }

    #[inline]
    fn reset(&mut self) -> io::Result<()> {
        self.wtr.reset()
    }

    #[inline]
    fn is_synchronous(&self) -> bool {
        self.wtr.is_synchronous()
    }
}

impl<W: io::Write> io::Write for WriterInner<W> {
    #[inline(always)]
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match *self {
            WriterInner::NoColor(ref mut wtr) => wtr.write(buf),
            WriterInner::Ansi(ref mut wtr) => wtr.write(buf),
#[cfg(any())]










            WriterInner::Windows { ref mut wtr, .. } => wtr.write(buf),
        }
    }

    #[inline(always)]
    fn flush(&mut self) -> io::Result<()> {
        match *self {
            WriterInner::NoColor(ref mut wtr) => wtr.flush(),
            WriterInner::Ansi(ref mut wtr) => wtr.flush(),
#[cfg(any())]










            WriterInner::Windows { ref mut wtr, .. } => wtr.flush(),
        }
    }
}

impl<W: io::Write> WriteColor for WriterInner<W> {
    fn supports_color(&self) -> bool {
        match *self {
            WriterInner::NoColor(_) => false,
            WriterInner::Ansi(_) => true,
#[cfg(any())]










            WriterInner::Windows { .. } => true,
        }
    }

    fn supports_hyperlinks(&self) -> bool {
        match *self {
            WriterInner::NoColor(_) => false,
            WriterInner::Ansi(_) => true,
#[cfg(any())]










            WriterInner::Windows { .. } => false,
        }
    }

    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        match *self {
            WriterInner::NoColor(ref mut wtr) => wtr.set_color(spec),
            WriterInner::Ansi(ref mut wtr) => wtr.set_color(spec),
#[cfg(any())]










            WriterInner::Windows { ref mut wtr, ref console } => {
                wtr.flush()?;
                let mut console = console.lock().unwrap();
                spec.write_console(&mut *console)
            }
        }
    }

    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        match *self {
            WriterInner::NoColor(ref mut wtr) => wtr.set_hyperlink(link),
            WriterInner::Ansi(ref mut wtr) => wtr.set_hyperlink(link),
#[cfg(any())]










            WriterInner::Windows { .. } => Ok(()),
        }
    }

    fn reset(&mut self) -> io::Result<()> {
        match *self {
            WriterInner::NoColor(ref mut wtr) => wtr.reset(),
            WriterInner::Ansi(ref mut wtr) => wtr.reset(),
#[cfg(any())]










            WriterInner::Windows { ref mut wtr, ref mut console } => {
                wtr.flush()?;
                console.lock().unwrap().reset()?;
                Ok(())
            }
        }
    }

    fn is_synchronous(&self) -> bool {
        match *self {
            WriterInner::NoColor(_) => false,
            WriterInner::Ansi(_) => false,
#[cfg(any())]










            WriterInner::Windows { .. } => true,
        }
    }
}

impl<'a, W: io::Write> io::Write for WriterInnerLock<'a, W> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(ref mut wtr) => wtr.write(buf),
            WriterInnerLock::Ansi(ref mut wtr) => wtr.write(buf),
#[cfg(any())]










            WriterInnerLock::Windows { ref mut wtr, .. } => wtr.write(buf),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(ref mut wtr) => wtr.flush(),
            WriterInnerLock::Ansi(ref mut wtr) => wtr.flush(),
#[cfg(any())]










            WriterInnerLock::Windows { ref mut wtr, .. } => wtr.flush(),
        }
    }
}

impl<'a, W: io::Write> WriteColor for WriterInnerLock<'a, W> {
    fn supports_color(&self) -> bool {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(_) => false,
            WriterInnerLock::Ansi(_) => true,
#[cfg(any())]










            WriterInnerLock::Windows { .. } => true,
        }
    }

    fn supports_hyperlinks(&self) -> bool {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(_) => false,
            WriterInnerLock::Ansi(_) => true,
#[cfg(any())]










            WriterInnerLock::Windows { .. } => false,
        }
    }

    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(ref mut wtr) => wtr.set_color(spec),
            WriterInnerLock::Ansi(ref mut wtr) => wtr.set_color(spec),
#[cfg(any())]










            WriterInnerLock::Windows { ref mut wtr, ref mut console } => {
                wtr.flush()?;
                spec.write_console(console)
            }
        }
    }

    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(ref mut wtr) => wtr.set_hyperlink(link),
            WriterInnerLock::Ansi(ref mut wtr) => wtr.set_hyperlink(link),
#[cfg(any())]










            WriterInnerLock::Windows { .. } => Ok(()),
        }
    }

    fn reset(&mut self) -> io::Result<()> {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(ref mut wtr) => wtr.reset(),
            WriterInnerLock::Ansi(ref mut wtr) => wtr.reset(),
#[cfg(any())]










            WriterInnerLock::Windows { ref mut wtr, ref mut console } => {
                wtr.flush()?;
                console.reset()?;
                Ok(())
            }
        }
    }

    fn is_synchronous(&self) -> bool {
        match *self {
            WriterInnerLock::Unreachable(_) => unreachable!(),
            WriterInnerLock::NoColor(_) => false,
            WriterInnerLock::Ansi(_) => false,
#[cfg(any())]










            WriterInnerLock::Windows { .. } => true,
        }
    }
}

/// Writes colored buffers to stdout or stderr.
///
/// Writable buffers can be obtained by calling `buffer` on a `BufferWriter`.
///
/// This writer works with terminals that support ANSI escape sequences or
/// with a Windows console.
///
/// It is intended for a `BufferWriter` to be put in an `Arc` and written to
/// from multiple threads simultaneously.
#[derive(Debug)]
pub struct BufferWriter {
    stream: LossyStandardStream<IoStandardStream>,
    printed: AtomicBool,
    separator: Option<Vec<u8>>,
    color_choice: ColorChoice,
#[cfg(any())]










    console: Option<Mutex<wincon::Console>>,
}

impl BufferWriter {
    /// Create a new `BufferWriter` that writes to a standard stream with the
    /// given color preferences.
    ///
    /// The specific color/style settings can be configured when writing to
    /// the buffers themselves.
fn create(sty: StandardStreamType, choice: ColorChoice) -> BufferWriter {
        BufferWriter {
            stream: LossyStandardStream::new(IoStandardStream::new(sty)),
            printed: AtomicBool::new(false),
            separator: None,
            color_choice: choice,
        }
    }

    /// Create a new `BufferWriter` that writes to a standard stream with the
    /// given color preferences.
    ///
    /// If coloring is desired and a Windows console could not be found, then
    /// ANSI escape sequences are used instead.
    ///
    /// The specific color/style settings can be configured when writing to
    /// the buffers themselves.

    /// Create a new `BufferWriter` that writes to stdout with the given
    /// color preferences.
    ///
    /// On Windows, if coloring is desired and a Windows console could not be
    /// found, then ANSI escape sequences are used instead.
    ///
    /// The specific color/style settings can be configured when writing to
    /// the buffers themselves.
    pub fn stdout(choice: ColorChoice) -> BufferWriter {
        BufferWriter::create(StandardStreamType::Stdout, choice)
    }

    /// Create a new `BufferWriter` that writes to stderr with the given
    /// color preferences.
    ///
    /// On Windows, if coloring is desired and a Windows console could not be
    /// found, then ANSI escape sequences are used instead.
    ///
    /// The specific color/style settings can be configured when writing to
    /// the buffers themselves.
    pub fn stderr(choice: ColorChoice) -> BufferWriter {
        BufferWriter::create(StandardStreamType::Stderr, choice)
    }

    /// If set, the separator given is printed between buffers. By default, no
    /// separator is printed.
    ///
    /// The default value is `None`.
    pub fn separator(&mut self, sep: Option<Vec<u8>>) {
        self.separator = sep;
    }

    /// Creates a new `Buffer` with the current color preferences.
    ///
    /// A `Buffer` satisfies both `io::Write` and `WriteColor`. A `Buffer` can
    /// be printed using the `print` method.
pub fn buffer(&self) -> Buffer {
        Buffer::new(self.color_choice)
    }

    /// Creates a new `Buffer` with the current color preferences.
    ///
    /// A `Buffer` satisfies both `io::Write` and `WriteColor`. A `Buffer` can
    /// be printed using the `print` method.

    /// Prints the contents of the given buffer.
    ///
    /// It is safe to call this from multiple threads simultaneously. In
    /// particular, all buffers are written atomically. No interleaving will
    /// occur.
    pub fn print(&self, buf: &Buffer) -> io::Result<()> {
        if buf.is_empty() {
            return Ok(());
        }
        let mut stream = self.stream.wrap(self.stream.get_ref().lock());
        if let Some(ref sep) = self.separator {
            if self.printed.load(Ordering::Relaxed) {
                stream.write_all(sep)?;
                stream.write_all(b"\n")?;
            }
        }
        match buf.0 {
            BufferInner::NoColor(ref b) => stream.write_all(&b.0)?,
            BufferInner::Ansi(ref b) => stream.write_all(&b.0)?,
#[cfg(any())]










            BufferInner::Windows(ref b) => {
                let console_mutex = self
                    .console
                    .as_ref()
                    .expect("got Windows buffer but have no Console");
                let mut console = console_mutex.lock().unwrap();
                b.print(&mut *console, &mut stream)?;
            }
        }
        self.printed.store(true, Ordering::Relaxed);
        Ok(())
    }
}

/// Write colored text to memory.
///
/// `Buffer` is a platform independent abstraction for printing colored text to
/// an in memory buffer. When the buffer is printed using a `BufferWriter`, the
/// color information will be applied to the output device (a tty on Unix and a
/// console on Windows).
///
/// A `Buffer` is typically created by calling the `BufferWriter.buffer`
/// method, which will take color preferences and the environment into
/// account. However, buffers can also be manually created using `no_color`,
/// `ansi` or `console` (on Windows).
#[derive(Clone, Debug)]
pub struct Buffer(BufferInner);

/// BufferInner is an enumeration of different buffer types.
#[derive(Clone, Debug)]
enum BufferInner {
    /// No coloring information should be applied. This ignores all coloring
    /// directives.
    NoColor(NoColor<Vec<u8>>),
    /// Apply coloring using ANSI escape sequences embedded into the buffer.
    Ansi(Ansi<Vec<u8>>),
    /// Apply coloring using the Windows console APIs. This buffer saves
    /// color information in memory and only interacts with the console when
    /// the buffer is printed.
#[cfg(any())]










    Windows(WindowsBuffer),
}

impl Buffer {
    /// Create a new buffer with the given color settings.
fn new(choice: ColorChoice) -> Buffer {
        if choice.should_attempt_color() {
            Buffer::ansi()
        } else {
            Buffer::no_color()
        }
    }

    /// Create a new buffer with the given color settings.
    ///
    /// On Windows, one can elect to create a buffer capable of being written
    /// to a console. Only enable it if a console is available.
    ///
    /// If coloring is desired and `console` is false, then ANSI escape
    /// sequences are used instead.

    /// Create a buffer that drops all color information.
    pub fn no_color() -> Buffer {
        Buffer(BufferInner::NoColor(NoColor(vec![])))
    }

    /// Create a buffer that uses ANSI escape sequences.
    pub fn ansi() -> Buffer {
        Buffer(BufferInner::Ansi(Ansi(vec![])))
    }

    /// Create a buffer that can be written to a Windows console.

    /// Returns true if and only if this buffer is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Returns the length of this buffer in bytes.
    pub fn len(&self) -> usize {
        match self.0 {
            BufferInner::NoColor(ref b) => b.0.len(),
            BufferInner::Ansi(ref b) => b.0.len(),
#[cfg(any())]










            BufferInner::Windows(ref b) => b.buf.len(),
        }
    }

    /// Clears this buffer.
    pub fn clear(&mut self) {
        match self.0 {
            BufferInner::NoColor(ref mut b) => b.0.clear(),
            BufferInner::Ansi(ref mut b) => b.0.clear(),
#[cfg(any())]










            BufferInner::Windows(ref mut b) => b.clear(),
        }
    }

    /// Consume this buffer and return the underlying raw data.
    ///
    /// On Windows, this unrecoverably drops all color information associated
    /// with the buffer.
    pub fn into_inner(self) -> Vec<u8> {
        match self.0 {
            BufferInner::NoColor(b) => b.0,
            BufferInner::Ansi(b) => b.0,
#[cfg(any())]










            BufferInner::Windows(b) => b.buf,
        }
    }

    /// Return the underlying data of the buffer.
    pub fn as_slice(&self) -> &[u8] {
        match self.0 {
            BufferInner::NoColor(ref b) => &b.0,
            BufferInner::Ansi(ref b) => &b.0,
#[cfg(any())]










            BufferInner::Windows(ref b) => &b.buf,
        }
    }

    /// Return the underlying data of the buffer as a mutable slice.
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        match self.0 {
            BufferInner::NoColor(ref mut b) => &mut b.0,
            BufferInner::Ansi(ref mut b) => &mut b.0,
#[cfg(any())]










            BufferInner::Windows(ref mut b) => &mut b.buf,
        }
    }
}

impl io::Write for Buffer {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self.0 {
            BufferInner::NoColor(ref mut w) => w.write(buf),
            BufferInner::Ansi(ref mut w) => w.write(buf),
#[cfg(any())]










            BufferInner::Windows(ref mut w) => w.write(buf),
        }
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        match self.0 {
            BufferInner::NoColor(ref mut w) => w.flush(),
            BufferInner::Ansi(ref mut w) => w.flush(),
#[cfg(any())]










            BufferInner::Windows(ref mut w) => w.flush(),
        }
    }
}

impl WriteColor for Buffer {
    #[inline]
    fn supports_color(&self) -> bool {
        match self.0 {
            BufferInner::NoColor(_) => false,
            BufferInner::Ansi(_) => true,
#[cfg(any())]










            BufferInner::Windows(_) => true,
        }
    }

    #[inline]
    fn supports_hyperlinks(&self) -> bool {
        match self.0 {
            BufferInner::NoColor(_) => false,
            BufferInner::Ansi(_) => true,
#[cfg(any())]










            BufferInner::Windows(_) => false,
        }
    }

    #[inline]
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        match self.0 {
            BufferInner::NoColor(ref mut w) => w.set_color(spec),
            BufferInner::Ansi(ref mut w) => w.set_color(spec),
#[cfg(any())]










            BufferInner::Windows(ref mut w) => w.set_color(spec),
        }
    }

    #[inline]
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        match self.0 {
            BufferInner::NoColor(ref mut w) => w.set_hyperlink(link),
            BufferInner::Ansi(ref mut w) => w.set_hyperlink(link),
#[cfg(any())]










            BufferInner::Windows(ref mut w) => w.set_hyperlink(link),
        }
    }

    #[inline]
    fn reset(&mut self) -> io::Result<()> {
        match self.0 {
            BufferInner::NoColor(ref mut w) => w.reset(),
            BufferInner::Ansi(ref mut w) => w.reset(),
#[cfg(any())]










            BufferInner::Windows(ref mut w) => w.reset(),
        }
    }

    #[inline]
    fn is_synchronous(&self) -> bool {
        false
    }
}

/// Satisfies `WriteColor` but ignores all color options.
#[derive(Clone, Debug)]
pub struct NoColor<W>(W);

impl<W: Write> NoColor<W> {
    /// Create a new writer that satisfies `WriteColor` but drops all color
    /// information.
    pub fn new(wtr: W) -> NoColor<W> {
        NoColor(wtr)
    }

    /// Consume this `NoColor` value and return the inner writer.
    pub fn into_inner(self) -> W {
        self.0
    }

    /// Return a reference to the inner writer.
    pub fn get_ref(&self) -> &W {
        &self.0
    }

    /// Return a mutable reference to the inner writer.
    pub fn get_mut(&mut self) -> &mut W {
        &mut self.0
    }
}

impl<W: io::Write> io::Write for NoColor<W> {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.0.write(buf)
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        self.0.flush()
    }
}

impl<W: io::Write> WriteColor for NoColor<W> {
    #[inline]
    fn supports_color(&self) -> bool {
        false
    }

    #[inline]
    fn supports_hyperlinks(&self) -> bool {
        false
    }

    #[inline]
    fn set_color(&mut self, _: &ColorSpec) -> io::Result<()> {
        Ok(())
    }

    #[inline]
    fn set_hyperlink(&mut self, _: &HyperlinkSpec) -> io::Result<()> {
        Ok(())
    }

    #[inline]
    fn reset(&mut self) -> io::Result<()> {
        Ok(())
    }

    #[inline]
    fn is_synchronous(&self) -> bool {
        false
    }
}

/// Satisfies `WriteColor` using standard ANSI escape sequences.
#[derive(Clone, Debug)]
pub struct Ansi<W>(W);

impl<W: Write> Ansi<W> {
    /// Create a new writer that satisfies `WriteColor` using standard ANSI
    /// escape sequences.
    pub fn new(wtr: W) -> Ansi<W> {
        Ansi(wtr)
    }

    /// Consume this `Ansi` value and return the inner writer.
    pub fn into_inner(self) -> W {
        self.0
    }

    /// Return a reference to the inner writer.
    pub fn get_ref(&self) -> &W {
        &self.0
    }

    /// Return a mutable reference to the inner writer.
    pub fn get_mut(&mut self) -> &mut W {
        &mut self.0
    }
}

impl<W: io::Write> io::Write for Ansi<W> {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.0.write(buf)
    }

    #[inline]
    fn write_all(&mut self, buf: &[u8]) -> io::Result<()> {
        self.0.write_all(buf)
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        self.0.flush()
    }
}

impl<W: io::Write> WriteColor for Ansi<W> {
    #[inline]
    fn supports_color(&self) -> bool {
        true
    }

    #[inline]
    fn supports_hyperlinks(&self) -> bool {
        true
    }

    #[inline]
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        if spec.reset {
            self.reset()?;
        }
        if spec.bold {
            self.write_str("\x1B[1m")?;
        }
        if spec.dimmed {
            self.write_str("\x1B[2m")?;
        }
        if spec.italic {
            self.write_str("\x1B[3m")?;
        }
        if spec.underline {
            self.write_str("\x1B[4m")?;
        }
        if spec.strikethrough {
            self.write_str("\x1B[9m")?;
        }
        if let Some(ref c) = spec.fg_color {
            self.write_color(true, c, spec.intense)?;
        }
        if let Some(ref c) = spec.bg_color {
            self.write_color(false, c, spec.intense)?;
        }
        Ok(())
    }

    #[inline]
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        self.write_str("\x1B]8;;")?;
        if let Some(uri) = link.uri() {
            self.write_all(uri)?;
        }
        self.write_str("\x1B\\")
    }

    #[inline]
    fn reset(&mut self) -> io::Result<()> {
        self.write_str("\x1B[0m")
    }

    #[inline]
    fn is_synchronous(&self) -> bool {
        false
    }
}

impl<W: io::Write> Ansi<W> {
    fn write_str(&mut self, s: &str) -> io::Result<()> {
        self.write_all(s.as_bytes())
    }

    fn write_color(
        &mut self,
        fg: bool,
        c: &Color,
        intense: bool,
    ) -> io::Result<()> {
        macro_rules! write_intense {
            ($clr:expr) => {
                if fg {
                    self.write_str(concat!("\x1B[38;5;", $clr, "m"))
                } else {
                    self.write_str(concat!("\x1B[48;5;", $clr, "m"))
                }
            };
        }
        macro_rules! write_normal {
            ($clr:expr) => {
                if fg {
                    self.write_str(concat!("\x1B[3", $clr, "m"))
                } else {
                    self.write_str(concat!("\x1B[4", $clr, "m"))
                }
            };
        }
        macro_rules! write_var_ansi_code {
            ($pre:expr, $($code:expr),+) => {{
                let pre_len = $pre.len();
                assert!(pre_len <= 7);
                let mut fmt = [0u8; 19];
                fmt[..pre_len].copy_from_slice($pre);
                let mut i = pre_len - 1;
                $(
                    let c1: u8 = ($code / 100) % 10;
                    let c2: u8 = ($code / 10) % 10;
                    let c3: u8 = $code % 10;
                    let mut printed = false;

                    if c1 != 0 {
                        printed = true;
                        i += 1;
                        fmt[i] = b'0' + c1;
                    }
                    if c2 != 0 || printed {
                        i += 1;
                        fmt[i] = b'0' + c2;
                    }
                    i += 1;
                    fmt[i] = b'0' + c3;
                    i += 1;
                    fmt[i] = b';';
                )+

                fmt[i] = b'm';
                self.write_all(&fmt[0..i+1])
            }}
        }
        macro_rules! write_custom {
            ($ansi256:expr) => {
                if fg {
                    write_var_ansi_code!(b"\x1B[38;5;", $ansi256)
                } else {
                    write_var_ansi_code!(b"\x1B[48;5;", $ansi256)
                }
            };

            ($r:expr, $g:expr, $b:expr) => {{
                if fg {
                    write_var_ansi_code!(b"\x1B[38;2;", $r, $g, $b)
                } else {
                    write_var_ansi_code!(b"\x1B[48;2;", $r, $g, $b)
                }
            }};
        }
        if intense {
            match *c {
                Color::Black => write_intense!("8"),
                Color::Blue => write_intense!("12"),
                Color::Green => write_intense!("10"),
                Color::Red => write_intense!("9"),
                Color::Cyan => write_intense!("14"),
                Color::Magenta => write_intense!("13"),
                Color::Yellow => write_intense!("11"),
                Color::White => write_intense!("15"),
                Color::Ansi256(c) => write_custom!(c),
                Color::Rgb(r, g, b) => write_custom!(r, g, b),
                Color::__Nonexhaustive => unreachable!(),
            }
        } else {
            match *c {
                Color::Black => write_normal!("0"),
                Color::Blue => write_normal!("4"),
                Color::Green => write_normal!("2"),
                Color::Red => write_normal!("1"),
                Color::Cyan => write_normal!("6"),
                Color::Magenta => write_normal!("5"),
                Color::Yellow => write_normal!("3"),
                Color::White => write_normal!("7"),
                Color::Ansi256(c) => write_custom!(c),
                Color::Rgb(r, g, b) => write_custom!(r, g, b),
                Color::__Nonexhaustive => unreachable!(),
            }
        }
    }
}

impl WriteColor for io::Sink {
    fn supports_color(&self) -> bool {
        false
    }

    fn supports_hyperlinks(&self) -> bool {
        false
    }

    fn set_color(&mut self, _: &ColorSpec) -> io::Result<()> {
        Ok(())
    }

    fn set_hyperlink(&mut self, _: &HyperlinkSpec) -> io::Result<()> {
        Ok(())
    }

    fn reset(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// An in-memory buffer that provides Windows console coloring.
///
/// This doesn't actually communicate with the Windows console. Instead, it
/// acts like a normal buffer but also saves the color information associated
/// with positions in the buffer. It is only when the buffer is written to the
/// console that coloring is actually applied.
///
/// This is roughly isomorphic to the ANSI based approach (i.e.,
/// `Ansi<Vec<u8>>`), except with ANSI, the color information is embedded
/// directly into the buffer.
///
/// Note that there is no way to write something generic like
/// `WindowsConsole<W: io::Write>` since coloring on Windows is tied
/// specifically to the console APIs, and therefore can't work on arbitrary
/// writers.




/// A color specification.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ColorSpec {
    fg_color: Option<Color>,
    bg_color: Option<Color>,
    bold: bool,
    intense: bool,
    underline: bool,
    dimmed: bool,
    italic: bool,
    reset: bool,
    strikethrough: bool,
}

impl Default for ColorSpec {
    fn default() -> ColorSpec {
        ColorSpec {
            fg_color: None,
            bg_color: None,
            bold: false,
            intense: false,
            underline: false,
            dimmed: false,
            italic: false,
            reset: true,
            strikethrough: false,
        }
    }
}

impl ColorSpec {
    /// Create a new color specification that has no colors or styles.
    pub fn new() -> ColorSpec {
        ColorSpec::default()
    }

    /// Get the foreground color.
    pub fn fg(&self) -> Option<&Color> {
        self.fg_color.as_ref()
    }

    /// Set the foreground color.
    pub fn set_fg(&mut self, color: Option<Color>) -> &mut ColorSpec {
        self.fg_color = color;
        self
    }

    /// Get the background color.
    pub fn bg(&self) -> Option<&Color> {
        self.bg_color.as_ref()
    }

    /// Set the background color.
    pub fn set_bg(&mut self, color: Option<Color>) -> &mut ColorSpec {
        self.bg_color = color;
        self
    }

    /// Get whether this is bold or not.
    ///
    /// Note that the bold setting has no effect in a Windows console.
    pub fn bold(&self) -> bool {
        self.bold
    }

    /// Set whether the text is bolded or not.
    ///
    /// Note that the bold setting has no effect in a Windows console.
    pub fn set_bold(&mut self, yes: bool) -> &mut ColorSpec {
        self.bold = yes;
        self
    }

    /// Get whether this is dimmed or not.
    ///
    /// Note that the dimmed setting has no effect in a Windows console.
    pub fn dimmed(&self) -> bool {
        self.dimmed
    }

    /// Set whether the text is dimmed or not.
    ///
    /// Note that the dimmed setting has no effect in a Windows console.
    pub fn set_dimmed(&mut self, yes: bool) -> &mut ColorSpec {
        self.dimmed = yes;
        self
    }

    /// Get whether this is italic or not.
    ///
    /// Note that the italic setting has no effect in a Windows console.
    pub fn italic(&self) -> bool {
        self.italic
    }

    /// Set whether the text is italicized or not.
    ///
    /// Note that the italic setting has no effect in a Windows console.
    pub fn set_italic(&mut self, yes: bool) -> &mut ColorSpec {
        self.italic = yes;
        self
    }

    /// Get whether this is underline or not.
    ///
    /// Note that the underline setting has no effect in a Windows console.
    pub fn underline(&self) -> bool {
        self.underline
    }

    /// Set whether the text is underlined or not.
    ///
    /// Note that the underline setting has no effect in a Windows console.
    pub fn set_underline(&mut self, yes: bool) -> &mut ColorSpec {
        self.underline = yes;
        self
    }

    /// Get whether this is strikethrough or not.
    ///
    /// Note that the strikethrough setting has no effect in a Windows console.
    pub fn strikethrough(&self) -> bool {
        self.strikethrough
    }

    /// Set whether the text is strikethrough or not.
    ///
    /// Note that the strikethrough setting has no effect in a Windows console.
    pub fn set_strikethrough(&mut self, yes: bool) -> &mut ColorSpec {
        self.strikethrough = yes;
        self
    }

    /// Get whether reset is enabled or not.
    ///
    /// reset is enabled by default. When disabled and using ANSI escape
    /// sequences, a "reset" code will be emitted every time a `ColorSpec`'s
    /// settings are applied.
    ///
    /// Note that the reset setting has no effect in a Windows console.
    pub fn reset(&self) -> bool {
        self.reset
    }

    /// Set whether to reset the terminal whenever color settings are applied.
    ///
    /// reset is enabled by default. When disabled and using ANSI escape
    /// sequences, a "reset" code will be emitted every time a `ColorSpec`'s
    /// settings are applied.
    ///
    /// Typically this is useful if callers have a requirement to more
    /// scrupulously manage the exact sequence of escape codes that are emitted
    /// when using ANSI for colors.
    ///
    /// Note that the reset setting has no effect in a Windows console.
    pub fn set_reset(&mut self, yes: bool) -> &mut ColorSpec {
        self.reset = yes;
        self
    }

    /// Get whether this is intense or not.
    ///
    /// On Unix-like systems, this will output the ANSI escape sequence
    /// that will print a high-intensity version of the color
    /// specified.
    ///
    /// On Windows systems, this will output the ANSI escape sequence
    /// that will print a brighter version of the color specified.
    pub fn intense(&self) -> bool {
        self.intense
    }

    /// Set whether the text is intense or not.
    ///
    /// On Unix-like systems, this will output the ANSI escape sequence
    /// that will print a high-intensity version of the color
    /// specified.
    ///
    /// On Windows systems, this will output the ANSI escape sequence
    /// that will print a brighter version of the color specified.
    pub fn set_intense(&mut self, yes: bool) -> &mut ColorSpec {
        self.intense = yes;
        self
    }

    /// Returns true if this color specification has no colors or styles.
    pub fn is_none(&self) -> bool {
        self.fg_color.is_none()
            && self.bg_color.is_none()
            && !self.bold
            && !self.underline
            && !self.dimmed
            && !self.italic
            && !self.intense
            && !self.strikethrough
    }

    /// Clears this color specification so that it has no color/style settings.
    pub fn clear(&mut self) {
        self.fg_color = None;
        self.bg_color = None;
        self.bold = false;
        self.underline = false;
        self.intense = false;
        self.dimmed = false;
        self.italic = false;
        self.strikethrough = false;
    }

}

/// The set of available colors for the terminal foreground/background.
///
/// The `Ansi256` and `Rgb` colors will only output the correct codes when
/// paired with the `Ansi` `WriteColor` implementation.
///
/// The `Ansi256` and `Rgb` color types are not supported when writing colors
/// on Windows using the console. If they are used on Windows, then they are
/// silently ignored and no colors will be emitted.
///
/// This set may expand over time.
///
/// This type has a `FromStr` impl that can parse colors from their human
/// readable form. The format is as follows:
///
/// 1. Any of the explicitly listed colors in English. They are matched
///    case insensitively.
/// 2. A single 8-bit integer, in either decimal or hexadecimal format.
/// 3. A triple of 8-bit integers separated by a comma, where each integer is
///    in decimal or hexadecimal format.
///
/// Hexadecimal numbers are written with a `0x` prefix.
#[allow(missing_docs)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Color {
    Black,
    Blue,
    Green,
    Red,
    Cyan,
    Magenta,
    Yellow,
    White,
    Ansi256(u8),
    Rgb(u8, u8, u8),
    #[doc(hidden)]
    __Nonexhaustive,
}

impl Color {
    /// Translate this color to a wincon::Color.

    /// Parses a numeric color string, either ANSI or RGB.
    fn from_str_numeric(s: &str) -> Result<Color, ParseColorError> {

        fn parse_number(s: &str) -> Option<u8> {
            use std::u8;

            if s.starts_with("0x") {
                u8::from_str_radix(&s[2..], 16).ok()
            } else {
                u8::from_str_radix(s, 10).ok()
            }
        }

        let codes: Vec<&str> = s.split(',').collect();
        if codes.len() == 1 {
            if let Some(n) = parse_number(&codes[0]) {
                Ok(Color::Ansi256(n))
            } else {
                if s.chars().all(|c| c.is_digit(16)) {
                    Err(ParseColorError {
                        kind: ParseColorErrorKind::InvalidAnsi256,
                        given: s.to_string(),
                    })
                } else {
                    Err(ParseColorError {
                        kind: ParseColorErrorKind::InvalidName,
                        given: s.to_string(),
                    })
                }
            }
        } else if codes.len() == 3 {
            let mut v = vec![];
            for code in codes {
                let n = parse_number(code).ok_or_else(|| ParseColorError {
                    kind: ParseColorErrorKind::InvalidRgb,
                    given: s.to_string(),
                })?;
                v.push(n);
            }
            Ok(Color::Rgb(v[0], v[1], v[2]))
        } else {
            Err(if s.contains(",") {
                ParseColorError {
                    kind: ParseColorErrorKind::InvalidRgb,
                    given: s.to_string(),
                }
            } else {
                ParseColorError {
                    kind: ParseColorErrorKind::InvalidName,
                    given: s.to_string(),
                }
            })
        }
    }
}

/// An error from parsing an invalid color specification.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ParseColorError {
    kind: ParseColorErrorKind,
    given: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
enum ParseColorErrorKind {
    InvalidName,
    InvalidAnsi256,
    InvalidRgb,
}

impl ParseColorError {
    /// Return the string that couldn't be parsed as a valid color.
    pub fn invalid(&self) -> &str {
        &self.given
    }
}

impl error::Error for ParseColorError {
    fn description(&self) -> &str {
        use self::ParseColorErrorKind::*;
        match self.kind {
            InvalidName => "unrecognized color name",
            InvalidAnsi256 => "invalid ansi256 color number",
            InvalidRgb => "invalid RGB color triple",
        }
    }
}

impl fmt::Display for ParseColorError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use self::ParseColorErrorKind::*;
        match self.kind {
            InvalidName => write!(
                f,
                "unrecognized color name '{}'. Choose from: \
                 black, blue, green, red, cyan, magenta, yellow, \
                 white",
                self.given
            ),
            InvalidAnsi256 => write!(
                f,
                "unrecognized ansi256 color number, \
                 should be '[0-255]' (or a hex number), but is '{}'",
                self.given
            ),
            InvalidRgb => write!(
                f,
                "unrecognized RGB color triple, \
                 should be '[0-255],[0-255],[0-255]' (or a hex \
                 triple), but is '{}'",
                self.given
            ),
        }
    }
}

impl FromStr for Color {
    type Err = ParseColorError;

    fn from_str(s: &str) -> Result<Color, ParseColorError> {
        match &*s.to_lowercase() {
            "black" => Ok(Color::Black),
            "blue" => Ok(Color::Blue),
            "green" => Ok(Color::Green),
            "red" => Ok(Color::Red),
            "cyan" => Ok(Color::Cyan),
            "magenta" => Ok(Color::Magenta),
            "yellow" => Ok(Color::Yellow),
            "white" => Ok(Color::White),
            _ => Color::from_str_numeric(s),
        }
    }
}

/// A hyperlink specification.
#[derive(Clone, Debug)]
pub struct HyperlinkSpec<'a> {
    uri: Option<&'a [u8]>,
}

impl<'a> HyperlinkSpec<'a> {
    /// Creates a new hyperlink specification.
    pub fn open(uri: &'a [u8]) -> HyperlinkSpec<'a> {
        HyperlinkSpec { uri: Some(uri) }
    }

    /// Creates a hyperlink specification representing no hyperlink.
    pub fn close() -> HyperlinkSpec<'a> {
        HyperlinkSpec { uri: None }
    }

    /// Returns the URI of the hyperlink if one is attached to this spec.
    pub fn uri(&self) -> Option<&'a [u8]> {
        self.uri
    }
}

#[derive(Debug)]
struct LossyStandardStream<W> {
    wtr: W,
#[cfg(any())]










    is_console: bool,
}

impl<W: io::Write> LossyStandardStream<W> {
fn new(wtr: W) -> LossyStandardStream<W> {
        LossyStandardStream { wtr }
    }


fn wrap<Q: io::Write>(&self, wtr: Q) -> LossyStandardStream<Q> {
        LossyStandardStream::new(wtr)
    }


    fn get_ref(&self) -> &W {
        &self.wtr
    }
}

impl<W: WriteColor> WriteColor for LossyStandardStream<W> {
    fn supports_color(&self) -> bool {
        self.wtr.supports_color()
    }
    fn supports_hyperlinks(&self) -> bool {
        self.wtr.supports_hyperlinks()
    }
    fn set_color(&mut self, spec: &ColorSpec) -> io::Result<()> {
        self.wtr.set_color(spec)
    }
    fn set_hyperlink(&mut self, link: &HyperlinkSpec) -> io::Result<()> {
        self.wtr.set_hyperlink(link)
    }
    fn reset(&mut self) -> io::Result<()> {
        self.wtr.reset()
    }
    fn is_synchronous(&self) -> bool {
        self.wtr.is_synchronous()
    }
}

impl<W: io::Write> io::Write for LossyStandardStream<W> {
fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.wtr.write(buf)
    }


    fn flush(&mut self) -> io::Result<()> {
        self.wtr.flush()
    }
}
