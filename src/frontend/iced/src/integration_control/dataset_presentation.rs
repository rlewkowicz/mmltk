//! Bounded, opt-in observations of the same component used by the Dataset card.
//! Fixtures never enter the application model or masquerade as native work.
use super::{Message, pixel_checks::ProbeOutcome, probe, reporting};
use crate::fluent_theme::{Element, Theme};
use crate::generated::*;
use crate::message::Message as RootMessage;
use crate::view::train::dataset::progress;
use iced::advanced::{Layout, Shell, Widget, layout, mouse, overlay, renderer, widget};
use iced::{Event, Length, Rectangle, Size, Vector};
use std::cell::{Cell, RefCell};

pub(super) const FIXTURE_COUNT: u8 = 36;
pub(super) const OBSERVATION_BUDGET: u16 = 180;
pub(super) fn input_key(index: u8) -> u16 {
    if index >= 19 {
        120 + u16::from(index - 19)
    } else {
        80 + u16::from(index)
    }
}
fn label_probe(key: u16) -> bool {
    (2..=5).contains(&key) || (121..=125).contains(&key)
}
const IDS: [&str; 26] = [
    progress::AREA_ID,
    progress::WORK_ID,
    progress::TRACK_IDS[0],
    progress::TRACK_IDS[1],
    progress::TRACK_IDS[2],
    "train.dataset.progress.source.objects365v2",
    "train.dataset.benchmark_choices",
    "train.dataset.coconut_options",
    "train.dataset.splits",
    "train.dataset.compile_size",
    "train.dataset.benchmark_divider",
    "train.dataset.dimensions_divider",
    "train.dataset.benchmark_override",
    "train.dataset.benchmark.coconut",
    "train.dataset.benchmark.custom",
    "train.dataset.coconut.recover_dropped_masks",
    "train.dataset.source",
    "train.dataset.compile_dimensions",
    "diagnostics.summary",
    "diagnostics.content",
    "train.dataset.validation.coconut",
    "train.dataset.validation.stock",
    "train.dataset.validation.coconut_stock",
    "train.dataset.validation.coconut.description",
    "train.dataset.validation.stock.description",
    "train.dataset.validation.coconut_stock.description",
];

