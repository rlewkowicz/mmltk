pub mod dataset;
pub mod detail;
pub mod details;
pub mod gallery;
pub mod overlay;
pub mod state;

use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use crate::view_model::{ApplicationModel, ExploreModel};
use iced::widget::scrollable::{Direction, Scrollbar};
use iced::widget::{column, container, row, scrollable, stack, text};
use iced::{Fill, Length, Padding};

pub const DATASET_PANE_ID: &str = "explore.pane.dataset_filters";
pub const DETAILS_PANE_ID: &str = "explore.pane.dataset_details";
pub const DATASET_SCROLL_ID: &str = "explore.pane.dataset_filters.scroll";
pub const DETAILS_SCROLL_ID: &str = "explore.pane.dataset_details.scroll";
pub const OPEN_ID: &str = "explore.open";
pub const STOP_ID: &str = "explore.stop";
pub const STATUS_CARD_ID: &str = "explore.card.status";
pub const GALLERY_WORKSPACE_ID: &str = "explore.gallery.workspace";
pub const GALLERY_CAPACITY_ID: &str = "explore.gallery.capacity";
pub const GALLERY_EMPTY_ID: &str = "explore.gallery.empty";
pub const GALLERY_LABELS_ID: &str = "explore.gallery.labels";
pub const GALLERY_MASKS_ID: &str = "explore.gallery.masks";
pub const GALLERY_BOXES_ID: &str = "explore.gallery.boxes";
pub const GALLERY_LATER_ID: &str = "explore.gallery.later";
pub const AUGMENTATION_TOGGLE_ID: &str = "explore.gallery.augmentation";
pub const AUGMENTATION_SEED_ID: &str = "explore.gallery.augmentation.seed";
pub const AUGMENTATION_REROLL_ID: &str = "explore.gallery.augmentation.reroll";
pub const RESHUFFLE_ID: &str = "explore.gallery.reshuffle";
pub const DETAIL_WORKSPACE_ID: &str = "explore.detail.workspace";
pub const DETAIL_NEXT_ID: &str = "explore.detail.next";
pub const DETAIL_PREVIOUS_ID: &str = "explore.detail.previous";
pub const DETAIL_CLOSE_ID: &str = "explore.detail.close";
pub const DETAIL_ANNOTATE_ID: &str = "explore.detail.open_annotation";
pub const DETAIL_ORIGINAL_ID: &str = "explore.detail.source.original";
pub const DETAIL_UPSCALE_BASIC_ID: &str = "explore.detail.upscale.basic";
pub const DETAIL_UPSCALE_FAST_ID: &str = "explore.detail.upscale.fast";
pub const DETAIL_UPSCALE_NEURAL_ID: &str = "explore.detail.upscale.neural";

pub const DETAIL_FIT_ID: &str = "explore.detail.fit";
pub const DETAIL_BOXES_ID: &str = "explore.detail.boxes";
pub const DETAIL_MASKS_ID: &str = "explore.detail.masks";
pub const DETAIL_LABELS_ID: &str = "explore.detail.labels";
pub const CUSTOM_SOURCE_ID: &str = "explore.dataset.custom_source";
pub const CUSTOM_BROWSE_ID: &str = "explore.dataset.custom_browse";
pub const ORDER_CONTROL_ID: &str = "explore.dataset.order";
pub const RANGE_CONTROL_ID: &str = "explore.dataset.compiled_range";
pub const OVERLAY_CLASSES_ID: &str = "explore.details.overlay_classes";
pub const ORDER_SHUFFLED_ID: &str = "explore.dataset.order.shuffled";
pub const RANGE_START_ONE_ID: &str = "explore.dataset.range.start_one";
pub const OVERLAY_NONE_ID: &str = "explore.details.overlay.none";

pub const LEFT_PANE_WIDTH: f32 = 280.0;
pub const RIGHT_PANE_WIDTH: f32 = 300.0;
pub const PANE_GAP: f32 = 12.0;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PaneLayout {
    pub dataset: f32,
    pub center: f32,
    pub details: f32,
    pub gap: f32,
}

pub fn pane_layout(width: f32) -> PaneLayout {
    PaneLayout {
        dataset: LEFT_PANE_WIDTH,
        center: (width - LEFT_PANE_WIDTH - RIGHT_PANE_WIDTH - PANE_GAP * 2.0).max(360.0),
        details: RIGHT_PANE_WIDTH,
        gap: PANE_GAP,
    }
}

