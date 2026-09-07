use crate::pane_grid::Axis;

#[derive(Debug, Clone)]
pub enum Configuration<T> {
        Split {
                axis: Axis,

                ratio: f32,

                a: Box<Configuration<T>>,

                b: Box<Configuration<T>>,
    },
                Pane(T),
}
