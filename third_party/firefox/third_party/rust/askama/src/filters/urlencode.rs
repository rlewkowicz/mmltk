use std::convert::Infallible;
use std::fmt;
use std::fmt::Write;

use percent_encoding::{AsciiSet, NON_ALPHANUMERIC, utf8_percent_encode};

use crate::filters::HtmlSafeOutput;
use crate::{FastWritable, Values};

const URLENCODE_STRICT_SET: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'_')
    .remove(b'.')
    .remove(b'-')
    .remove(b'~');

const URLENCODE_SET: &AsciiSet = &URLENCODE_STRICT_SET.remove(b'/');

/// Percent-encodes the argument for safe use in URI; does not encode `/`.
///
/// This should be safe for all parts of URI (paths segments, query keys, query
/// values). In the rare case that the server can't deal with forward slashes in
/// the query string, use [`urlencode_strict`], which encodes them as well.
///
/// Encodes all characters except ASCII letters, digits, and `_.-~/`. In other
/// words, encodes all characters which are not in the unreserved set,
/// as specified by [RFC3986](https://tools.ietf.org/html/rfc3986#section-2.3),
/// with the exception of `/`.
///
/// ```none,ignore
/// <a href="/metro{{ "/stations/Château d'Eau"|urlencode }}">Station</a>
/// <a href="/page?text={{ "look, unicode/emojis ✨"|urlencode }}">Page</a>
/// ```
///
/// To encode `/` as well, see [`urlencode_strict`](./fn.urlencode_strict.html).
///
/// [`urlencode_strict`]: ./fn.urlencode_strict.html
///
/// ```
/// # #[cfg(feature = "code-in-doc")] {
/// # use askama::Template;
/// /// ```jinja
/// /// <div>{{ example|urlencode }}</div>
/// /// ```
/// #[derive(Template)]
/// #[template(ext = "html", in_doc = true)]
/// struct Example<'a> {
///     example: &'a str,
/// }
///
/// assert_eq!(
///     Example { example: "hello?world" }.to_string(),
///     "<div>hello%3Fworld</div>"
/// );
/// # }
/// ```
#[inline]
pub fn urlencode<T>(s: T) -> Result<HtmlSafeOutput<UrlencodeFilter<T>>, Infallible> {
    Ok(HtmlSafeOutput(UrlencodeFilter(s, URLENCODE_SET)))
}

/// Percent-encodes the argument for safe use in URI; encodes `/`.
///
/// Use this filter for encoding query keys and values in the rare case that
/// the server can't process them unencoded.
///
/// Encodes all characters except ASCII letters, digits, and `_.-~`. In other
/// words, encodes all characters which are not in the unreserved set,
/// as specified by [RFC3986](https://tools.ietf.org/html/rfc3986#section-2.3).
///
/// ```none,ignore
/// <a href="/page?text={{ "look, unicode/emojis ✨"|urlencode_strict }}">Page</a>
/// ```
///
/// If you want to preserve `/`, see [`urlencode`](./fn.urlencode.html).
///
/// ```
/// # #[cfg(feature = "code-in-doc")] {
/// # use askama::Template;
/// /// ```jinja
/// /// <a href='{{ example|urlencode_strict }}'>Example</a>
/// /// ```
/// #[derive(Template)]
/// #[template(ext = "html", in_doc = true)]
/// struct Example<'a> {
///     example: &'a str,
/// }
///
/// assert_eq!(
///     Example { example: "/hello/world" }.to_string(),
///     "<a href='%2Fhello%2Fworld'>Example</a>"
/// );
/// # }
/// ```
#[inline]
pub fn urlencode_strict<T>(s: T) -> Result<HtmlSafeOutput<UrlencodeFilter<T>>, Infallible> {
    Ok(HtmlSafeOutput(UrlencodeFilter(s, URLENCODE_STRICT_SET)))
}

pub struct UrlencodeFilter<T>(pub T, pub &'static AsciiSet);

impl<T: fmt::Display> fmt::Display for UrlencodeFilter<T> {
    #[inline]
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(UrlencodeWriter(f, self.1), "{}", self.0)
    }
}

impl<T: FastWritable> FastWritable for UrlencodeFilter<T> {
    #[inline]
    fn write_into<W: fmt::Write + ?Sized>(
        &self,
        f: &mut W,
        values: &dyn Values,
    ) -> crate::Result<()> {
        self.0.write_into(&mut UrlencodeWriter(f, self.1), values)
    }
}

struct UrlencodeWriter<W>(W, &'static AsciiSet);

impl<W: fmt::Write> fmt::Write for UrlencodeWriter<W> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for s in utf8_percent_encode(s, self.1) {
            self.0.write_str(s)?;
        }
        Ok(())
    }
}
