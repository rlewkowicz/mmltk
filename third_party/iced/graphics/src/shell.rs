use std::sync::Arc;

#[derive(Clone)]
pub struct Shell(Arc<dyn Notifier>);

impl Shell {
        pub fn new(notifier: impl Notifier) -> Self {
        Self(Arc::new(notifier))
    }

        pub fn headless() -> Self {
        struct Headless;

        impl Notifier for Headless {
            fn tick(&self) {}
            fn request_redraw(&self) {}
            fn invalidate_layout(&self) {}
        }

        Self::new(Headless)
    }

            pub fn tick(&self) {
        self.0.tick();
    }

        pub fn request_redraw(&self) {
        self.0.request_redraw();
    }

        pub fn invalidate_layout(&self) {
        self.0.invalidate_layout();
    }
}

pub trait Notifier: Send + Sync + 'static {
            fn tick(&self);

        fn request_redraw(&self);

        fn invalidate_layout(&self);
}
