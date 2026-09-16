use crate::Theme;
use std::sync::Arc;

use iced::{Color, border, widget::container};

/// Produces a [`PlotStyle`] from a given application theme.
pub(crate) type StyleFn = Arc<dyn Fn(&Theme) -> PlotStyle + Send + Sync>;

/// Configures the appearance of a [`PlotWidget`](crate::PlotWidget).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct PlotStyle {
    /// Style for the outer frame surrounding labels, ticks, overlays, and plot area.
    pub frame: container::Style,

    /// Style for the inner plot area behind the GPU-rendered canvas.
    pub plot_area: container::Style,

    /// Style for the legend panel.
    pub legend: container::Style,

    /// Style for the controls/help panel.
    pub controls_panel: container::Style,

    /// Style for the cursor-position overlay bubble.
    pub cursor_overlay: container::Style,

    /// Style for point tooltip bubbles.
    pub tooltip: container::Style,

    /// Style for grid lines rendered inside the plot area.
    pub grid: GridStyle,

    /// Color of tick labels rendered around the plot area.
    pub tick_label_color: Color,

    /// Color of axis labels rendered around the plot area.
    pub axis_label_color: Color,
}

/// Configures the appearance of grid lines inside the plot area.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct GridStyle {
    /// Color of major grid lines.
    pub major: Color,

    /// Color of minor grid lines.
    pub minor: Color,

    /// Color of sub-minor grid lines.
    pub sub_minor: Color,
}

/// Returns the default plot style derived from the current application theme.
pub fn default_style(theme: &Theme) -> PlotStyle {
    let tokens = theme.tokens();
    let panel = container::Style {
        background: Some(tokens.neutral_background1.into()),
        text_color: Some(tokens.neutral_foreground1),
        border: border::rounded(5).color(tokens.neutral_stroke1).width(1),
        ..Default::default()
    };
    PlotStyle {
        frame: panel, plot_area: panel, legend: panel, controls_panel: panel,
        cursor_overlay: panel, tooltip: panel,
        grid: GridStyle {
            major: with_alpha(tokens.neutral_foreground1, 0.45),
            minor: with_alpha(tokens.neutral_foreground1, 0.28),
            sub_minor: with_alpha(tokens.neutral_foreground1, 0.10),
        },
        tick_label_color: tokens.neutral_foreground1,
        axis_label_color: tokens.neutral_foreground1,
    }
}

fn with_alpha(color: Color, alpha: f32) -> Color {
    Color { a: alpha, ..color }
}
