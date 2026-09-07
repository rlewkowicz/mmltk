use crate::core::{Rectangle, Transformation};

pub trait Layer: Default {
        fn with_bounds(bounds: Rectangle) -> Self;

        fn bounds(&self) -> Rectangle;

                    fn flush(&mut self);

        fn resize(&mut self, bounds: Rectangle);

        fn reset(&mut self);

                                            fn start(&self) -> usize;

        fn end(&self) -> usize;

        fn merge(&mut self, _layer: &mut Self);
}

#[derive(Debug)]
pub struct Stack<T: Layer> {
    layers: Vec<T>,
    transformations: Vec<Transformation>,
    previous: Vec<usize>,
    current: usize,
    active_count: usize,
}

impl<T: Layer> Stack<T> {
        pub fn new() -> Self {
        Self {
            layers: vec![T::default()],
            transformations: vec![Transformation::IDENTITY],
            previous: vec![],
            current: 0,
            active_count: 1,
        }
    }

            #[inline]
    pub fn current_mut(&mut self) -> (&mut T, Transformation) {
        let transformation = self.transformation();

        (&mut self.layers[self.current], transformation)
    }

        #[inline]
    pub fn transformation(&self) -> Transformation {
        self.transformations.last().copied().unwrap()
    }

            pub fn push_clip(&mut self, bounds: Rectangle) {
        self.previous.push(self.current);

        self.current = self.active_count;
        self.active_count += 1;

        let bounds = bounds * self.transformation();

        if self.current == self.layers.len() {
            self.layers.push(T::with_bounds(bounds));
        } else {
            self.layers[self.current].resize(bounds);
        }
    }

                pub fn pop_clip(&mut self) {
        self.flush();

        self.current = self.previous.pop().unwrap();
    }

                            pub fn push_transformation(&mut self, transformation: Transformation) {
        self.transformations
            .push(self.transformation() * transformation);
    }

        pub fn pop_transformation(&mut self) {
        let _ = self.transformations.pop();
    }

        pub fn iter(&self) -> impl Iterator<Item = &T> {
        self.layers[..self.active_count].iter()
    }

        pub fn as_slice(&self) -> &[T] {
        &self.layers[..self.active_count]
    }

        pub fn flush(&mut self) {
        self.layers[self.current].flush();
    }

                pub fn merge(&mut self) {
        self.flush();

        let mut left = self.active_count;

        while left > 1 {
            let mut current = left - 1;
            let mut target = &self.layers[current];
            let mut target_start = target.start();
            let mut target_index = current;

            while current > 0 {
                current -= 1;

                let candidate = &self.layers[current];
                let start = candidate.start();
                let end = candidate.end();

                if end == 0 {
                    continue;
                }

                if end > target_start || candidate.bounds() != target.bounds() {
                    break;
                }

                target = candidate;
                target_start = start;
                target_index = current;
            }

            let (head, tail) = self.layers.split_at_mut(target_index + 1);
            let layer = &mut head[target_index];

            for middle in &mut tail[0..left - target_index - 1] {
                layer.merge(middle);
            }

            left = current;
        }
    }

                        pub fn reset(&mut self, new_bounds: Rectangle) {
        for layer in self.layers[..self.active_count].iter_mut() {
            layer.reset();
        }

        self.layers[0].resize(new_bounds);
        self.current = 0;
        self.active_count = 1;
        self.previous.clear();
    }
}

impl<T: Layer> Default for Stack<T> {
    fn default() -> Self {
        Self::new()
    }
}