#[derive(Clone, Debug, Default)]
pub struct Row {
    pub id: &'static str,
    pub bounds: Rectangle,
    pub text: String,
}
#[derive(Clone, Debug)]
pub struct NativeFrame {
    pub generation: u64,
    pub phase: DatasetCompilePhase,
    pub completed: u64,
    pub total: u64,
    pub tracks: DatasetCompileTracks,
}
impl NativeFrame {
    fn from_state(state: &ArtifactUiState) -> Option<Self> {
        (state.active
            && state.terminal.outcome != ArtifactTerminalOutcome::CancellationRequested
            && state.progress.phase != DatasetCompilePhase::Idle)
            .then(|| Self {
                generation: state.generation,
                phase: state.progress.phase,
                completed: state.progress.completed,
                total: state.progress.total,
                tracks: state.progress.tracks.clone(),
            })
    }
    fn report(&self, sink: &reporting::Sink) {
        // Decimal JSON preserves every native u64 bit through the JS reporter.
        sink.record(
            "integration.dataset_native_work",
            progress::WORK_ID,
            &format!(
                r#"{{"generation":{},"phase":{},"completed":{},"total":{}}}"#,
                self.generation, self.phase as u8, self.completed, self.total
            ),
            [0.0; 4],
        );
        for (id, track) in progress::TRACK_IDS.into_iter().zip([
            &self.tracks.acquisition,
            &self.tracks.labels,
            &self.tracks.pixels,
        ]) {
            sink.record("integration.dataset_native_track", id,
                &format!(r#"{{"generation":{},"activity":{},"completed":{},"total":{},"known":{},"active":{},"complete":{},"invalidated":{}}}"#,
                    self.generation, track.activity as u8, track.completed, track.total,
                    track.totalknown, track.active, track.complete, track.invalidated), [0.0; 4]);
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Frame {
    pub key: u16,
    pub native: Option<NativeFrame>,
    pub rows: Vec<Row>,
    pub offset: Vector,
    pub scale: f32,
    pub page: Rectangle,
    pub selected: Option<String>,
    pub horizontal: f32,
    translation: Vector,
    pub colors: [f32; 7],
    paints: Vec<Paint>,
}
impl Frame {
    pub(super) fn row(&self, id: &str) -> Option<&Row> {
        self.rows.iter().find(|row| row.id == id)
    }
    pub(super) fn same_geometry(&self, other: &Self) -> bool {
        self.key == other.key
            && self.offset == other.offset
            && self.page == other.page
            && self.horizontal == other.horizontal
            && self.rows.len() == other.rows.len()
            && self
                .rows
                .iter()
                .zip(&other.rows)
                .all(|(a, b)| a.id == b.id && a.bounds == b.bounds)
    }
    fn same_capture(&self, other: &Self) -> bool {
        self.same_geometry(other)
            && self.scale == other.scale
            && self.translation == other.translation
            && self.colors == other.colors
            && self.paints == other.paints
            && self
                .rows
                .iter()
                .zip(&other.rows)
                .all(|(a, b)| a.text == b.text)
    }
}
#[derive(Default)]
pub(super) struct State {
    pub frame: Option<Frame>,
    pub stable: u8,
    pub frames: u16,
    pub pixels: Option<u16>,
    pixel_pending: Option<(u64, u16)>,
    pixel_sequence: u64,
    pixel_finished: bool,
    custody: u8,
    pub fixture: Option<ArtifactUiState>,
    pub diagnostics: crate::view::diagnostics::Component,
    pub input_pending: bool,
    pub input_original: String,
    pub input_selection: Option<BenchmarkDatasetSelection>,
    pub input_expected: String,
    pub pressed: Rectangle,
}
impl State {
    pub fn observe(&mut self, frame: Frame) {
        let unchanged = self
            .frame
            .as_ref()
            .is_some_and(|old| old.same_capture(&frame));
        if self.frame.as_ref().is_none_or(|old| old.key != frame.key) {
            self.frames = 0;
        }
        if !unchanged {
            self.pixels = None;
            // Intentional geometry/owner custody retains only the terminal
            // handoff until the deferred rejection and restoration drain.
            if !matches!(self.custody, 4 | 5) {
                if let Some((request, _)) = self.pixel_pending.take() {
                    retire(request);
                }
            }
            self.pixel_finished = false;
        }
        self.frames = self.frames.saturating_add(1);
        self.stable = if unchanged {
            self.stable.saturating_add(1)
        } else {
            0
        };
        if (frame.key >= 200 || frame.key == 12 || label_probe(frame.key))
            && self.stable >= 3
            && self.pixel_pending.is_none()
            && !self.pixel_finished
        {
            self.pixel_sequence += 1;
            self.pixel_pending = Some((self.pixel_sequence, frame.key));
            sample(
                &frame,
                self.pixel_sequence,
                if self.custody <= 6 { self.custody } else { 0 },
                OBSERVATION_BUDGET.saturating_sub(self.frames),
            );
            if self.custody == 2 {
                // Supersession is same-key request custody, not product state.
                self.custody = 3;
                self.pixel_sequence += 1;
                self.pixel_pending = Some((self.pixel_sequence, frame.key));
                sample(
                    &frame,
                    self.pixel_sequence,
                    if self.custody <= 6 { self.custody } else { 0 },
                    OBSERVATION_BUDGET.saturating_sub(self.frames),
                );
            }
        }
        self.frame = Some(frame);
    }
    /// Only the current request can settle this observation. Invalidation needs
    /// fresh stable draws; a measured failure remains terminal for these facts.
    pub fn complete(&mut self, request: u64, key: u16, outcome: ProbeOutcome) -> bool {
        if self.pixel_pending != Some((request, key)) {
            return false;
        }
        self.pixel_pending = None;
        if matches!(self.custody, 4 | 5) && outcome != ProbeOutcome::Invalidated {
            self.pixel_finished = true;
            return true;
        }
        match outcome {
            ProbeOutcome::Invalidated => {
                if matches!(self.custody, 4 | 5) {
                    self.custody += 1;
                }
                self.stable = 0;
                false
            }
            ProbeOutcome::Observed(_, _) => {
                self.pixel_finished = true;
                self.pixels = Some(key);
                false
            }
            ProbeOutcome::Failed => {
                self.pixel_finished = true;
                true
            }
        }
    }
    pub fn custody_complete(&mut self) -> bool {
        if self.custody >= 6 {
            self.custody = 7;
            return true;
        }
        self.custody = match self.custody {
            0 => 1,
            1 => 2,
            3 => 4,
            _ => return false,
        };
        self.pixels = None;
        self.pixel_finished = false;
        self.stable = 0;
        false
    }
    pub fn settled(&self, key: u16) -> bool {
        self.frame.as_ref().is_some_and(|frame| frame.key == key) && self.stable >= 3
    }
    pub fn install(&mut self, index: u8) {
        let state = self.fixture.get_or_insert_with(|| {
            application_snapshot_defaults()
                .unwrap()
                .into_iter()
                .find_map(|fact| match fact.value {
                    ApplicationSnapshot::Dataset(value) => Some(value),
                    _ => None,
                })
                .unwrap()
        });
        // Two bounded widths exercise wrapping without borrowing any domain state.
        if index % 9 == 0 {
            state.generation += 1;
            state.active = true;
            state.terminal.outcome = ArtifactTerminalOutcome::Idle;
            state.terminal.detail.clear();
            state.progress.phase = DatasetCompilePhase::Pixels;
            state.progress.completed = 1234;
            state.progress.total = 5678;
            state.progress.activity = "Preparing local fixture".into();
            state.progress.elapsedseconds = 1234;
            state.progress.remainingseconds = 5678;
            state.progress.throughputpersecond = 123;
            state.progress.projectedoutputbytes = 85 * 1024 * 1024 * 1024;
            state.progress.droppedinstances = 1234;
            state.progress.quarantinedimages = 12;
            for track in [
                &mut state.progress.tracks.acquisition,
                &mut state.progress.tracks.labels,
                &mut state.progress.tracks.pixels,
            ] {
                track.completed = 2345;
                track.total = 9876;
                track.totalknown = true;
                track.active = true;
                track.complete = false;
                track.activity = DatasetCompileActivity::Compiling;
            }
            state.progress.tracks.acquisition.totalknown = false;
            state.progress.tracks.acquisition.activity = DatasetCompileActivity::Acquiring;
            state.progress.tracks.labels.active = false;
            state.progress.tracks.labels.activity = DatasetCompileActivity::Preparing;
            state.progress.sources = vec![progress::fixtures::cached_source()];
        }
        match index % 9 {
            1 => {
                state.progress.activity.clear();
                state.progress.elapsedseconds = 0;
                state.progress.remainingseconds = 0;
                state.progress.throughputpersecond = 0;
                state.progress.projectedoutputbytes = 0;
                state.progress.droppedinstances = 0;
                state.progress.quarantinedimages = 0;
                let source = &mut state.progress.sources[0];
                progress::fixtures::resume_source(source);
                source.completedbytes = 4096;
                source.totalbytes = 0;
                source.completedimages = 0;
                source.totalimages = 0;
            }
            2 | 3 => {
                let source = &mut state.progress.sources[0];
                source.cachehit = true;
                source.complete = true;
                source.bytetotalknown = true;
                source.completedbytes = 85 * 1024 * 1024 * 1024;
                source.totalbytes = source.completedbytes;
                source.completedimages = 345491;
                source.totalimages = 345491;
                source.retrycount = 0;
                source.resumed = false;
                source.invalidatedimages = 0;
                source.transfer = None;
                state.progress.phase = if index % 9 == 2 {
                    DatasetCompilePhase::Syncing
                } else {
                    DatasetCompilePhase::Publishing
                };
                for track in [
                    &mut state.progress.tracks.acquisition,
                    &mut state.progress.tracks.labels,
                    &mut state.progress.tracks.pixels,
                ] {
                    track.completed = track.total;
                    track.totalknown = true;
                    track.complete = true;
                    track.active = false;
                    track.activity = DatasetCompileActivity::Complete;
                }
            }
            4 => state.terminal.outcome = ArtifactTerminalOutcome::CancellationRequested,
            5 => {
                state.active = false;
                state.terminal.outcome = ArtifactTerminalOutcome::Cancelled;
            }
            6 => {
                state.terminal.outcome = ArtifactTerminalOutcome::Failed;
                state.terminal.detail = "Local fixture publication failed".into();
            }
            7 => {
                state.terminal.outcome = ArtifactTerminalOutcome::Succeeded;
                state.terminal.detail = "Local fixture output".into();
            }
            8 => {
                // Retained late progress cannot reopen settled presentation.
                state.progress.activity = "Late progress must remain hidden".into();
                state.progress.completed = 1;
            }
            _ => {}
        }
    }
}

struct Capture {
    frame: Frame,
    translation: Vector,
    pending_translation: Vector,
    clip: Option<Rectangle>,
    pending_clip: Option<Rectangle>,
    row: Option<usize>,
    pending_row: Option<usize>,
}
impl Capture {
    fn new(key: u16) -> Self {
        Self {
            frame: Frame {
                key,
                ..Frame::default()
            },
            translation: Vector::ZERO,
            pending_translation: Vector::ZERO,
            clip: None,
            pending_clip: None,
            row: None,
            pending_row: None,
        }
    }
    fn translated(&self, bounds: Rectangle) -> Rectangle {
        Rectangle {
            x: bounds.x - self.translation.x,
            y: bounds.y - self.translation.y,
            ..bounds
        }
    }
}
impl widget::Operation for Capture {
    fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
        let row = self.row;
        let translation = self.translation;
        let clip = self.clip;
        self.clip = self.pending_clip.take().or(clip);
        self.row = self.pending_row.take().or(row);
        self.translation += self.pending_translation;
        self.pending_translation = Vector::ZERO;
        operate(self);
        self.row = row;
        self.translation = translation;
        self.clip = clip;
    }
    fn container(&mut self, id: Option<&widget::Id>, bounds: Rectangle) {
        self.pending_row = None;
        if id == Some(&widget::Id::from("integration.dataset.fixture")) {
            self.frame.rows.clear();
            self.row = None;
        }
        if id == Some(&widget::Id::from("train.dataset.benchmark_choices")) {
            self.frame.translation = self.translation;
        }
        if let Some(id) = IDS
            .iter()
            .find(|candidate| id == Some(&widget::Id::from(**candidate)))
        {
            self.pending_row = Some(self.frame.rows.len());
            self.frame.rows.push(Row {
                id,
                bounds: self.translated(bounds),
                text: String::new(),
            });
        }
    }
    fn scrollable(
        &mut self,
        id: Option<&widget::Id>,
        bounds: Rectangle,
        _content: Rectangle,
        translation: Vector,
        _state: &mut dyn widget::operation::Scrollable,
    ) {
        let bounds = self.translated(bounds);
        let clip = self.clip.map_or(bounds, |clip| {
            clip.intersection(&bounds).unwrap_or_default()
        });
        self.pending_clip = Some(clip);
        if id == Some(&widget::Id::from(crate::view::PAGE_SCROLL_ID)) {
            self.frame.offset = translation;
            self.frame.page = clip;
        }
        if id == Some(&widget::Id::from(crate::view::HORIZONTAL_SCROLL_ID)) {
            self.frame.horizontal = translation.x;
        }
        self.pending_translation += translation;
    }
    fn text_input(
        &mut self,
        id: Option<&widget::Id>,
        bounds: Rectangle,
        state: &mut dyn widget::operation::TextInput,
    ) {
        if id
            == Some(&widget::Id::from(
                constraint_workflowstrainrequesttraincompiledpath()
                    .stable_field_id
                    .to_string(),
            ))
        {
            self.frame.selected = state.selected_text();
            self.frame.rows.push(Row {
                id: "train.dataset.split.input",
                bounds: self.translated(bounds),
                text: state.text().into(),
            });
        }
    }
    fn text(&mut self, _id: Option<&widget::Id>, bounds: Rectangle, text: &str) {
        if text == "Infer train/validation splits" {
            self.frame.rows.push(Row {
                id: "train.dataset.infer.observed",
                bounds: self.translated(bounds),
                text: text.into(),
            });
        }
        if text == "Overwrite" {
            self.frame.rows.push(Row {
                id: "train.dataset.overwrite.observed",
                bounds: self.translated(bounds),
                text: text.into(),
            });
        }
        if let Some(index) = self.row {
            let row = &mut self.frame.rows[index];
            if !row.text.is_empty() {
                row.text.push('\n');
            }
            row.text.push_str(text);
        }
    }
}

pub(super) fn wrap<'a>(
    content: Element<'a, RootMessage>,
    key: u16,
    fixture: Option<&ArtifactUiState>,
    width: f32,
    scale: f32,
    native: Option<&ArtifactUiState>,
) -> Element<'a, RootMessage> {
    let content = match fixture {
        Some(state) => iced::widget::stack![
            content,
            iced::widget::container(
                iced::widget::container(iced::widget::column![
                    iced::widget::container(crate::view::shared::card_section_divider())
                        .id("train.dataset.benchmark_divider")
                        .width(iced::Fill),
                    progress::view(Some(state)),
                    iced::widget::container(crate::view::shared::card_section_divider())
                        .id("train.dataset.dimensions_divider")
                        .width(iced::Fill),
                ])
                .id("integration.dataset.fixture")
                .width(width)
                .padding(12)
                .style(crate::fluent_theme::container_card)
            )
            .width(iced::Fill)
            .height(iced::Fill)
            .style(crate::fluent_theme::container_shell)
        ]
        .into(),
        None => iced::widget::stack![content].into(),
    };
    let observed: Element<'a, RootMessage> = Element::new(Observed {
        content,
        key,
        scale,
        native: native
            .filter(|_| key == 100)
            .and_then(NativeFrame::from_state),
        frame: RefCell::new(Frame::default()),
        sent: Cell::new(false),
    });
    iced::widget::themer(
        fixture.map(|_| crate::fluent_theme::app_theme(key >= 218)),
        observed,
    )
    .into()
}
// Transparent observation boundaries preserve each child's tree and layout.
// The checkbox's private paragraph is observed through its actual label node;
// public text/radio paragraphs additionally expose the resolved font metrics.
#[derive(Clone, Copy)]
pub(crate) enum TextKind {
    Radio,
    Checkbox,
    Description,
}
type RenderParagraph = <iced::Renderer as iced::advanced::text::Renderer>::Paragraph;
#[derive(Clone, Debug, PartialEq)]
struct Paint {
    hint_factor: Option<f32>,
    font: iced::Font,
    shaping: iced::advanced::text::Shaping,
    wrapping: iced::advanced::text::Wrapping,
    id: &'static str,
    bounds: Rectangle,
    label: Rectangle,
    clip: Rectangle,
    size: f32,
    line: f32,
}
#[derive(Default)]
struct Paints {
    active: bool,
    rows: Vec<Paint>,
    reference: iced::advanced::text::paragraph::Plain<RenderParagraph>,
}
thread_local! { static PAINTS: RefCell<Paints> = RefCell::new(Paints::default()); }
pub(crate) fn observe_text<'a, M: 'a>(
    id: &'static str,
    kind: TextKind,
    content: impl Into<Element<'a, M>>,
) -> Element<'a, M> {
    let content = content.into();
    if !super::reporting_enabled() {
        return content;
    }
    Element::new(TextBoundary { id, kind, content })
}
struct TextBoundary<'a, M> {
    id: &'static str,
    kind: TextKind,
    content: Element<'a, M>,
}
impl<M> Widget<M, Theme, iced::Renderer> for TextBoundary<'_, M> {
    fn tag(&self) -> widget::tree::Tag {
        self.content.as_widget().tag()
    }
    fn state(&self) -> widget::tree::State {
        self.content.as_widget().state()
    }
    fn diff(&mut self, tree: &mut widget::Tree) {
        self.content.as_widget_mut().diff(tree);
    }
    fn size(&self) -> Size<Length> {
        self.content.as_widget().size()
    }
    fn layout(
        &mut self,
        tree: &mut widget::Tree,
        renderer: &iced::Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        self.content.as_widget_mut().layout(tree, renderer, limits)
    }
    fn update(
        &mut self,
        tree: &mut widget::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &iced::Renderer,
        shell: &mut Shell<'_, M>,
        viewport: &Rectangle,
    ) {
        self.content
            .as_widget_mut()
            .update(tree, event, layout, cursor, renderer, shell, viewport);
    }
    // CLEANUP-IGNORE: Iced requires this draw signature; this observer measures the existing control's painted text.
    fn draw(
        &self,
        tree: &widget::Tree,
        renderer: &mut iced::Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        self.content
            .as_widget()
            .draw(tree, renderer, theme, style, layout, cursor, viewport);
        PAINTS.with_borrow_mut(|paints| {
            if !paints.active || paints.rows.len() >= 9 {
                return;
            }
            use iced::advanced::text::Paragraph as _;
            let label = match self.kind {
                TextKind::Description => layout.bounds(),
                _ => layout.children().nth(1).unwrap().bounds(),
            };
            let (size, line, font, shaping, wrapping, hint_factor) =
                if tree.tag == widget::tree::Tag::of::<widget::text::State<RenderParagraph>>() {
                    let paragraph = tree
                        .state
                        .downcast_ref::<widget::text::State<RenderParagraph>>()
                        .raw();
                    (
                        paragraph.size().0,
                        paragraph.line_height().to_absolute(paragraph.size()).0,
                        paragraph.font(),
                        paragraph.shaping(),
                        paragraph.wrapping(),
                        paragraph.hint_factor(),
                    )
                } else {
                    (
                        0.0,
                        0.0,
                        iced::Font::default(),
                        iced::advanced::text::Shaping::default(),
                        iced::advanced::text::Wrapping::default(),
                        None,
                    )
                };
            paints.rows.push(Paint {
                hint_factor,
                font,
                shaping,
                wrapping,
                id: self.id,
                bounds: layout.bounds(),
                label,
                clip: *viewport,
                size,
                line,
            });
        });
    }
    fn operate(
        &mut self,
        tree: &mut widget::Tree,
        layout: Layout<'_>,
        renderer: &iced::Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        if matches!(self.kind, TextKind::Description) {
            operation.container(Some(&widget::Id::from(self.id)), layout.bounds());
        }
        operation.traverse(&mut |operation| {
            self.content
                .as_widget_mut()
                .operate(tree, layout, renderer, operation)
        });
    }
    fn mouse_interaction(
        &self,
        tree: &widget::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &iced::Renderer,
    ) -> mouse::Interaction {
        self.content
            .as_widget()
            .mouse_interaction(tree, layout, cursor, viewport, renderer)
    }
    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut widget::Tree,
        layout: Layout<'b>,
        renderer: &iced::Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, M, Theme, iced::Renderer>> {
        self.content
            .as_widget_mut()
            .overlay(tree, layout, renderer, viewport, translation)
    }
}