#[derive(Debug, Clone)]
pub enum Message {
    Dataset(dataset::Message),
    Gallery(gallery::Message),
    Detail(detail::Message),
    Details(details::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    OpenRequested,
    StopRequested,
    DialogRequested(u64),
    SettingsEdited(EditSchedule),
    OpenAnnotationRequested,
    UpscaleRequested(crate::generated::UpscaleKernel),
    FilterEdited(crate::generated::ExploreFilterUpdate),
    RerollRequested,
    AugmentationUpdated(crate::generated::ExploreAugmentationUpdate),
    AugmentationRerollRequested,
    DetailUpdated(crate::generated::ExploreDetailUpdate),
    OverlayUpdated(crate::generated::ExploreOverlay),
    ScrollRequested(f32),
    ImageSelected(u32),
    PreviousRequested,
    NextRequested,
    CloseDetailRequested,
    ViewportChanged(crate::generated::ExploreViewportUpdate),
}

#[derive(Default)]
pub struct Component {
    pub(crate) input: crate::workspace_input::Binding,
    state: state::State,
}

impl Component {
    #[cfg(test)]
    pub(crate) fn state_for_test(&mut self) -> &mut state::State {
        &mut self.state
    }

    pub fn rebase(&mut self, model: &ApplicationModel) {
        self.state.rebase(model.explore.snapshot.as_ref(), false);
    }

    pub fn bootstrap(&mut self, model: &ApplicationModel) {
        self.state.rebase(model.explore.snapshot.as_ref(), true);
    }

    pub fn record_submission(&mut self, request: crate::generated::ExploreFilterUpdate) {
        self.state.record_submission(request);
    }

    pub fn record_admission(&mut self, revision: u64) {
        self.state.record_admission(revision);
    }

    pub fn abandon_submission(&mut self) {
        self.state.abandon_submission();
    }

    pub fn submit_detail(&mut self, original: bool) {
        self.state.submit_detail(original);
    }

    pub fn settle_detail(&mut self, accepted: bool) {
        self.state.settle_detail(accepted);
    }

    pub fn request_viewport(
        &mut self,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        request: crate::generated::ExploreViewportUpdate,
    ) {
        self.state.request_viewport(snapshot, request);
    }

    pub fn dispatchable_viewport(&self) -> Option<crate::generated::ExploreViewportUpdate> {
        self.state.dispatchable_viewport()
    }

    pub fn viewport_queued(&mut self, request: crate::generated::ExploreViewportUpdate) {
        self.state.viewport_queued(request);
    }

    pub fn arm_viewport_writable_wait(&mut self) -> bool {
        self.state.arm_viewport_writable_wait()
    }

    pub fn viewport_writable(&mut self) {
        self.state.viewport_writable();
    }

    pub fn clear_viewport_admission(&mut self) {
        self.state.clear_viewport_admission();
    }

    pub fn measured_viewport(
        &self,
        columns: u32,
        first_row: u32,
        matching_count: u32,
    ) -> Option<crate::generated::ExploreViewport> {
        self.state
            .measured_viewport(columns, first_row, matching_count)
    }

    pub(crate) fn gallery_size(&self) -> Option<iced::Size> {
        self.state.gallery_size()
    }

    #[cfg(test)]
    pub fn measure_gallery(
        &mut self,
        width: f32,
        height: f32,
        maximum_extent: crate::generated::VisualExtent,
        columns: u32,
    ) -> bool {
        self.state
            .measure_gallery(width, height, maximum_extent, columns)
    }

    pub fn measured_layout_request(
        &self,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        columns: u32,
        matching_count: u32,
    ) -> Option<crate::generated::ExploreViewportUpdate> {
        self.state
            .measured_layout_request(snapshot, columns, matching_count)
    }

