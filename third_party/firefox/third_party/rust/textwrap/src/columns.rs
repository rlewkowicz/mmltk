//! Functionality for wrapping text into columns.

use crate::core::display_width;
use crate::{wrap, Options};

/// Wrap text into columns with a given total width.
///
/// The `left_gap`, `middle_gap` and `right_gap` arguments specify the
/// strings to insert before, between, and after the columns. The
/// total width of all columns and all gaps is specified using the
/// `total_width_or_options` argument. This argument can simply be an
/// integer if you want to use default settings when wrapping, or it
/// can be a [`Options`] value if you want to customize the wrapping.
///
/// If the columns are narrow, it is recommended to set
/// [`Options::break_words`] to `true` to prevent words from
/// protruding into the margins.
///
/// The per-column width is computed like this:
///
/// ```
/// # let (left_gap, middle_gap, right_gap) = ("", "", "");
/// # let columns = 2;
/// # let options = textwrap::Options::new(80);
/// let inner_width = options.width
///     - textwrap::core::display_width(left_gap)
///     - textwrap::core::display_width(right_gap)
///     - textwrap::core::display_width(middle_gap) * (columns - 1);
/// let column_width = inner_width / columns;
/// ```
///
/// The `text` is wrapped using [`wrap()`] and the given `options`
/// argument, but the width is overwritten to the computed
/// `column_width`.
///
/// # Panics
///
/// Panics if `columns` is zero.
///
/// # Examples
///
/// ```
/// use textwrap::wrap_columns;
///
/// let text = "\
/// This is an example text, which is wrapped into three columns. \
/// Notice how the final column can be shorter than the others.";
///
/// #[cfg(feature = "smawk")]
/// assert_eq!(wrap_columns(text, 3, 50, "| ", " | ", " |"),
///            vec!["| This is       | into three    | column can be  |",
///                 "| an example    | columns.      | shorter than   |",
///                 "| text, which   | Notice how    | the others.    |",
///                 "| is wrapped    | the final     |                |"]);
///
/// // Without the `smawk` feature, the middle column is a little more uneven:
/// #[cfg(not(feature = "smawk"))]
/// assert_eq!(wrap_columns(text, 3, 50, "| ", " | ", " |"),
///            vec!["| This is an    | three         | column can be  |",
///                 "| example text, | columns.      | shorter than   |",
///                 "| which is      | Notice how    | the others.    |",
///                 "| wrapped into  | the final     |                |"]);
pub fn wrap_columns<'a, Opt>(
    text: &str,
    columns: usize,
    total_width_or_options: Opt,
    left_gap: &str,
    middle_gap: &str,
    right_gap: &str,
) -> Vec<String>
where
    Opt: Into<Options<'a>>,
{
    assert!(columns > 0);

    let mut options: Options = total_width_or_options.into();

    let inner_width = options
        .width
        .saturating_sub(display_width(left_gap))
        .saturating_sub(display_width(right_gap))
        .saturating_sub(display_width(middle_gap) * (columns - 1));

    let column_width = std::cmp::max(inner_width / columns, 1);
    options.width = column_width;
    let last_column_padding = " ".repeat(inner_width % column_width);
    let wrapped_lines = wrap(text, options);
    let lines_per_column =
        wrapped_lines.len() / columns + usize::from(wrapped_lines.len() % columns > 0);
    let mut lines = Vec::new();
    for line_no in 0..lines_per_column {
        let mut line = String::from(left_gap);
        for column_no in 0..columns {
            match wrapped_lines.get(line_no + column_no * lines_per_column) {
                Some(column_line) => {
                    line.push_str(column_line);
                    line.push_str(&" ".repeat(column_width - display_width(column_line)));
                }
                None => {
                    line.push_str(&" ".repeat(column_width));
                }
            }
            if column_no == columns - 1 {
                line.push_str(&last_column_padding);
            } else {
                line.push_str(middle_gap);
            }
        }
        line.push_str(right_gap);
        lines.push(line);
    }

    lines
}