struct Observed<'a> {
    content: Element<'a, RootMessage>,
    key: u16,
    scale: f32,
    native: Option<NativeFrame>,
    frame: RefCell<Frame>,
    sent: Cell<bool>,
}
impl Widget<RootMessage, Theme, iced::Renderer> for Observed<'_> {
    fn size(&self) -> Size<Length> {
        self.content.as_widget().size()
    }
    fn diff(&mut self, tree: &mut widget::Tree) {
        tree.diff_children(std::slice::from_mut(&mut self.content));
    }
    fn layout(
        &mut self,
        tree: &mut widget::Tree,
        renderer: &iced::Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        let child = self
            .content
            .as_widget_mut()
            .layout(&mut tree.children[0], renderer, limits);
        if self.key != 0 && super::reporting_enabled() {
            let mut capture = Capture::new(self.key);
            self.content.as_widget_mut().operate(
                &mut tree.children[0],
                Layout::new(&child),
                renderer,
                &mut capture,
            );
            capture.frame.scale = self.scale;
            *self.frame.borrow_mut() = capture.frame;
        }
        layout::Node::with_children(child.size(), vec![child])
    }
    fn update(
        &mut self,
        tree: &mut widget::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &iced::Renderer,
        shell: &mut Shell<'_, RootMessage>,
        viewport: &Rectangle,
    ) {
        let child = layout.children().next().unwrap();
        self.content.as_widget_mut().update(
            &mut tree.children[0],
            event,
            child,
            cursor,
            renderer,
            shell,
            viewport,
        );
        if self.key != 0
            && super::reporting_enabled()
            && matches!(
                event,
                Event::Window(iced::window::Event::RedrawRequested(_))
            )
        {
            let mut capture = Capture::new(self.key);
            self.content.as_widget_mut().operate(
                &mut tree.children[0],
                child,
                renderer,
                &mut capture,
            );
            capture.frame.scale = self.scale;
            *self.frame.borrow_mut() = capture.frame;
            self.sent.set(false);
        }
    }
    // CLEANUP-IGNORE: Iced requires this draw signature; this observer captures a scoped rendered frame.
    fn draw(
        &self,
        tree: &widget::Tree,
        renderer: &mut iced::Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let observing = self.key != 0 && super::reporting_enabled() && !self.sent.get();
        if observing {
            PAINTS.with_borrow_mut(|paints| {
                paints.active = true;
                paints.rows.clear();
            });
        }
        self.content.as_widget().draw(
            &tree.children[0],
            renderer,
            theme,
            style,
            layout.children().next().unwrap(),
            cursor,
            viewport,
        );
        if observing {
            PAINTS.with_borrow_mut(|paints| paints.active = false);
        }
        if self.key != 0
            && super::reporting_enabled()
            && !self.sent.replace(true)
            && let Some(mut output) = probe::scenario_output()
        {
            let frame = self.frame.borrow();
            if frame.key != self.key {
                return;
            }
            reporting::emit(|sink| {
                for row in &frame.rows {
                    sink.record(
                        "integration.dataset_draw",
                        row.id,
                        &row.text,
                        [
                            row.bounds.x.into(),
                            row.bounds.y.into(),
                            row.bounds.width.into(),
                            row.bounds.height.into(),
                        ],
                    );
                }
                sink.record(
                    "integration.dataset_viewport",
                    "dataset.presentation",
                    "actual-layout",
                    [
                        frame.page.x.into(),
                        frame.page.y.into(),
                        frame.page.width.into(),
                        frame.page.height.into(),
                    ],
                );
                PAINTS.with_borrow_mut(|paints| {
                    for paint in &paints.rows {
                        let rectangle = |rect: Rectangle| {
                            [
                                f64::from(rect.x - frame.translation.x),
                                f64::from(rect.y - frame.translation.y),
                                rect.width.into(),
                                rect.height.into(),
                            ]
                        };
                        sink.record(
                            "integration.dataset_paint",
                            paint.id,
                            "actual-draw",
                            rectangle(paint.bounds),
                        );
                        sink.record(
                            "integration.dataset_label",
                            paint.id,
                            "actual-label-layout",
                            rectangle(paint.label),
                        );
                        sink.record(
                            "integration.dataset_clip",
                            paint.id,
                            "draw-viewport",
                            rectangle(paint.clip),
                        );
                        sink.record(
                            "integration.dataset_font",
                            paint.id,
                            "resolved-paragraph",
                            [paint.size.into(), paint.line.into(), 0.0, 0.0],
                        );
                    }
                    // Re-shape the captured recovery caption with the actual small
                    // paragraph's resolved metrics. This distinguishes wrapped
                    // small text from an oversized font with a similar height.
                    let recovery = paints
                        .rows
                        .iter()
                        .find(|paint| paint.id == "train.dataset.coconut.recover_dropped_masks");
                    let small = paints
                        .rows
                        .iter()
                        .find(|paint| paint.id.ends_with(".description"));
                    if let (Some(recovery), Some(small), Some(caption)) = (
                        recovery,
                        small,
                        frame.row("train.dataset.coconut.recover_dropped_masks"),
                    ) {
                        paints.reference.update(iced::advanced::text::Text {
                            content: caption.text.as_str(),
                            bounds: Size::new(recovery.label.width + 0.1, f32::INFINITY),
                            size: small.size.into(),
                            line_height: iced::advanced::text::LineHeight::Absolute(
                                small.line.into(),
                            ),
                            font: small.font,
                            align_x: iced::advanced::text::Alignment::Default,
                            align_y: iced::alignment::Vertical::Top,
                            shaping: small.shaping,
                            wrapping: small.wrapping,
                            ellipsis: iced::advanced::text::Ellipsis::None,
                            hint_factor: small.hint_factor,
                        });
                        let measured = paints.reference.min_bounds();
                        sink.record(
                            "integration.dataset_label_reference",
                            recovery.id,
                            "resolved-small-paragraph",
                            [
                                measured.width.into(),
                                measured.height.into(),
                                small.size.into(),
                                small.line.into(),
                            ],
                        );
                    }
                });
                if let Some(native) = &self.native {
                    native.report(sink);
                }
                sink.record(
                    "integration.dataset_frame",
                    "dataset.presentation",
                    "actual-draw",
                    [
                        self.key.into(),
                        frame.horizontal.into(),
                        frame.offset.y.into(),
                        f64::from(u8::from(theme.is_dark())),
                    ],
                );
            });
            output.receipt = None;
            let mut drawn = frame.clone();
            drawn.native = self.native.clone();
            PAINTS.with_borrow(|paints| drawn.paints.clone_from(&paints.rows));
            let outline = crate::fluent_theme::container_card(theme).border.color;
            let background = theme.tokens().neutral_background1;
            drawn.colors = [
                outline.r,
                outline.g,
                outline.b,
                outline.a,
                background.r,
                background.g,
                background.b,
            ];
            output.send(Message::DatasetDrawn(drawn));
        }
    }
    fn operate(
        &mut self,
        tree: &mut widget::Tree,
        layout: Layout<'_>,
        renderer: &iced::Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        self.content.as_widget_mut().operate(
            &mut tree.children[0],
            layout.children().next().unwrap(),
            renderer,
            operation,
        );
    }
    fn mouse_interaction(
        &self,
        tree: &widget::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &iced::Renderer,
    ) -> mouse::Interaction {
        self.content.as_widget().mouse_interaction(
            &tree.children[0],
            layout.children().next().unwrap(),
            cursor,
            viewport,
            renderer,
        )
    }
    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut widget::Tree,
        layout: Layout<'b>,
        renderer: &iced::Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, RootMessage, Theme, iced::Renderer>> {
        self.content.as_widget_mut().overlay(
            &mut tree.children[0],
            layout.children().next().unwrap(),
            renderer,
            viewport,
            translation,
        )
    }
}