    pub fn update(
        &mut self,
        application: &mut ApplicationModel,
        settings: &mut SettingsModel,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        let model = &mut application.explore;
        let outcome = match message {
            Message::Dataset(message) => {
                match dataset::update(&self.state, model, settings, message)? {
                    dataset::Outcome::OpenRequested => Outcome::OpenRequested,
                    dataset::Outcome::StopRequested => Outcome::StopRequested,
                    dataset::Outcome::DialogRequested(id) => Outcome::DialogRequested(id),
                    dataset::Outcome::SettingsEdited(schedule) => Outcome::SettingsEdited(schedule),
                    dataset::Outcome::FilterEdited(request) => Outcome::FilterEdited(request),
                }
            }
            Message::Gallery(message) => {
                match gallery::update(&mut self.state, model.snapshot.as_ref(), settings, message)?
                {
                    None => return Ok(None),
                    Some(outcome) => match outcome {
                        gallery::Outcome::ScrollRequested(offset) => {
                            Outcome::ScrollRequested(offset)
                        }
                        gallery::Outcome::OverlayUpdated(overlay) => {
                            Outcome::OverlayUpdated(overlay)
                        }
                        gallery::Outcome::RerollRequested => Outcome::RerollRequested,
                        gallery::Outcome::AugmentationUpdated(request) => {
                            Outcome::AugmentationUpdated(request)
                        }
                        gallery::Outcome::AugmentationRerollRequested => {
                            Outcome::AugmentationRerollRequested
                        }
                        gallery::Outcome::ImageSelected(index) => Outcome::ImageSelected(index),
                        gallery::Outcome::ViewportChanged(viewport) => {
                            Outcome::ViewportChanged(viewport)
                        }
                        gallery::Outcome::SettingsEdited(schedule) => {
                            Outcome::SettingsEdited(schedule)
                        }
                    },
                }
            }
            Message::Detail(message) => match detail::update(&mut self.state, model, message)? {
                None => return Ok(None),
                Some(outcome) => match outcome {
                    detail::Outcome::PreviousRequested => Outcome::PreviousRequested,
                    detail::Outcome::NextRequested => Outcome::NextRequested,
                    detail::Outcome::CloseRequested => Outcome::CloseDetailRequested,
                    detail::Outcome::OpenAnnotationRequested => Outcome::OpenAnnotationRequested,
                    detail::Outcome::UpscaleRequested(kernel) => Outcome::UpscaleRequested(kernel),
                    detail::Outcome::DetailUpdated(request) => Outcome::DetailUpdated(request),
                    detail::Outcome::OverlayUpdated(overlay) => Outcome::OverlayUpdated(overlay),
                },
            },
            Message::Details(message) => match details::update(&self.state, model, message)? {
                details::Outcome::FilterEdited(request) => Outcome::FilterEdited(request),
            },
        };
        Ok(Some(outcome))
    }

    pub fn view<'a>(
        &'a self,
        model: &'a ApplicationModel,
        settings: &'a SettingsModel,
        surface: Option<Surface>,
        width: f32,
    ) -> Element<'a, Message> {
        view(
            &self.state,
            model,
            settings,
            surface,
            width,
            self.input.clone(),
        )
    }
}

pub fn settings_edit(
    settings: &mut SettingsModel,
    apply: impl FnOnce(&mut crate::generated::GuiSettingsState) -> crate::generated::SettingsValueUpdate,
) -> Result<EditSchedule, String> {
    settings.edit(EditCadence::Debounced, apply)
}

pub fn filter_edit(
    state: &state::State,
    model: &ExploreModel,
    apply: impl FnOnce(&mut crate::generated::ExploreFilterUpdate),
) -> Result<crate::generated::ExploreFilterUpdate, String> {
    let mut request = state
        .presented_filter(model.snapshot.as_ref())
        .ok_or_else(|| "Explore filter settings are not installed".to_owned())?
        .to_owned();
    apply(&mut request);
    Ok(request)
}

pub(super) fn sidebar<'a, Message: 'a>(
    scroll_id: &'static str,
    title: &'static str,
    content: impl Into<Element<'a, Message>>,
) -> Element<'a, Message> {
    container(column![
        container(text(title).size(18))
            .padding(Padding::from([12, 12]))
            .width(Fill)
            .style(crate::fluent_theme::container_header),
        scrollable(container(content).padding(12).width(Fill))
            .id(scroll_id)
            .direction(Direction::Vertical(
                Scrollbar::new().width(5).scroller_width(5),
            ))
            .style(crate::fluent_theme::scrollable_default)
            .height(Fill),
    ])
    .width(Fill)
    .height(Fill)
    .style(crate::fluent_theme::container_card)
    .into()
}

