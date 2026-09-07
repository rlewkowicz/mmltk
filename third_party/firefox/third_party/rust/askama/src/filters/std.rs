use core::convert::Infallible;
use std::collections::HashMap;
use std::collections::hash_map::Entry;
use std::hash::Hash;
use std::rc::Rc;

/// Returns an iterator with all duplicates removed.
///
/// The sorting order is kept, no extra allocation is performed. However, to make it possible,
/// the data is wrapped inside `Rc`.
///
/// ```
/// # use askama::Template;
/// #[derive(Template)]
/// #[template(ext = "html", source = "{% for elem in example|unique %}{{ elem }},{% endfor %}")]
/// struct Example<'a> {
///     example: Vec<&'a str>,
/// }
///
/// assert_eq!(
///     Example { example: vec!["a", "b", "a", "c"] }.to_string(),
///     "a,b,c,"
/// );
/// ```
pub fn unique<T: Hash + Eq>(
    it: impl IntoIterator<Item = T>,
) -> Result<impl Iterator<Item = Rc<T>>, Infallible> {
    let mut set = HashMap::new();

    Ok(it.into_iter().filter_map(move |elem| {
        if let Entry::Vacant(entry) = set.entry(Rc::new(elem)) {
            Some(Rc::clone(entry.insert_entry(()).key()))
        } else {
            None
        }
    }))
}