fn retire(request: u64) {
    #[cfg(target_arch = "wasm32")]
    retire_js(request as f64);
    #[cfg(not(target_arch = "wasm32"))]
    let _ = request;
}

#[cfg(target_arch = "wasm32")]
fn sample(frame: &Frame, request: u64, custody: u8, budget: u16) {
    let bounds = frame
        .row(progress::AREA_ID)
        .map_or(Rectangle::default(), |row| row.bounds);
    let Some(mut output) = probe::scenario_output() else {
        return;
    };
    output.receipt = None;
    let key = frame.key;
    let callback = super::pixel_checks::pixel_result_callback(move |outcome| {
        output.send(Message::DatasetPixels(request, key, outcome));
    });
    let mut rectangles: Vec<f64> = [
        "train.dataset.benchmark_divider",
        "train.dataset.dimensions_divider",
    ]
    .into_iter()
    .filter(|_| !label_probe(key))
    .filter_map(|id| frame.row(id))
    .flat_map(|row| {
        [
            row.bounds.x,
            row.bounds.y,
            row.bounds.width,
            row.bounds.height,
        ]
        .map(|value| f64::from(value * frame.scale))
    })
    .collect();
    let mut labels = Vec::new();
    let mut label_ids = String::new();
    if label_probe(key) {
        for paint in &frame.paints {
            if let Some(bounds) = paint
                .label
                .intersection(&paint.clip)
                .filter(|bounds| bounds.width >= 4.0 && bounds.height >= 4.0)
            {
                if !label_ids.is_empty() {
                    label_ids.push('\n');
                }
                label_ids.push_str(paint.id);
                labels.extend(
                    [
                        bounds.x - frame.translation.x,
                        bounds.y - frame.translation.y,
                        bounds.width,
                        bounds.height,
                    ]
                    .map(|value| f64::from(value * frame.scale)),
                );
            }
        }
    }
    let mut bounds = [
        f64::from(bounds.x * frame.scale),
        f64::from(bounds.y * frame.scale),
        f64::from(bounds.width * frame.scale),
        f64::from(bounds.height * frame.scale),
    ];
    let mut colors = frame.colors.map(f64::from);
    sample_js(
        key,
        &bounds,
        &rectangles,
        &colors,
        frame.scale.into(),
        &label_ids,
        &labels,
        &callback,
        request as f64,
        custody,
        budget,
    );
    if custody == 1 {
        // The FFI borrows ended. Volatile writes make the Release-build reuse
        // exercise observable without mutating through a live shared borrow.
        for value in bounds
            .iter_mut()
            .chain(&mut rectangles)
            .chain(&mut colors)
            .chain(&mut labels)
        {
            unsafe {
                std::ptr::write_volatile(value, -1000.0);
            }
        }
        custody_js(1);
    } else if matches!(custody, 4 | 5) {
        custody_js(custody);
    }
}
#[cfg(not(target_arch = "wasm32"))]
fn sample(_frame: &Frame, _request: u64, _custody: u8, _budget: u16) {}
#[cfg(target_arch = "wasm32")]
#[wasm_bindgen::prelude::wasm_bindgen(module = "/src/integration_control/browser.mjs")]
unsafe extern "C" {
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationDatasetPixels)]
    fn sample_js(
        key: u16,
        bounds: &[f64],
        dividers: &[f64],
        colors: &[f64],
        scale: f64,
        label_ids: &str,
        labels: &[f64],
        callback: &wasm_bindgen::JsValue,
        request: f64,
        custody: u8,
        budget: u16,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationDatasetCustody)]
    fn custody_js(action: u8);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationDatasetRetire)]
    fn retire_js(request: f64);
}