pub fn view<'a>(
    state: &'a state::State,
    model: &'a ApplicationModel,
    settings: &'a SettingsModel,
    surface: Option<Surface>,
    width: f32,
    input: crate::workspace_input::Binding,
) -> Element<'a, Message> {
    let layout = pane_layout(width);
    let center_width = layout.center;
    let paired = crate::presentation_surface::explore_display(surface);
    let gallery = match &paired {
        Some(crate::presentation_surface::ExploreDisplay::Gallery(surface, metadata)) => {
            Some((*surface, metadata.clone()))
        }
        _ => None,
    };
    let content = row![
        container(dataset::view(state, model, settings).map(Message::Dataset))
            .id(DATASET_PANE_ID)
            .width(Length::Fixed(layout.dataset))
            .height(Fill),
        gallery::view(state, model, settings, gallery, center_width, input.clone())
            .map(Message::Gallery),
        container(details::view(state, model, settings).map(Message::Details))
            .id(DETAILS_PANE_ID)
            .width(Length::Fixed(layout.details))
            .height(Fill),
    ]
    .spacing(layout.gap)
    .width(Length::Fixed(width))
    .height(Fill);

    let detail = match paired {
        Some(crate::presentation_surface::ExploreDisplay::Detail(surface, content)) => Some(
            detail::view(state, model, settings, surface, content, input.clone())
                .map(Message::Detail),
        ),
        _ => None,
    };
    let mut layers: Vec<Element<'a, Message>> = Vec::with_capacity(2);
    layers.push(
        container(content)
            .width(Length::Fixed(width))
            .height(Fill)
            .into(),
    );
    layers.extend(detail);
    stack(layers)
        .width(Length::Fixed(width))
        .height(Fill)
        .into()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn graphics_first_detail_composes_with_its_paired_identity_without_logical_detail() {
        for logical in 0..3 {
            crate::presentation_surface::reset_test_releases();
            let (mut model, frame) = crate::view_model::test_support::explore_presentation();
            let expected = model.explore.snapshot.clone().unwrap();
            assert!(crate::presentation_surface::accept_publication(frame));
            let surface = crate::presentation_surface::metadata::surface(frame).unwrap();
            match logical {
                0 => model.explore.snapshot = None,
                1 => {
                    model.explore.snapshot.as_mut().unwrap().mode =
                        crate::generated::ExploreMode::Gallery
                }
                _ => {
                    let changed = model.explore.snapshot.as_mut().unwrap();
                    changed.selectedimage = Some(99);
                    changed.detail.showoriginaldimensions = !expected.detail.showoriginaldimensions;
                    changed.overlay.showlabels = !expected.overlay.showlabels;
                }
            }
            let Some(crate::presentation_surface::ExploreDisplay::Detail(shown, content)) =
                crate::presentation_surface::explore_display(Some(surface))
            else {
                panic!("accepted detail selects its physical composition");
            };
            assert_eq!(shown.frame, Some(frame));
            assert_eq!(
                content.viewer_identity(),
                expected
                    .selectedimage
                    .map(|image| (expected.dataset.identity, image))
            );
            assert_eq!(content.overlay(), &expected.overlay);
            assert_eq!(content.frame(), &expected.frame);
            let state = state::State::default();
            assert_eq!(
                state.detail_original(&content),
                expected.detail.showoriginaldimensions
            );
            let settings = SettingsModel::default();
            drop(view(
                &state,
                &model,
                &settings,
                Some(surface),
                1200.0,
                crate::workspace_input::Binding::default(),
            ));
            crate::presentation_surface::retire_publication(frame);
            assert_eq!(crate::presentation_surface::test_releases(), vec![frame]);
        }
    }

    #[test]
    fn specialized_explore_regions_keep_fixed_sidebars() {
        assert_eq!(LEFT_PANE_WIDTH, 280.0);
        assert_eq!(RIGHT_PANE_WIDTH, 300.0);
        assert_eq!(
            1200.0 - LEFT_PANE_WIDTH - RIGHT_PANE_WIDTH - PANE_GAP * 2.0,
            596.0
        );
        assert_eq!(
            pane_layout(1200.0),
            PaneLayout {
                dataset: 280.0,
                center: 596.0,
                details: 300.0,
                gap: 12.0,
            }
        );
        assert_eq!(
            pane_layout(1500.0),
            PaneLayout {
                dataset: 280.0,
                center: 896.0,
                details: 300.0,
                gap: 12.0,
            }
        );
    }
}
