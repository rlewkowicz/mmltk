//! Component-local product state; queue submission and GPU settlement are distinct.
use iced::advanced::text::{self, Paragraph as _};
use iced::time::{Duration, Instant};
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

const REFRESH: Duration = Duration::from_millis(500);

pub(crate) fn enabled(settings: &crate::view::settings::SettingsModel) -> bool {
    settings
        .draft
        .as_ref()
        .is_some_and(|draft| draft.ui.showworkspaceperformance)
}

pub(crate) fn draw(
    renderer: &mut iced::Renderer,
    theme: &crate::fluent_theme::Theme,
    meter: &Meter,
    clip: iced::Rectangle,
    control: &'static str,
) {
    use iced::advanced::Renderer as _;
    use iced::advanced::renderer;
    use iced::advanced::text::Renderer as _;
    if clip.width < 80.0 || clip.height < 28.0 {
        return;
    }
    let bounds = iced::Rectangle {
        x: clip.x + clip.width - 80.0,
        y: clip.y + 6.0,
        width: 74.0,
        height: 22.0,
    };
    let dark = theme.is_dark();
    renderer.fill_quad(
        renderer::Quad {
            bounds,
            ..Default::default()
        },
        if dark {
            iced::Color::BLACK
        } else {
            iced::Color::WHITE
        },
    );
    let text_bounds = iced::Rectangle {
        x: bounds.x + 4.0,
        y: bounds.y,
        width: bounds.width - 8.0,
        height: bounds.height,
    };
    let minimum = meter.paragraph.min_bounds();
    renderer.fill_paragraph(
        &meter.paragraph,
        iced::Point::new(
            text_bounds.x + text_bounds.width - minimum.width,
            text_bounds.y + (text_bounds.height - minimum.height) / 2.0,
        ),
        if dark {
            iced::Color::WHITE
        } else {
            iced::Color::BLACK
        },
        clip,
    );
    crate::integration_control::report_workspace_fps(control, meter, bounds, clip, dark);
}

pub(crate) struct Meter {
    observer: Arc<dyn Fn() + Send + Sync>,
    submitted: Arc<AtomicU64>,
    previous: u64,
    since: Instant,
    text: String,
    text_scratch: String,
    paragraph: iced::advanced::graphics::text::Paragraph,
    pub(crate) frames: u64,
    pub(crate) seconds: f64,
}

impl Meter {
    fn paragraph(content: &str) -> iced::advanced::graphics::text::Paragraph {
        iced::advanced::graphics::text::Paragraph::with_text(text::Text {
            content,
            bounds: iced::Size::new(66.0, 22.0),
            size: iced::Pixels(12.0),
            line_height: text::LineHeight::default(),
            font: iced::Font::DEFAULT,
            align_x: text::Alignment::Right,
            align_y: iced::alignment::Vertical::Center,
            shaping: text::Shaping::Basic,
            wrapping: text::Wrapping::None,
            ellipsis: text::Ellipsis::default(),
            hint_factor: None,
        })
    }

    fn new(now: Instant) -> Self {
        let submitted = Arc::new(AtomicU64::new(0));
        let counter = submitted.clone();
        Self {
            observer: Arc::new(move || {
                counter.fetch_add(1, Ordering::Relaxed);
            }),
            submitted,
            previous: 0,
            since: now,
            text: "0 FPS".into(),
            text_scratch: String::with_capacity(16),
            paragraph: Self::paragraph("0 FPS"),
            frames: 0,
            seconds: 0.0,
        }
    }

    pub(crate) fn update(owner: &mut Option<Self>, enabled: bool, event: &iced::Event) {
        if !enabled {
            *owner = None;
            return;
        }
        let iced::Event::Window(iced::window::Event::RedrawRequested(now)) = event else {
            return;
        };
        let meter = owner.get_or_insert_with(|| Self::new(*now));
        let elapsed = now.saturating_duration_since(meter.since);
        if elapsed >= REFRESH {
            let submitted = meter.submitted.load(Ordering::Relaxed);
            meter.frames = submitted.wrapping_sub(meter.previous);
            meter.seconds = elapsed.as_secs_f64();
            use std::fmt::Write;
            meter.text_scratch.clear();
            let _ = write!(
                meter.text_scratch,
                "{:.0} FPS",
                meter.frames as f64 / meter.seconds
            );
            if meter.text_scratch != meter.text {
                std::mem::swap(&mut meter.text, &mut meter.text_scratch);
                meter.paragraph = Self::paragraph(&meter.text);
            }
            meter.previous = submitted;
            meter.since = *now;
        }
    }

    pub(crate) fn observer(&self) -> Arc<dyn Fn() + Send + Sync> {
        self.observer.clone()
    }

    pub(crate) fn text(&self) -> &str {
        &self.text
    }

    pub(crate) fn sample_time(&self) -> Instant {
        self.since
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn submitted_workspace_frames_refresh_the_cached_half_second_display() {
        let start = Instant::now();
        let mut meter = Some(Meter::new(start));
        let observer = meter.as_ref().unwrap().observer();
        let initial_paragraph = meter.as_ref().unwrap().paragraph.clone();
        for _ in 0..30 {
            observer();
        }
        let redraw = |milliseconds| {
            iced::Event::Window(iced::window::Event::RedrawRequested(
                start + Duration::from_millis(milliseconds),
            ))
        };
        Meter::update(&mut meter, true, &redraw(499));
        assert_eq!(meter.as_ref().unwrap().text(), "0 FPS");
        assert!(std::ptr::eq(
            meter.as_ref().unwrap().paragraph.buffer(),
            initial_paragraph.buffer()
        ));
        Meter::update(&mut meter, true, &redraw(500));
        assert_eq!(meter.as_ref().unwrap().text(), "60 FPS");
        assert!(!std::ptr::eq(
            meter.as_ref().unwrap().paragraph.buffer(),
            initial_paragraph.buffer()
        ));
        for _ in 0..20 {
            observer();
        }
        Meter::update(&mut meter, true, &redraw(1000));
        assert_eq!(meter.as_ref().unwrap().text(), "40 FPS");
        let retained_paragraph = meter.as_ref().unwrap().paragraph.clone();
        for _ in 0..20 {
            observer();
        }
        Meter::update(&mut meter, true, &redraw(1500));
        assert_eq!(meter.as_ref().unwrap().text(), "40 FPS");
        assert!(std::ptr::eq(
            meter.as_ref().unwrap().paragraph.buffer(),
            retained_paragraph.buffer()
        ));
        Meter::update(&mut meter, false, &redraw(1501));
        Meter::update(&mut meter, true, &redraw(1600));
        assert_eq!(meter.as_ref().unwrap().text(), "0 FPS");
    }
}
