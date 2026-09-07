//! Functionality for unfilling and refilling text.

use crate::core::display_width;
use crate::line_ending::NonEmptyLines;
use crate::{fill, LineEnding, Options};

/// Unpack a paragraph of already-wrapped text.
///
/// This function attempts to recover the original text from a single
/// paragraph of wrapped text, such as what [`fill()`] would produce.
/// This means that it turns
///
/// ```text
/// textwrap: a small
/// library for
/// wrapping text.
/// ```
///
/// back into
///
/// ```text
/// textwrap: a small library for wrapping text.
/// ```
///
/// In addition, it will recognize a common prefix and a common line
/// ending among the lines.
///
/// The prefix of the first line is returned in
/// [`Options::initial_indent`] and the prefix (if any) of the the
/// other lines is returned in [`Options::subsequent_indent`].
///
/// Line ending is returned in [`Options::line_ending`]. If line ending
/// can not be confidently detected (mixed or no line endings in the
/// input), [`LineEnding::LF`] will be returned.
///
/// In addition to `' '`, the prefixes can consist of characters used
/// for unordered lists (`'-'`, `'+'`, and `'*'`) and block quotes
/// (`'>'`) in Markdown as well as characters often used for inline
/// comments (`'#'` and `'/'`).
///
/// The text must come from a single wrapped paragraph. This means
/// that there can be no empty lines (`"\n\n"` or `"\r\n\r\n"`) within
/// the text. It is unspecified what happens if `unfill` is called on
/// more than one paragraph of text.
///
/// # Examples
///
/// ```
/// use textwrap::{LineEnding, unfill};
///
/// let (text, options) = unfill("\
/// * This is an
///   example of
///   a list item.
/// ");
///
/// assert_eq!(text, "This is an example of a list item.\n");
/// assert_eq!(options.initial_indent, "* ");
/// assert_eq!(options.subsequent_indent, "  ");
/// assert_eq!(options.line_ending, LineEnding::LF);
/// ```
pub fn unfill(text: &str) -> (String, Options<'_>) {
    let prefix_chars: &[_] = &[' ', '-', '+', '*', '>', '#', '/'];

    let mut options = Options::new(0);
    for (idx, line) in text.lines().enumerate() {
        options.width = std::cmp::max(options.width, display_width(line));
        let without_prefix = line.trim_start_matches(prefix_chars);
        let prefix = &line[..line.len() - without_prefix.len()];

        if idx == 0 {
            options.initial_indent = prefix;
        } else if idx == 1 {
            options.subsequent_indent = prefix;
        } else if idx > 1 {
            for ((idx, x), y) in prefix.char_indices().zip(options.subsequent_indent.chars()) {
                if x != y {
                    options.subsequent_indent = &prefix[..idx];
                    break;
                }
            }
            if prefix.len() < options.subsequent_indent.len() {
                options.subsequent_indent = prefix;
            }
        }
    }

    let mut unfilled = String::with_capacity(text.len());
    let mut detected_line_ending = None;

    for (idx, (line, ending)) in NonEmptyLines(text).enumerate() {
        if idx == 0 {
            unfilled.push_str(&line[options.initial_indent.len()..]);
        } else {
            unfilled.push(' ');
            unfilled.push_str(&line[options.subsequent_indent.len()..]);
        }
        match (detected_line_ending, ending) {
            (None, Some(_)) => detected_line_ending = ending,
            (Some(LineEnding::CRLF), Some(LineEnding::LF)) => detected_line_ending = ending,
            _ => (),
        }
    }

    if let Some(line_ending) = detected_line_ending {
        if text.ends_with(line_ending.as_str()) {
            unfilled.push_str(line_ending.as_str());
        }
    }

    options.line_ending = detected_line_ending.unwrap_or(LineEnding::LF);
    (unfilled, options)
}

/// Refill a paragraph of wrapped text with a new width.
///
/// This function will first use [`unfill()`] to remove newlines from
/// the text. Afterwards the text is filled again using [`fill()`].
///
/// The `new_width_or_options` argument specify the new width and can
/// specify other options as well — except for
/// [`Options::initial_indent`] and [`Options::subsequent_indent`],
/// which are deduced from `filled_text`.
///
/// # Examples
///
/// ```
/// use textwrap::refill;
///
/// // Some loosely wrapped text. The "> " prefix is recognized automatically.
/// let text = "\
/// > Memory
/// > safety without garbage
/// > collection.
/// ";
///
/// assert_eq!(refill(text, 20), "\
/// > Memory safety
/// > without garbage
/// > collection.
/// ");
///
/// assert_eq!(refill(text, 40), "\
/// > Memory safety without garbage
/// > collection.
/// ");
///
/// assert_eq!(refill(text, 60), "\
/// > Memory safety without garbage collection.
/// ");
/// ```
///
/// You can also reshape bullet points:
///
/// ```
/// use textwrap::refill;
///
/// let text = "\
/// - This is my
///   list item.
/// ";
///
/// assert_eq!(refill(text, 20), "\
/// - This is my list
///   item.
/// ");
/// ```
pub fn refill<'a, Opt>(filled_text: &str, new_width_or_options: Opt) -> String
where
    Opt: Into<Options<'a>>,
{
    let mut new_options = new_width_or_options.into();
    let (text, options) = unfill(filled_text);
    let stripped = text.strip_suffix(options.line_ending.as_str());
    let new_line_ending = new_options.line_ending.as_str();

    new_options.initial_indent = options.initial_indent;
    new_options.subsequent_indent = options.subsequent_indent;
    let mut refilled = fill(stripped.unwrap_or(&text), new_options);

    if stripped.is_some() {
        refilled.push_str(new_line_ending);
    }
    refilled
}
