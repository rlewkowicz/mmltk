pub mod column;

pub use column::Column;

#[macro_export]
macro_rules! keyed_column {
    () => (
        $crate::keyed::Column::new()
    );
    ($(($key:expr, $x:expr)),+ $(,)?) => (
        $crate::keyed::Column::with_children(vec![$(($key, $crate::core::Element::from($x))),+])
    );
}