#[cfg(target_arch = "wasm32")]
pub(super) fn input(action: u8, next: u8, bounds: Rectangle, scale: f32, value: &str) {
    let Some(mut output) = probe::scenario_output() else {
        return;
    };
    output.receipt = None;
    let callback = wasm_bindgen::closure::Closure::once_into_js(move |valid: bool| {
        output.send(Message::DatasetInputDelivered(next, valid));
    });
    input_js(
        action,
        &[
            f64::from(bounds.x * scale),
            f64::from(bounds.y * scale),
            f64::from(bounds.width * scale),
            f64::from(bounds.height * scale),
        ],
        value,
        &callback,
    );
}
#[cfg(not(target_arch = "wasm32"))]
pub(super) fn input(_action: u8, _next: u8, _bounds: Rectangle, _scale: f32, _value: &str) {}
#[cfg(target_arch = "wasm32")]
#[wasm_bindgen::prelude::wasm_bindgen(module = "/src/integration_control/browser.mjs")]
unsafe extern "C" {
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationDatasetInput)]
    fn input_js(action: u8, bounds: &[f64], value: &str, callback: &wasm_bindgen::JsValue);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn stable(state: &mut State, frame: &Frame) -> (u64, u16) {
        for _ in 0..4 {
            state.observe(frame.clone());
        }
        state.pixel_pending.expect("one current capture")
    }

    #[test]
    fn dataset_callbacks_retain_scenario_and_request_ownership() {
        use crate::integration_control::{Phase, ProbeFixture};

        let mut fixture = ProbeFixture::new("retained");
        let controller = &mut fixture.controller;
        let generation = controller.driver.generation;
        let scoped = |generation, message| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(message),
        };
        controller.driver.phase = Phase::DatasetInput(0);
        controller.lifecycle.presentation.input_pending = true;
        let _ = controller.update(scoped(
            generation.wrapping_sub(1),
            Message::DatasetInputDelivered(1, true),
        ));
        assert_eq!(controller.driver.phase, Phase::DatasetInput(0));
        assert!(controller.lifecycle.presentation.input_pending);
        let _ = controller.update(scoped(generation, Message::DatasetInputDelivered(1, true)));
        assert_eq!(controller.driver.phase, Phase::DatasetInput(1));
        assert!(!controller.lifecycle.presentation.input_pending);

        controller.driver.phase = Phase::DatasetFixture(1);
        let frame = Frame {
            key: 201,
            ..Frame::default()
        };
        let request = stable(&mut controller.lifecycle.presentation, &frame);
        for (scope, request_id) in [
            (generation.wrapping_sub(1), request.0),
            (generation, request.0.wrapping_add(1)),
        ] {
            let _ = controller.update(scoped(
                scope,
                Message::DatasetPixels(request_id, request.1, ProbeOutcome::Failed),
            ));
            assert_eq!(controller.driver.phase, Phase::DatasetFixture(1));
            assert_eq!(
                controller.lifecycle.presentation.pixel_pending,
                Some(request)
            );
        }
        let _ = controller.update(scoped(
            generation,
            Message::DatasetPixels(request.0, request.1, ProbeOutcome::Observed(0, 0)),
        ));
        assert_eq!(controller.lifecycle.presentation.pixels, Some(frame.key));
        let _ = controller.update(scoped(
            generation,
            Message::DatasetPixels(request.0, request.1, ProbeOutcome::Failed),
        ));
        assert_eq!(controller.driver.phase, Phase::DatasetFixture(1));
        assert_eq!(controller.lifecycle.presentation.pixels, Some(frame.key));
    }

    #[test]
    fn capture_changes_retire_pending_and_successful_observations() {
        let original = Frame {
            key: 200,
            scale: 1.0,
            ..Frame::default()
        };
        for change in 0..9 {
            let mut state = State::default();
            let old = stable(&mut state, &original);
            let mut changed = original.clone();
            match change {
                0 => changed.scale = 2.0,
                1 => changed.colors[0] = 0.5,
                2 => changed.page.width = 100.0,
                3 => changed.translation.y = 10.0,
                4 => changed.offset.y = 10.0,
                5 => changed.key += 1,
                6 => changed.rows.push(Row {
                    id: "test",
                    text: "new".into(),
                    ..Row::default()
                }),
                _ => changed.paints.push(Paint {
                    hint_factor: None,
                    font: iced::Font::default(),
                    shaping: iced::advanced::text::Shaping::default(),
                    wrapping: iced::advanced::text::Wrapping::default(),
                    id: "test",
                    bounds: Rectangle::default(),
                    label: Rectangle::default(),
                    clip: Rectangle {
                        width: change as f32,
                        ..Rectangle::default()
                    },
                    size: 12.0,
                    line: 14.0,
                }),
            }
            state.observe(changed.clone());
            assert!(state.pixel_pending.is_none());
            assert!(!state.complete(old.0, old.1, ProbeOutcome::Failed));
            let current = stable(&mut state, &changed);
            assert_ne!(old, current);
            assert!(!state.complete(old.0, old.1, ProbeOutcome::Observed(0, 0)));
            assert_eq!(state.pixel_pending, Some(current));
            assert!(!state.complete(current.0, current.1, ProbeOutcome::Observed(0, 0)));
            assert_eq!(state.pixels, Some(changed.key));
            assert!(!state.complete(current.0, current.1, ProbeOutcome::Failed));
            assert_eq!(state.pixels, Some(changed.key));
            state.observe(changed);
            assert!(state.pixel_pending.is_none());
            state.observe(original.clone());
            assert_eq!(state.pixels, None);
        }
    }

    #[test]
    fn packaged_custody_preserves_fixture_key_and_rejects_superseded_completion() {
        let mut state = State::default();
        let frame = Frame {
            key: 200,
            ..Frame::default()
        };
        let initial = stable(&mut state, &frame);
        assert!(!state.complete(initial.0, 200, ProbeOutcome::Observed(0, 0)));
        assert!(!state.custody_complete());
        let copied = stable(&mut state, &frame);
        assert_eq!(state.custody, 1);
        assert!(!state.complete(copied.0, 200, ProbeOutcome::Observed(0, 0)));
        assert!(!state.custody_complete());
        let replacement = stable(&mut state, &frame);
        assert_eq!(state.custody, 3);
        assert!(!state.complete(replacement.0 - 1, 200, ProbeOutcome::Failed));
        assert_eq!(state.pixel_pending, Some(replacement));
        assert!(!state.complete(replacement.0, 200, ProbeOutcome::Observed(0, 0)));
        assert!(!state.custody_complete());
        for stage in [4, 5] {
            let request = stable(&mut state, &frame);
            assert_eq!(state.custody, stage);
            assert!(!state.complete(request.0, 200, ProbeOutcome::Invalidated));
            assert_eq!(state.pixels, None);
        }
        let current = stable(&mut state, &frame);
        assert!(!state.complete(current.0, 200, ProbeOutcome::Observed(0, 0)));
        assert!(state.custody_complete());
        assert_eq!(state.pixels, Some(200));
        assert_eq!(state.custody, 7);
    }

    #[test]
    fn invalidation_rearms_after_fresh_draws_but_failure_is_terminal() {
        let mut state = State::default();
        let frame = Frame {
            key: 200,
            ..Frame::default()
        };
        let request = stable(&mut state, &frame);
        let frames = state.frames;
        assert!(!state.complete(request.0, request.1, ProbeOutcome::Invalidated));
        assert_eq!(state.stable, 0);
        for _ in 0..2 {
            state.observe(frame.clone());
        }
        assert!(state.pixel_pending.is_none());
        state.observe(frame.clone());
        let next = state.pixel_pending.unwrap();
        assert!(next.0 > request.0);
        assert_eq!(state.frames, frames + 3);
        assert!(!state.complete(request.0, request.1, ProbeOutcome::Invalidated));
        assert_eq!(state.pixel_pending, Some(next));
        assert!(state.complete(next.0, next.1, ProbeOutcome::Failed));
        assert!(!state.complete(next.0, next.1, ProbeOutcome::Observed(0, 0)));
        for _ in 0..4 {
            state.observe(frame.clone());
        }
        assert!(state.pixel_pending.is_none());
        assert_eq!(state.pixels, None);
    }
}
