use crate::time::Instant;
use crate::{Point, Size};

use std::path::PathBuf;

#[derive(PartialEq, Clone, Debug)]
pub enum Event {
        Opened {
                                                position: Option<Point>,
                        size: Size,
                scale_factor: f32,
    },

        Closed,

        Moved(Point),

        Resized(Size),

        Rescaled(f32),

                RedrawRequested(Instant),

        CloseRequested,

        Focused,

        Unfocused,

                                    FileHovered(PathBuf),

                                    FileDropped(PathBuf),

                                    FilesHoveredLeft,
}
