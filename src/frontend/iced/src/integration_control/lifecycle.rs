use crate::generated::FeatureId;
#[cfg(target_arch = "wasm32")]
use crate::integration_control::slider_drag_js;
use crate::integration_control::widget_ops::{AnnotationReveal, reveal_control};
use crate::integration_control::widget_ops::{click, click_number_edge, wheel_number_input};
use crate::integration_control::{
    Driver, Phase, reporting, route_edit_available, settled_settings_snapshot, ui_scale_evidence,
    widget_ops,
};
use crate::message::Message as RootMessage;
use crate::view::train;
use crate::view_model::ApplicationModel;
use iced::widget::operation::RelativeOffset;
use iced::{Rectangle, Task};

/// Mutable observations owned by this scenario or mechanism.
pub(super) struct State {
    spinner_baseline: f64,
    show_fps_baseline: bool,
    benchmark_baseline: bool,
    benchmark_selection: Option<crate::generated::BenchmarkDatasetSelection>,
    benchmark_dialog: Option<crate::generated::FileDialogSnapshot>,
    benchmark_source: String,
    perceptual_baseline: [bool; 3],
    settings_revision: u64,
    numeric_target: f64,
    numeric_selection_length: usize,
    denoising_target: bool,
    ui_scale_baseline: f32,
    ui_scale_first: Option<f32>,
    primary_pixels: std::collections::BTreeSet<String>,
}
impl Default for State {
    fn default() -> Self {
        Self {
            spinner_baseline: 0.0,
            show_fps_baseline: false,
            benchmark_baseline: false,
            benchmark_selection: None,
            benchmark_dialog: None,
            benchmark_source: String::new(),
            perceptual_baseline: [false; 3],
            settings_revision: 0,
            numeric_target: 0.0,
            numeric_selection_length: 0,
            denoising_target: false,
            ui_scale_baseline: 1.0,
            ui_scale_first: None,
            primary_pixels: std::collections::BTreeSet::new(),
        }
    }
}

impl State {
    fn benchmark_choice(
        &self,
        index: usize,
    ) -> (
        &'static str,
        bool,
        crate::generated::BenchmarkDatasetSelection,
    ) {
        use crate::generated::{
            BenchmarkDatasetVariant as Dataset, CoconutValidation as Validation,
        };
        let mut selection = self
            .benchmark_selection
            .as_ref()
            .expect("benchmark baseline captured")
            .clone();
        let mut enabled = true;
        let control = match index {
            0 => BENCHMARK_OVERRIDE,
            1 => {
                selection.dataset = Dataset::Coconut;
                train::BENCHMARK_COCONUT_ID
            }
            2 => {
                selection.dataset = Dataset::Coconut;
                selection.validation = Validation::Stock;
                train::STOCK_VALIDATION_ID
            }
            3 => {
                selection.dataset = Dataset::Coconut;
                selection.validation = Validation::CoconutStock;
                train::COCONUT_STOCK_ID
            }
            4 => {
                selection.dataset = Dataset::Coconut;
                selection.validation = Validation::Coconut;
                train::COCONUT_VALIDATION_ID
            }
            5 => {
                selection.dataset = Dataset::CocoCustom;
                selection.validation = Validation::Coconut;
                train::BENCHMARK_CUSTOM_ID
            }
            6 => {
                selection.dataset = Dataset::Coconut;
                selection.validation = Validation::Coconut;
                train::BENCHMARK_COCONUT_ID
            }
            7 => {
                selection.dataset = Dataset::Coconut;
                selection.validation = Validation::Coconut;
                DATASET_BROWSE
            }
            8 => {
                selection.dataset = Dataset::Coconut;
                match selection.validation {
                    Validation::Coconut => train::COCONUT_VALIDATION_ID,
                    Validation::Stock => train::STOCK_VALIDATION_ID,
                    Validation::CoconutStock => train::COCONUT_STOCK_ID,
                }
            }
            9 => match selection.dataset {
                Dataset::CocoCustom => train::BENCHMARK_CUSTOM_ID,
                Dataset::Coconut => train::BENCHMARK_COCONUT_ID,
            },
            10 => {
                enabled = self.benchmark_baseline;
                BENCHMARK_OVERRIDE
            }
            _ => {
                enabled = false;
                BENCHMARK_OVERRIDE
            }
        };
        (control, enabled, selection)
    }

    pub(super) fn primary_action_pixels(&mut self, control: &str, active: bool) {
        if !active && self.primary_pixels.len() < 6 {
            self.primary_pixels.insert(control.to_owned());
        }
    }

    fn after_page(page: FeatureId) -> Phase {
        if page == FeatureId::Train {
            Phase::TrainModelCard
        } else {
            crate::view::workflow::Composition::new(page, 0.0)
                .next_ordinary_page()
                .map_or(Phase::ReturnTrain, Phase::PageNavigation)
        }
    }

    pub(super) fn begin_advanced_numeric_edit(
        &mut self,
        driver: &mut Driver,
        widgets: &mut widget_ops::RevealState,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        index: usize,
    ) -> Task<RootMessage> {
        let Some((target, replacement, selection_length)) = numeric_edit_target(settings, index)
        else {
            driver.fail("representative numeric field has no generated-valid alternate value");
            return Task::none();
        };
        self.numeric_target = target;
        widgets.replace_numeric_value(replacement);
        self.numeric_selection_length = selection_length;
        self.settings_revision = model
            .settings_snapshot
            .as_ref()
            .map_or(0, |snapshot| snapshot.revision);
        driver.phase = Phase::AdvancedNumericEdit(index);
        widgets.arm_scrolled(driver, advanced_field_id(index), RelativeOffset::END)
    }
    pub(super) fn advance_lifecycle(
        &mut self,
        driver: &mut Driver,
        widgets: &mut widget_ops::RevealState,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        applied_scale: f32,
        active: FeatureId,
    ) -> Task<RootMessage> {
        match driver.phase.clone() {
            Phase::SettingsOpen => widgets.arm(driver, "navigation.settings"),
            Phase::AwaitSettings if settings.open => {
                driver.phase = Phase::SettingsModal;
                widgets.arm(driver, SETTINGS_MODAL)
            }
            Phase::SettingsModal => widgets.arm(driver, SETTINGS_MODAL),
            Phase::SettingsGroup(index) => widgets.arm(driver, SETTINGS_GROUPS[index]),
            Phase::SettingsScaleDrag => {
                self.ui_scale_baseline = applied_scale;
                self.ui_scale_first = None;
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                widgets.arm(driver, SETTINGS_NUMERIC_CONTROLS[0])
            }
            Phase::AwaitSettingsScaleDrag => {
                let Some(current) = settings.draft.as_ref().map(|draft| draft.ui.uiscale) else {
                    return Task::none();
                };
                if same_numeric_value(f64::from(current), f64::from(self.ui_scale_baseline)) {
                    return Task::none();
                }
                if !same_numeric_value(f64::from(applied_scale), f64::from(self.ui_scale_baseline))
                {
                    driver.fail("UI scale changed before the pointer was released");
                    return Task::none();
                }
                if self.ui_scale_first.is_none() {
                    self.ui_scale_first = Some(current);
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.ui_scale_drag",
                            SETTINGS_NUMERIC_CONTROLS[0],
                            "first-position",
                            [
                                f64::from(self.ui_scale_baseline),
                                f64::from(current),
                                f64::from(applied_scale),
                                1.0,
                            ],
                        )
                    });
                    return Task::none();
                }
                if self
                    .ui_scale_first
                    .is_some_and(|first| same_numeric_value(f64::from(first), f64::from(current)))
                {
                    return Task::none();
                }
                if current <= 0.85 {
                    driver.fail("UI-scale drag did not move the draft beyond 0.85");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.ui_scale_drag",
                        SETTINGS_NUMERIC_CONTROLS[0],
                        "second-position",
                        [
                            f64::from(self.ui_scale_baseline),
                            f64::from(current),
                            f64::from(applied_scale),
                            2.0,
                        ],
                    )
                });
                driver.phase = Phase::AwaitSettingsScaleRelease;
                Task::none()
            }
            Phase::AwaitSettingsScaleRelease => {
                let Some(current) = settings.draft.as_ref().map(|draft| draft.ui.uiscale) else {
                    return Task::none();
                };
                if !same_numeric_value(f64::from(applied_scale), f64::from(current)) {
                    return Task::none();
                }
                driver.phase = Phase::AwaitSettingsScaleSnapshot;
                Task::none()
            }
            Phase::AwaitSettingsScaleSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.uiscale > 0.85
                            && same_numeric_value(
                                f64::from(snapshot.settingsstate.ui.uiscale),
                                f64::from(applied_scale),
                            )
                    }) =>
            {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.ui_scale_drag",
                        SETTINGS_NUMERIC_CONTROLS[0],
                        "released-and-settled",
                        ui_scale_evidence(model, settings, self.ui_scale_baseline, applied_scale),
                    )
                });
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(self.settings_revision, |snapshot| snapshot.revision);
                driver.phase = Phase::AwaitSettingsScaleRestoreDraft;
                Task::done(RootMessage::Settings(
                    crate::view::settings::Message::UiScaleChanged(self.ui_scale_baseline),
                ))
            }
            Phase::AwaitSettingsScaleRestoreDraft
                if settings.draft.as_ref().is_some_and(|draft| {
                    same_numeric_value(
                        f64::from(draft.ui.uiscale),
                        f64::from(self.ui_scale_baseline),
                    )
                }) =>
            {
                driver.phase = Phase::AwaitSettingsScaleRestored;
                Task::done(RootMessage::Settings(
                    crate::view::settings::Message::UiScaleReleased,
                ))
            }
            Phase::AwaitSettingsScaleRestored
                if same_numeric_value(
                    f64::from(applied_scale),
                    f64::from(self.ui_scale_baseline),
                ) && settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        same_numeric_value(
                            f64::from(snapshot.settingsstate.ui.uiscale),
                            f64::from(self.ui_scale_baseline),
                        )
                    }) =>
            {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.ui_scale_restored",
                        SETTINGS_NUMERIC_CONTROLS[0],
                        "baseline",
                        ui_scale_evidence(model, settings, self.ui_scale_baseline, applied_scale),
                    )
                });
                driver.phase = Phase::SettingsShowFps;
                widgets.arm(driver, SETTINGS_SHOW_FPS)
            }
            Phase::SettingsShowFps => {
                self.show_fps_baseline = settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| draft.ui.showworkspaceperformance);
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                widgets.arm(driver, SETTINGS_SHOW_FPS)
            }
            Phase::AwaitSettingsShowFps
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.ui.showworkspaceperformance != self.show_fps_baseline
                }) =>
            {
                driver.phase = Phase::AwaitSettingsShowFpsChangedSnapshot;
                Task::none()
            }
            Phase::AwaitSettingsShowFpsChangedSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.showworkspaceperformance != self.show_fps_baseline
                    }) =>
            {
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(self.settings_revision, |snapshot| snapshot.revision);
                driver.phase = Phase::SettingsRestoreShowFps;
                widgets.arm(driver, SETTINGS_SHOW_FPS)
            }
            Phase::SettingsRestoreShowFps => widgets.arm(driver, SETTINGS_SHOW_FPS),
            Phase::AwaitSettingsShowFpsRestored
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.ui.showworkspaceperformance == self.show_fps_baseline
                }) =>
            {
                driver.phase = Phase::AwaitSettingsShowFpsSnapshot;
                Task::none()
            }
            Phase::AwaitSettingsShowFpsSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.showworkspaceperformance == self.show_fps_baseline
                    }) =>
            {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.show_fps",
                        SETTINGS_SHOW_FPS,
                        "round-trip",
                        [
                            1.0,
                            1.0,
                            self.settings_revision as f64,
                            model
                                .settings_snapshot
                                .as_ref()
                                .map_or(0.0, |snapshot| snapshot.revision as f64),
                        ],
                    )
                });
                driver.phase = Phase::SettingsNumeric { index: 0, part: 0 };
                widgets.arm(driver, settings_numeric_id(0, 0))
            }
            Phase::SettingsNumeric { index, part } => {
                widgets.arm(driver, settings_numeric_id(index, part))
            }
            Phase::SettingsFooter => widgets.arm(driver, SETTINGS_FOOTER),
            Phase::SettingsReset => widgets.arm(driver, SETTINGS_RESET),
            Phase::SettingsClose => widgets.arm(driver, SETTINGS_CLOSE),
            Phase::AwaitSettingsClosed if !settings.open => {
                driver.phase = Phase::TrainNavigation;
                widgets.arm(driver, crate::view::navigation::stable_id(FeatureId::Train))
            }
            Phase::TrainNavigation => {
                widgets.arm(driver, crate::view::navigation::stable_id(FeatureId::Train))
            }
            Phase::AwaitTrain if active == FeatureId::Train => {
                if !driver.viewer_scenario.is_empty() {
                    driver.phase = Phase::DatasetSource;
                    return widgets.arm(driver, DATASET_SOURCE);
                }
                driver.phase = Phase::PageRegion {
                    page: FeatureId::Train,
                    index: 0,
                };
                widgets.arm(driver, region_id(FeatureId::Train, 0))
            }
            Phase::PageNavigation(page) if route_edit_available(model, settings) => {
                widgets.arm(driver, crate::view::navigation::stable_id(page))
            }
            Phase::AwaitPage(page) if active == page => {
                driver.phase = Phase::PageRegion { page, index: 0 };
                widgets.arm(driver, region_id(page, 0))
            }
            Phase::PageRegion { page, index } => widgets.arm(driver, region_id(page, index)),
            Phase::PagePrimary(page) | Phase::AwaitPagePrimary(page)
                if !crate::integration_control::reporting_enabled()
                    || self.primary_pixels.contains(
                        crate::view::workflow::Composition::new(page, 0.0)
                            .stable_id(crate::view::workflow::Region::PrimaryAction),
                    ) =>
            {
                driver.advance_to(Self::after_page(page))
            }
            Phase::PagePrimary(page) if widgets.begin_location() => reveal_control(
                crate::view::workflow::Composition::new(page, 0.0)
                    .stable_id(crate::view::workflow::Region::PrimaryAction)
                    .to_owned(),
                driver.generation,
                AnnotationReveal::Control,
            ),
            Phase::TrainModelCard => widgets.arm(driver, TRAIN_MODEL_CARD),
            Phase::TrainModelPart(index) => widgets.arm(driver, TRAIN_MODEL_PARTS[index]),
            Phase::TrainModelProgress => widgets.arm(driver, TRAIN_MODEL_PROGRESS),
            Phase::ReturnTrain if route_edit_available(model, settings) => {
                widgets.arm(driver, crate::view::navigation::stable_id(FeatureId::Train))
            }
            Phase::AwaitReturnTrain if active == FeatureId::Train => {
                driver.phase = Phase::AdvancedField(0);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.phase",
                        &advanced_field_id(0),
                        "advanced-field-0",
                        [0.0; 4],
                    )
                });
                widgets.arm_scrolled(driver, advanced_field_id(0), RelativeOffset::END)
            }
            Phase::AdvancedField(index) => {
                if index == 2 || index == 4 {
                    self.spinner_baseline = advanced_control_value(settings, index, 0.0);
                }
                widgets.arm(driver, advanced_field_id(index))
            }
            Phase::AdvancedSpinnerEdge { index, .. } => {
                widgets.arm(driver, advanced_field_id(index))
            }
            Phase::AdvancedSpinnerWheel(index) => widgets.arm(driver, advanced_field_id(index)),
            Phase::AdvancedSpinnerVerify { index, upper } => {
                let value = advanced_control_value(settings, index, self.spinner_baseline);
                if value != self.spinner_baseline {
                    driver.fail("numeric edge exposed an increment or decrement hit target");
                    return Task::none();
                }
                if upper {
                    driver.phase = Phase::AdvancedSpinnerEdge {
                        index,
                        upper: false,
                    };
                    widgets.arm(driver, advanced_field_id(index))
                } else {
                    driver.phase = Phase::AdvancedSpinnerWheel(index);
                    widgets.arm(driver, advanced_field_id(index))
                }
            }
            Phase::AdvancedSpinnerWheelVerify(index) => {
                let value = advanced_control_value(settings, index, self.spinner_baseline);
                if value != self.spinner_baseline {
                    driver.fail("numeric field allowed wheel mutation");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.spinnerless",
                        &advanced_field_id(index),
                        if index == 2 {
                            "integer-upper-lower-edges"
                        } else {
                            "floating-upper-lower-edges"
                        },
                        [value, 1.0, 1.0, 1.0],
                    )
                });
                self.begin_advanced_numeric_edit(driver, widgets, model, settings, index)
            }
            Phase::AdvancedNumericEdit(index) => widgets.arm(driver, advanced_field_id(index)),
            Phase::AwaitAdvancedNumericDraft(index)
                if same_numeric_value(
                    advanced_control_value(settings, index, f64::NAN),
                    self.numeric_target,
                ) =>
            {
                driver.phase = Phase::AwaitAdvancedNumericSnapshot(index);
                Task::none()
            }
            Phase::AwaitAdvancedNumericSnapshot(index)
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|_| {
                        advanced_snapshot_value(model, index)
                            .is_some_and(|value| same_numeric_value(value, self.numeric_target))
                    }) =>
            {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.advanced_edit",
                        &advanced_field_id(index),
                        if index == 2 { "integer" } else { "floating" },
                        [
                            self.numeric_target,
                            self.settings_revision as f64,
                            model
                                .settings_snapshot
                                .as_ref()
                                .map_or(0.0, |snapshot| snapshot.revision as f64),
                            1.0,
                        ],
                    )
                });
                driver.phase = Phase::AdvancedField(index + 1);
                widgets.arm(driver, advanced_field_id(index + 1))
            }
            Phase::AdvancedAssignment => {
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                widgets.arm_scrolled(driver, train::MATCH_FREE_ASSIGNMENT_ID, RelativeOffset::END)
            }
            Phase::AwaitAdvancedAssignmentDraft
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.request.trainingsupervision.assignment
                        == crate::generated::TrainAssignmentKind::MatchFree
                }) =>
            {
                driver.phase = Phase::AwaitAdvancedAssignmentSnapshot;
                Task::none()
            }
            Phase::AwaitAdvancedAssignmentSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .request
                            .trainingsupervision
                            .assignment
                            == crate::generated::TrainAssignmentKind::MatchFree
                    }) =>
            {
                driver.phase = Phase::AdvancedMatchFree(0);
                widgets.arm(driver, match_free_field_id(0))
            }
            Phase::AdvancedMatchFree(index) => widgets.arm(driver, match_free_field_id(index)),
            Phase::AdvancedDenoisingToggle => {
                self.denoising_target = settings.draft.as_ref().is_none_or(|draft| {
                    !draft
                        .workflows
                        .train
                        .request
                        .trainingsupervision
                        .denoising
                        .enabled
                });
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                widgets.arm(driver,
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingenabled()
                        .stable_field_id
                        .to_string(),
                )
            }
            Phase::AwaitAdvancedDenoisingDraft
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft
                        .workflows
                        .train
                        .request
                        .trainingsupervision
                        .denoising
                        .enabled
                        == self.denoising_target
                }) =>
            {
                driver.phase = Phase::AwaitAdvancedDenoisingSnapshot;
                Task::none()
            }
            Phase::AwaitAdvancedDenoisingSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .request
                            .trainingsupervision
                            .denoising
                            .enabled
                            == self.denoising_target
                    }) =>
            {
                if self.denoising_target {
                    driver.phase = Phase::AdvancedDenoising(0);
                    widgets.arm(driver, denoising_field_id(0))
                } else {
                    driver.phase = Phase::AdvancedDenoisingToggle;
                    Task::none()
                }
            }
            Phase::AdvancedDenoising(index) => widgets.arm(driver, denoising_field_id(index)),
            // Capture the expanded panel and every field without intervening
            // scrolling or settings changes so their coordinates are comparable.
            Phase::AdvancedLayout(index) => widgets.arm(driver, advanced_layout_field(index).0),
            Phase::TriggerError => {
                driver.phase = Phase::AwaitErrorModal;
                Task::done(RootMessage::Workspace(crate::view::router::Message::Train(
                    train::Message::StartRequested,
                )))
            }
            Phase::AwaitErrorModal if model.error.is_some() => {
                driver.phase = Phase::ErrorModal;
                widgets.arm(driver, ERROR_MODAL)
            }
            Phase::ErrorModal => widgets.arm(driver, ERROR_MODAL),
            Phase::ErrorCopy => widgets.arm(driver, ERROR_COPY),
            Phase::AwaitErrorCopy if model.error.is_some() => {
                driver.phase = Phase::ErrorDismiss;
                widgets.arm(driver, ERROR_DISMISS)
            }
            Phase::ErrorDismiss => widgets.arm(driver, ERROR_DISMISS),
            Phase::AwaitErrorDismissed if model.error.is_none() => {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.error_modal",
                        ERROR_MODAL,
                        "copy-and-dismiss",
                        [1.0, 1.0, 1.0, 0.0],
                    )
                });
                driver.phase = Phase::TrainCard;
                widgets.arm_scrolled(driver, TRAIN_CARD, RelativeOffset::START)
            }
            Phase::TrainCard => widgets.arm_scrolled(driver, TRAIN_CARD, RelativeOffset::START),
            Phase::DatasetBrowse => widgets.arm(driver, DATASET_BROWSE),
            Phase::BenchmarkOverride => {
                if let Some(draft) = &settings.draft {
                    self.benchmark_selection =
                        Some(draft.workflows.train.benchmarkselection.clone());
                }
                self.benchmark_baseline = settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| draft.workflows.train.compilebenchmarkdatasetoverride);
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                widgets.arm(driver, BENCHMARK_OVERRIDE)
            }
            Phase::AwaitBenchmarkOverride
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.compilebenchmarkdatasetoverride != self.benchmark_baseline
                }) =>
            {
                driver.phase = Phase::AwaitBenchmarkChangedSnapshot;
                Task::none()
            }
            Phase::AwaitBenchmarkChangedSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .compilebenchmarkdatasetoverride
                            != self.benchmark_baseline
                    }) =>
            {
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(self.settings_revision, |snapshot| snapshot.revision);
                driver.phase = Phase::BenchmarkRestore;
                widgets.arm(driver, BENCHMARK_OVERRIDE)
            }
            Phase::BenchmarkRestore => widgets.arm(driver, BENCHMARK_OVERRIDE),
            Phase::AwaitBenchmarkRestored
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.compilebenchmarkdatasetoverride == self.benchmark_baseline
                }) =>
            {
                driver.phase = Phase::AwaitBenchmarkSnapshot;
                Task::none()
            }
            Phase::AwaitBenchmarkSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .compilebenchmarkdatasetoverride
                            == self.benchmark_baseline
                    }) =>
            {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.benchmark_override",
                        BENCHMARK_OVERRIDE,
                        "round-trip",
                        [
                            1.0,
                            1.0,
                            self.settings_revision as f64,
                            model
                                .settings_snapshot
                                .as_ref()
                                .map_or(0.0, |snapshot| snapshot.revision as f64),
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.benchmark_baseline",
                        BENCHMARK_OVERRIDE,
                        "native-settled",
                        [
                            self.benchmark_baseline as u8 as f64,
                            self.benchmark_selection.as_ref().unwrap().dataset as u8 as f64,
                            self.benchmark_selection.as_ref().unwrap().validation as u8 as f64,
                            model.settings_snapshot.as_ref().unwrap().revision as f64,
                        ],
                    )
                });
                driver.phase = Phase::BenchmarkChoice(0);
                Task::none()
            }
            Phase::BenchmarkChoice(index) => {
                let Some(snapshot) = model
                    .settings_snapshot
                    .as_ref()
                    .filter(|_| !settings.has_local_edits())
                else {
                    return Task::none();
                };
                self.settings_revision = snapshot.revision;
                if index == 7 {
                    self.benchmark_dialog = model.file_dialog.clone();
                    self.benchmark_source = snapshot
                        .settingsstate
                        .workflows
                        .train
                        .datasetsourcedir
                        .clone();
                }
                let (control, enabled, _) = self.benchmark_choice(index);
                // Parent states already matching need no synthetic toggle. Radio no-ops still receive real clicks.
                if control == BENCHMARK_OVERRIDE
                    && snapshot
                        .settingsstate
                        .workflows
                        .train
                        .compilebenchmarkdatasetoverride
                        == enabled
                {
                    driver.phase = Phase::AwaitBenchmarkChoice(index);
                    return Task::none();
                }
                widget_ops::scroll_control_into_view(control.to_owned(), AnnotationReveal::Control)
                    .chain(widgets.arm(driver, control))
            }
            Phase::AwaitBenchmarkChoice(index) => {
                let Some(snapshot) = model.settings_snapshot.as_ref().filter(|snapshot| {
                    snapshot.revision >= self.settings_revision && !settings.has_local_edits()
                }) else {
                    return Task::none();
                };
                let (control, enabled, selection) = self.benchmark_choice(index);
                let train = &snapshot.settingsstate.workflows.train;
                if train.compilebenchmarkdatasetoverride != enabled
                    || train.benchmarkselection != selection
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.benchmark_choice",
                        control,
                        "native-settled",
                        [
                            index as f64,
                            selection.dataset as u8 as f64,
                            selection.validation as u8 as f64,
                            snapshot.revision as f64,
                        ],
                    )
                });
                if index == 7 {
                    let unchanged = model.file_dialog == self.benchmark_dialog
                        && train.datasetsourcedir == self.benchmark_source
                        && snapshot.revision == self.settings_revision;
                    if !unchanged {
                        driver.fail("disabled benchmark source control changed state");
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.benchmark_inactive",
                            DATASET_BROWSE,
                            "unchanged",
                            [7.0, 1.0, snapshot.revision as f64, 0.0],
                        )
                    });
                }
                let visibility = reporting::benchmark_visibility(
                    index,
                    enabled,
                    selection.dataset,
                    snapshot.revision,
                );
                if index == 11 {
                    self.perceptual_baseline = perceptual_control_values(
                        &settings.draft.as_ref().unwrap().workflows.train,
                    );
                    driver.phase = Phase::PerceptualControl(0);
                    visibility.chain(self.arm_perceptual_control(driver, widgets, 0))
                } else {
                    driver.phase = Phase::BenchmarkChoice(index + 1);
                    visibility
                }
            }
            Phase::PerceptualControl(index) => self.arm_perceptual_control(driver, widgets, index),
            Phase::AwaitPerceptualControl(index) => {
                let mut expected = self.perceptual_baseline;
                for step in 0..=index {
                    expected[perceptual_control_slot(step)] ^= true;
                }
                let Some(snapshot) =
                    settled_settings_snapshot(model, settings, self.settings_revision)
                else {
                    return Task::none();
                };
                if perceptual_control_values(&snapshot.settingsstate.workflows.train) != expected {
                    return Task::none();
                }
                self.settings_revision = snapshot.revision;
                if index == 5 {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.perceptual_controls",
                            "train.perceptual",
                            "independent-round-trip",
                            [1.0, 1.0, 1.0, snapshot.revision as f64],
                        )
                    });
                    driver.phase = Phase::DatasetSource;
                    widgets.arm_scrolled(driver, DATASET_SOURCE, RelativeOffset::START)
                } else {
                    driver.phase = Phase::PerceptualControl(index + 1);
                    self.arm_perceptual_control(driver, widgets, index + 1)
                }
            }
            Phase::DatasetSource => widgets.arm(driver, DATASET_SOURCE),
            Phase::AwaitDatasetSource
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.datasetsourcedir == driver.dataset_source
                }) =>
            {
                driver.phase = Phase::CompiledDirectory;
                widgets.arm(driver, COMPILED_DIRECTORY)
            }
            Phase::CompiledDirectory => widgets.arm(driver, COMPILED_DIRECTORY),
            Phase::AwaitCompiledDirectory
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.compileddatasetdir == driver.compiled_directory
                }) =>
            {
                driver.phase = Phase::CompileDimensions;
                widgets.arm(driver, COMPILE_DIMENSIONS)
            }
            Phase::CompileDimensions => widgets.arm(driver, COMPILE_DIMENSIONS),
            Phase::AwaitCompileDimensions
                if settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| draft.workflows.train.compiledimensions) =>
            {
                driver.phase = Phase::CompileResizeMode;
                widgets.arm(driver, COMPILE_LETTERBOX)
            }
            Phase::CompileResizeMode => widgets.arm(driver, COMPILE_LETTERBOX),
            Phase::AwaitCompileResizeMode
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.compileresizemode
                        == crate::generated::ImageResizeMode::Letterbox
                }) =>
            {
                driver.phase = Phase::CompileResolution;
                widgets.arm(driver, COMPILE_RESOLUTION)
            }
            Phase::CompileResolution => widgets.arm(driver, COMPILE_RESOLUTION),
            Phase::AwaitCompileResolution
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.request.resolution.to_string() == driver.resolution
                }) =>
            {
                let revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                driver.phase = Phase::AwaitDatasetSettings(revision);
                Task::none()
            }
            Phase::AwaitDatasetSettings(_) => {
                let Some(snapshot) = model.settings_snapshot.as_ref() else {
                    return Task::none();
                };
                let train = &snapshot.settingsstate.workflows.train;
                if train.compilebenchmarkdatasetoverride
                    || train.datasetsourcedir != driver.dataset_source
                    || train.compileddatasetdir != driver.compiled_directory
                    || train.request.resolution.to_string() != driver.resolution
                    || !train.compiledimensions
                    || train.compileresizemode != crate::generated::ImageResizeMode::Letterbox
                    || !train.usecompileddirectorydefaults
                    || !snapshot.exploresource.available
                    || snapshot.exploresource.selection
                        != crate::generated::ExploreDatasetSource::Train
                    || settings.has_local_edits()
                    || !model.dataset_compile_available()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.dataset_configured",
                        COMPILE_RESOLUTION,
                        "typed-settings",
                        [snapshot.revision as f64, 1.0, 0.0, 0.0],
                    )
                });
                if driver.reuse_compiled {
                    driver.phase = Phase::ExploreNavigation;
                    widgets.arm(
                        driver,
                        crate::view::navigation::stable_id(FeatureId::Explore),
                    )
                } else {
                    driver.phase = Phase::Compile;
                    widgets.arm_scrolled(driver, COMPILE_DATASET, RelativeOffset::END)
                }
            }
            Phase::Compile => widgets.arm_scrolled(driver, COMPILE_DATASET, RelativeOffset::END),
            phase @ (Phase::AwaitCompileProgress | Phase::AwaitCompileCompletion) => {
                let Some(dataset) = model.workflow.dataset.as_ref() else {
                    return Task::none();
                };
                let compile_succeeded = dataset.terminal.outcome
                    == crate::generated::ArtifactTerminalOutcome::Succeeded;
                if matches!(phase, Phase::AwaitCompileProgress)
                    && (dataset.active || compile_succeeded)
                    && (!dataset.progress.activity.is_empty() || dataset.progress.total != 0)
                {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.compile_progress",
                            COMPILE_PROGRESS,
                            &dataset.progress.activity,
                            [
                                dataset.generation as f64,
                                dataset.progress.completed as f64,
                                dataset.progress.total as f64,
                                dataset.progress.droppedinstances as f64,
                            ],
                        )
                    });
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.compile_metrics",
                            COMPILE_PROGRESS,
                            "elapsed-eta-throughput-dropped",
                            [
                                dataset.progress.elapsedseconds as f64,
                                dataset.progress.remainingseconds as f64,
                                dataset.progress.throughputpersecond as f64,
                                dataset.progress.droppedinstances as f64,
                            ],
                        )
                    });
                    driver.phase = Phase::CompileProgress;
                    return widgets.arm(driver, COMPILE_PROGRESS);
                }
                if compile_succeeded {
                    let split = dataset.inspection.splits.first();
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.dataset_complete",
                            DATASET_STATUS,
                            &dataset.terminal.artifact,
                            [
                                dataset.generation as f64,
                                split.map_or(0.0, |value| value.imagecount as f64),
                                split.map_or(0.0, |value| value.width as f64),
                                split.map_or(0.0, |value| value.height as f64),
                            ],
                        )
                    });
                    driver.phase = Phase::DatasetStatus;
                    return widgets.arm(driver, DATASET_STATUS);
                }
                Task::none()
            }
            Phase::CompileProgress => widgets.arm(driver, COMPILE_PROGRESS),
            Phase::CompileActionWithProgress => widgets.arm(driver, COMPILE_DATASET),
            Phase::DatasetStatus => widgets.arm(driver, DATASET_STATUS),
            _ => Task::none(),
        }
    }
    pub(super) fn expected_lifecycle(&self, driver: &Driver) -> Option<String> {
        Some(match driver.phase {

            Phase::SettingsOpen => "navigation.settings".to_owned(),
            Phase::SettingsModal => SETTINGS_MODAL.to_owned(),
            Phase::SettingsGroup(index) => SETTINGS_GROUPS[index].to_owned(),
            Phase::SettingsScaleDrag => SETTINGS_NUMERIC_CONTROLS[0].to_owned(),
            Phase::SettingsShowFps => SETTINGS_SHOW_FPS.to_owned(),
            Phase::SettingsRestoreShowFps => SETTINGS_SHOW_FPS.to_owned(),
            Phase::SettingsNumeric { index, part } => settings_numeric_id(index, part),
            Phase::SettingsFooter => SETTINGS_FOOTER.to_owned(),
            Phase::SettingsReset => SETTINGS_RESET.to_owned(),
            Phase::SettingsClose => SETTINGS_CLOSE.to_owned(),
            Phase::TrainNavigation => {
                crate::view::navigation::stable_id(FeatureId::Train).to_owned()
            }
            Phase::PageNavigation(page) => crate::view::navigation::stable_id(page).to_owned(),
            Phase::PageRegion { page, index } => region_id(page, index).to_owned(),
            Phase::PagePrimary(page) => crate::view::workflow::Composition::new(page, 0.0)
                .stable_id(crate::view::workflow::Region::PrimaryAction)
                .to_owned(),
            Phase::TrainModelCard => TRAIN_MODEL_CARD.to_owned(),
            Phase::TrainModelPart(index) => TRAIN_MODEL_PARTS[index].to_owned(),
            Phase::TrainModelProgress => TRAIN_MODEL_PROGRESS.to_owned(),
            Phase::ReturnTrain => crate::view::navigation::stable_id(FeatureId::Train).to_owned(),
            Phase::AdvancedField(index) => advanced_field_id(index),
            Phase::AdvancedSpinnerEdge { index, .. } => advanced_field_id(index),
            Phase::AdvancedSpinnerWheel(index) => advanced_field_id(index),
            Phase::AdvancedNumericEdit(index) => advanced_field_id(index),
            Phase::AdvancedAssignment => train::MATCH_FREE_ASSIGNMENT_ID.to_owned(),
            Phase::AdvancedMatchFree(index) => match_free_field_id(index),
            Phase::AdvancedDenoisingToggle => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingenabled()
                .stable_field_id
                .to_string(),
            Phase::AdvancedDenoising(index) => denoising_field_id(index),
            Phase::AdvancedLayout(index) => advanced_layout_field(index).0,
            Phase::ErrorModal => ERROR_MODAL.to_owned(),
            Phase::ErrorCopy => ERROR_COPY.to_owned(),
            Phase::ErrorDismiss => ERROR_DISMISS.to_owned(),
            Phase::TrainCard => TRAIN_CARD.to_owned(),
            Phase::DatasetBrowse => DATASET_BROWSE.to_owned(),
            Phase::BenchmarkOverride => BENCHMARK_OVERRIDE.to_owned(),
            Phase::BenchmarkRestore => BENCHMARK_OVERRIDE.to_owned(),
            Phase::BenchmarkChoice(index) => self.benchmark_choice(index).0.to_owned(),
            Phase::DatasetSource => DATASET_SOURCE.to_owned(),
            Phase::CompiledDirectory => COMPILED_DIRECTORY.to_owned(),
            Phase::PerceptualControl(index) => perceptual_control_id(index),
            Phase::CompileDimensions => COMPILE_DIMENSIONS.to_owned(),
            Phase::CompileResizeMode => COMPILE_LETTERBOX.to_owned(),
            Phase::CompileResolution => COMPILE_RESOLUTION.to_owned(),
            Phase::Compile | Phase::CompileActionWithProgress => COMPILE_DATASET.to_owned(),
            Phase::CompileProgress => COMPILE_PROGRESS.to_owned(),
            Phase::DatasetStatus => DATASET_STATUS.to_owned(),
            _ => return None,
        })
    }
    pub(super) fn located_lifecycle(
        &mut self,
        driver: &mut Driver,
        widgets: &mut widget_ops::RevealState,
        bounds: Rectangle,
        input_bounds: Rectangle,
    ) -> Option<train::Message> {
        match driver.phase.clone() {
            Phase::SettingsOpen => {
                driver.phase = Phase::AwaitSettings;
                if !click(input_bounds) {
                    driver.fail("Firefox Settings click dispatch failed");
                }
                None
            }
            Phase::SettingsModal => {
                driver.phase = Phase::SettingsGroup(0);
                None
            }
            Phase::SettingsGroup(index) => {
                driver.phase = if index + 1 < SETTINGS_GROUPS.len() {
                    Phase::SettingsGroup(index + 1)
                } else {
                    Phase::SettingsScaleDrag
                };
                None
            }
            Phase::SettingsScaleDrag => {
                #[cfg(target_arch = "wasm32")]
                let dispatched = slider_drag_js(
                    f64::from(input_bounds.x),
                    f64::from(input_bounds.y),
                    f64::from(input_bounds.width),
                    f64::from(input_bounds.height),
                ) == 1;
                #[cfg(not(target_arch = "wasm32"))]
                let dispatched = false;
                if !dispatched {
                    driver.fail("Firefox UI-scale pointer drag dispatch failed");
                    return None;
                }
                driver.phase = Phase::AwaitSettingsScaleDrag;
                None
            }
            Phase::SettingsShowFps => {
                driver.phase = Phase::AwaitSettingsShowFps;
                if !click(input_bounds) {
                    driver.fail("Firefox Show FPS click dispatch failed");
                }
                None
            }
            Phase::SettingsRestoreShowFps => {
                driver.phase = Phase::AwaitSettingsShowFpsRestored;
                if !click(input_bounds) {
                    driver.fail("Firefox Show FPS restore click dispatch failed");
                }
                None
            }
            Phase::SettingsNumeric { index, part } => {
                driver.phase = if part < 2 {
                    Phase::SettingsNumeric {
                        index,
                        part: part + 1,
                    }
                } else if index + 1 < SETTINGS_NUMERIC_CONTROLS.len() {
                    Phase::SettingsNumeric {
                        index: index + 1,
                        part: 0,
                    }
                } else {
                    Phase::SettingsFooter
                };
                None
            }
            Phase::SettingsFooter => {
                driver.phase = Phase::SettingsReset;
                None
            }
            Phase::SettingsReset => {
                driver.phase = Phase::SettingsClose;
                None
            }
            Phase::SettingsClose => {
                driver.phase = Phase::AwaitSettingsClosed;
                if !click(input_bounds) {
                    driver.fail("Firefox Settings close dispatch failed");
                }
                None
            }
            Phase::PageNavigation(page) => {
                driver.phase = Phase::AwaitPage(page);
                if !click(input_bounds) {
                    driver.fail("Firefox navigation click dispatch failed");
                }
                None
            }
            Phase::PageRegion { page, index } => {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.page_region",
                        region_id(page, index),
                        crate::view::navigation::label(page),
                        [
                            f64::from(bounds.x),
                            f64::from(bounds.y),
                            f64::from(bounds.width),
                            f64::from(bounds.height),
                        ],
                    )
                });
                let composition = crate::view::workflow::Composition::new(page, 0.0);
                if index + 1 < composition.audit_regions().len() {
                    driver.phase = Phase::PageRegion {
                        page,
                        index: index + 1,
                    };
                } else {
                    driver.phase = Phase::PagePrimary(page);
                }
                None
            }
            Phase::PagePrimary(page) => {
                driver.phase = Phase::AwaitPagePrimary(page);
                None
            }
            Phase::TrainModelCard => {
                driver.phase = Phase::TrainModelPart(0);
                None
            }
            Phase::TrainModelPart(index) => {
                driver.phase = if index + 1 < TRAIN_MODEL_PARTS.len() {
                    Phase::TrainModelPart(index + 1)
                } else {
                    Phase::TrainModelProgress
                };
                None
            }
            Phase::TrainModelProgress => {
                driver.phase = crate::view::workflow::Composition::new(FeatureId::Train, 0.0)
                    .next_ordinary_page()
                    .map_or(Phase::ReturnTrain, Phase::PageNavigation);
                None
            }
            Phase::ReturnTrain => {
                driver.phase = Phase::AwaitReturnTrain;
                if !click(input_bounds) {
                    driver.fail("Firefox Train navigation click dispatch failed");
                }
                None
            }
            Phase::AdvancedField(index) => {
                driver.phase = if index == 2 || index == 4 {
                    Phase::AdvancedSpinnerEdge { index, upper: true }
                } else if index + 1 < 8 {
                    Phase::AdvancedField(index + 1)
                } else {
                    Phase::AdvancedAssignment
                };
                None
            }
            Phase::AdvancedSpinnerEdge { index, upper } => {
                if !click_number_edge(input_bounds, upper) {
                    driver.fail("Firefox numeric-field edge click dispatch failed");
                    return None;
                }
                driver.phase = Phase::AdvancedSpinnerVerify { index, upper };
                None
            }
            Phase::AdvancedSpinnerWheel(index) => {
                if !wheel_number_input(input_bounds) {
                    driver.fail("Firefox numeric-field wheel dispatch failed");
                    return None;
                }
                // The containing page may scroll even when the number ignores
                // the wheel. Locate the next input after Iced applies that event.
                driver.phase = Phase::AwaitAdvancedSpinnerWheel(index);
                None
            }
            Phase::AdvancedNumericEdit(index) => {
                if !widgets.replace_numeric_input(input_bounds, self.numeric_selection_length) {
                    driver.fail("Firefox numeric-field text replacement dispatch failed");
                    return None;
                }
                driver.phase = Phase::AwaitAdvancedNumericDraft(index);
                None
            }
            Phase::AdvancedAssignment => {
                driver.phase = Phase::AwaitAdvancedAssignmentDraft;
                if !click(input_bounds) {
                    driver.fail("Firefox Match-Free assignment click dispatch failed");
                }
                None
            }
            Phase::AdvancedMatchFree(index) => {
                driver.phase = if index + 1 < 3 {
                    Phase::AdvancedMatchFree(index + 1)
                } else {
                    Phase::AdvancedDenoisingToggle
                };
                None
            }
            Phase::AdvancedDenoisingToggle => {
                driver.phase = Phase::AwaitAdvancedDenoisingDraft;
                if !click(input_bounds) {
                    driver.fail("Firefox denoising toggle click dispatch failed");
                }
                None
            }
            Phase::AdvancedDenoising(index) => {
                driver.phase = if index + 1 < 4 {
                    Phase::AdvancedDenoising(index + 1)
                } else {
                    Phase::AdvancedLayout(0)
                };
                None
            }
            Phase::AdvancedLayout(index) => {
                driver.phase = if index < 17 {
                    Phase::AdvancedLayout(index + 1)
                } else {
                    Phase::TriggerError
                };
                None
            }
            Phase::ErrorModal => {
                driver.phase = Phase::ErrorCopy;
                None
            }
            Phase::ErrorCopy => {
                driver.phase = Phase::AwaitErrorCopy;
                if !click(input_bounds) {
                    driver.fail("Firefox error Copy click dispatch failed");
                }
                None
            }
            Phase::ErrorDismiss => {
                driver.phase = Phase::AwaitErrorDismissed;
                if !click(input_bounds) {
                    driver.fail("Firefox error Dismiss click dispatch failed");
                }
                None
            }
            Phase::TrainCard => {
                driver.phase = Phase::DatasetBrowse;
                None
            }
            Phase::DatasetBrowse => {
                driver.phase = Phase::BenchmarkOverride;
                None
            }
            Phase::BenchmarkOverride => {
                driver.phase = Phase::AwaitBenchmarkOverride;
                if !click(input_bounds) {
                    driver.fail("Firefox benchmark override click dispatch failed");
                }
                None
            }
            Phase::BenchmarkChoice(index) => {
                driver.phase = Phase::AwaitBenchmarkChoice(index);
                if !click(input_bounds) {
                    driver.fail("Firefox benchmark radio click dispatch failed");
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.benchmark_click",
                        self.benchmark_choice(index).0,
                        "real-click",
                        [index as f64, 1.0, 0.0, 0.0],
                    )
                });
                None
            }
            Phase::BenchmarkRestore => {
                driver.phase = Phase::AwaitBenchmarkRestored;
                if !click(input_bounds) {
                    driver.fail("Firefox benchmark override restore click dispatch failed");
                }
                None
            }
            Phase::DatasetSource => {
                driver.phase = Phase::AwaitDatasetSource;
                Some(train::Message::Dataset(
                    train::dataset::Message::SourceChanged(driver.dataset_source.clone()),
                ))
            }
            Phase::CompiledDirectory => {
                driver.phase = Phase::AwaitCompiledDirectory;
                Some(train::Message::Dataset(
                    train::dataset::Message::CompiledDirectoryChanged(
                        driver.compiled_directory.clone(),
                    ),
                ))
            }
            Phase::PerceptualControl(index) => {
                driver.phase = Phase::AwaitPerceptualControl(index);
                if !click(input_bounds) {
                    driver.fail("Firefox perceptual control click dispatch failed");
                }
                None
            }
            Phase::CompileDimensions => {
                driver.phase = Phase::AwaitCompileDimensions;
                Some(train::Message::Dataset(
                    train::dataset::Message::CompileDimensionsChanged(true),
                ))
            }
            Phase::CompileResizeMode => {
                driver.phase = Phase::AwaitCompileResizeMode;
                if !click(input_bounds) {
                    driver.fail("Firefox Letterbox radio click dispatch failed");
                }
                None
            }
            Phase::CompileResolution => {
                driver.phase = Phase::AwaitCompileResolution;
                Some(train::Message::Dataset(
                    train::dataset::Message::ResolutionChanged(
                        driver.resolution.parse().unwrap_or_default(),
                    ),
                ))
            }
            Phase::DatasetStatus => {
                driver.phase = Phase::ExploreNavigation;
                None
            }
            Phase::CompileProgress => {
                driver.phase = Phase::CompileActionWithProgress;
                None
            }
            Phase::CompileActionWithProgress => {
                driver.phase = Phase::AwaitCompileCompletion;
                None
            }
            _ => {
                driver.phase = match driver.phase {
                    Phase::TrainNavigation => Phase::AwaitTrain,
                    Phase::Compile => Phase::AwaitCompileProgress,
                    _ => driver.phase.clone(),
                };
                driver.click_located(input_bounds)
            }
        }
    }
}

impl State {
    pub(super) fn arm_perceptual_control(
        &mut self,
        driver: &mut Driver,
        widgets: &mut widget_ops::RevealState,
        index: usize,
    ) -> Task<RootMessage> {
        if !widgets.begin_location() {
            return Task::none();
        }
        reveal_control(
            perceptual_control_id(index),
            driver.generation,
            AnnotationReveal::Control,
        )
    }
}

impl State {
    pub(super) fn wheel_delivered(&mut self, driver: &mut Driver) {
        if let Phase::AwaitAdvancedSpinnerWheel(index) = driver.phase {
            driver.phase = Phase::AdvancedSpinnerWheelVerify(index);
            reporting::emit(|sink| {
                sink.record(
                    "integration.number_wheel_delivered",
                    &advanced_field_id(index),
                    "iced-widget-update-complete",
                    [0.0; 4],
                )
            });
        }
    }
}

pub(super) fn perceptual_control_slot(index: usize) -> usize {
    [0, 1, 1, 0, 2, 2][index]
}

pub(super) fn perceptual_control_id(index: usize) -> String {
    match perceptual_control_slot(index) {
        0 => {
            crate::generated::constraint_workflowstrainrequestgpuaugmentationenabled()
                .stable_field_id
        }
        1 => {
            crate::generated::constraint_workflowstrainrequestgpuaugmentationperceptualdownscale()
                .stable_field_id
        }
        _ => {
            crate::generated::constraint_workflowstraincompileperceptualdownscale().stable_field_id
        }
    }
    .to_string()
}

pub(super) fn perceptual_control_values(train: &crate::generated::TrainViewState) -> [bool; 3] {
    [
        train.request.gpuaugmentation.enabled,
        train.request.gpuaugmentation.perceptualdownscale,
        train.compileperceptualdownscale,
    ]
}

pub(super) fn advanced_layout_field(index: usize) -> (String, String) {
    match index {
        0 => ("workflow.advanced".into(), "container".into()),
        1..=8 => (advanced_field_id(index - 1), format!("fixed-{}", index - 1)),
        9 => (train::MATCH_FREE_ASSIGNMENT_ID.into(), "assignment".into()),
        10 => (
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingenabled()
                .stable_field_id
                .to_string(),
            "dn-toggle".into(),
        ),
        11..=13 => (
            match_free_field_id(index - 11),
            format!("match-free-{}", index - 11),
        ),
        _ => (denoising_field_id(index - 14), format!("dn-{}", index - 14)),
    }
}

pub(super) fn region_id(page: FeatureId, index: usize) -> &'static str {
    let composition = crate::view::workflow::Composition::new(page, 0.0);
    composition.stable_id(composition.audit_regions()[index])
}

pub(super) fn settings_numeric_id(index: usize, part: usize) -> String {
    let control = SETTINGS_NUMERIC_CONTROLS[index];
    match part {
        0 => control.to_owned(),
        1 => format!("{control}.label"),
        2 => format!("{control}.value"),
        _ => unreachable!("Settings numeric audit has three rendered identities"),
    }
}

pub(super) fn advanced_field_id(index: usize) -> String {
    let constraint = match index {
        0 => crate::generated::constraint_workflowstrainrequestbatchsize(),
        1 => crate::generated::constraint_workflowstrainrequestvalbatchsize(),
        2 => crate::generated::constraint_workflowstrainrequestepochs(),
        3 => crate::generated::constraint_workflowstrainrequestgradaccumsteps(),
        4 => crate::generated::constraint_workflowstrainrequestlr(),
        5 => crate::generated::constraint_workflowstrainrequestlrencoder(),
        6 => crate::generated::constraint_workflowstrainrequestweightdecay(),
        7 => crate::generated::constraint_workflowstrainrequestmomentum(),
        _ => unreachable!("Advanced has eight fixed numeric controls"),
    };
    constraint.stable_field_id.to_string()
}

pub(super) fn match_free_field_id(index: usize) -> String {
    let constraint = match index {
        0 => crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreerho(),
        1 => crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreecorrespondenceweight(),
        2 => crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreequeryweight(),
        _ => unreachable!("Match-Free has three numeric controls"),
    };
    constraint.stable_field_id.to_string()
}

pub(super) fn denoising_field_id(index: usize) -> String {
    let constraint = match index {
        0 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinggroups(),
        1 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinglabelnoiseratio(),
        2 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingcenternoisescale(),
        3 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingsizenoisescale(),
        _ => unreachable!("DN has four numeric controls"),
    };
    constraint.stable_field_id.to_string()
}

pub(super) fn advanced_control_value(
    settings: &crate::view::settings::SettingsModel,
    index: usize,
    fallback: f64,
) -> f64 {
    settings.draft.as_ref().map_or(fallback, |draft| {
        if index == 2 {
            f64::from(draft.workflows.train.request.epochs)
        } else {
            crate::generated::effective_workflowstrainrequestlr(&draft.workflows.train.request)
        }
    })
}

pub(super) fn advanced_snapshot_value(model: &ApplicationModel, index: usize) -> Option<f64> {
    model.settings_snapshot.as_ref().map(|snapshot| {
        if index == 2 {
            f64::from(snapshot.settingsstate.workflows.train.request.epochs)
        } else {
            crate::generated::effective_workflowstrainrequestlr(
                &snapshot.settingsstate.workflows.train.request,
            )
        }
    })
}

pub(super) fn numeric_edit_target(
    settings: &crate::view::settings::SettingsModel,
    index: usize,
) -> Option<(f64, String, usize)> {
    let draft = settings.draft.as_ref()?;
    let (current_text, target, target_text) = if index == 2 {
        let current = draft.workflows.train.request.epochs;
        let constraint = crate::generated::constraint_workflowstrainrequestepochs();
        let minimum = constraint.minimum.map_or(i32::MIN, |value| value as i32);
        let maximum = constraint.maximum.map_or(i32::MAX, |value| value as i32);
        let target = if current < maximum {
            current + 1
        } else if current > minimum {
            current - 1
        } else {
            return None;
        };
        (current.to_string(), f64::from(target), target.to_string())
    } else {
        let current =
            crate::generated::effective_workflowstrainrequestlr(&draft.workflows.train.request);
        let constraint = crate::generated::constraint_workflowstrainrequestlr();
        let minimum = constraint.minimum.unwrap_or(f64::MIN);
        let maximum = constraint.maximum.unwrap_or(f64::MAX);
        let step = 0.0001;
        let target = if minimum.is_finite() && current != minimum {
            minimum
        } else if current + step <= maximum {
            current + step
        } else if current - step >= minimum {
            current - step
        } else {
            return None;
        };
        (current.to_string(), target, target.to_string())
    };
    let selection_length = current_text.chars().count();
    (!target_text.is_empty() && selection_length > 0).then_some((
        target,
        target_text,
        selection_length,
    ))
}

pub(super) fn same_numeric_value(left: f64, right: f64) -> bool {
    (left - right).abs() <= f64::EPSILON * left.abs().max(right.abs()).max(1.0) * 8.0
}

pub(super) const TRAIN_CARD: &str = train::DATASET_CARD_ID;

pub(super) const COMPILE_DATASET: &str = train::COMPILE_DATASET_ID;

pub(super) const DATASET_STATUS: &str = train::DATASET_STATUS_ID;

pub(super) const DATASET_SOURCE: &str = train::DATASET_SOURCE_ID;

pub(super) const COMPILED_DIRECTORY: &str = train::COMPILED_DIRECTORY_ID;

pub(super) const COMPILE_DIMENSIONS: &str = train::COMPILE_DIMENSIONS_ID;

pub(super) const COMPILE_RESOLUTION: &str = train::COMPILE_RESOLUTION_ID;
const COMPILE_LETTERBOX: &str = "train.dataset.resize.letterbox";

pub(super) const COMPILE_PROGRESS: &str = train::COMPILE_PROGRESS_ID;

pub(super) const DATASET_BROWSE: &str = train::DATASET_BROWSE_ID;

pub(super) const TRAIN_MODEL_CARD: &str =
    crate::view::workflow::model_card::stable_id(crate::generated::FeatureId::Train);

pub(super) const TRAIN_MODEL_PROGRESS: &str =
    crate::view::workflow::model_card::progress_id(crate::generated::FeatureId::Train);

pub(super) const TRAIN_MODEL_PARTS: [&str; 6] = [
    crate::view::workflow::model_card::TRAIN_SELECTOR_ID,
    crate::view::workflow::model_card::TRAIN_PRESETS_ID,
    crate::view::workflow::model_card::TRAIN_DIVIDER_ID,
    crate::view::workflow::model_card::TRAIN_CUSTOM_ID,
    crate::view::workflow::model_card::TRAIN_STATUS_ID,
    crate::view::workflow::model_card::TRAIN_ACTION_ID,
];

pub(super) const BENCHMARK_OVERRIDE: &str = train::BENCHMARK_OVERRIDE_ID;

pub(super) const SETTINGS_GROUPS: [&str; 3] = [
    "settings.group.appearance",
    "settings.group.typography",
    "settings.group.environment",
];

pub(super) const SETTINGS_SHOW_FPS: &str = "settings.show_fps";

pub(super) const SETTINGS_NUMERIC_CONTROLS: [&str; 5] = [
    "settings.ui_scale",
    "settings.font_size",
    "settings.secondary_font_size",
    "settings.mono_font_size",
    "settings.text_input_font_size",
];

pub(super) const SETTINGS_FOOTER: &str = "settings.footer";

pub(super) const SETTINGS_MODAL: &str = "settings.modal";

pub(super) const SETTINGS_RESET: &str = "settings.reset";

pub(super) const SETTINGS_CLOSE: &str = "settings.close";

pub(super) const ERROR_MODAL: &str = "error.modal";

pub(super) const ERROR_COPY: &str = crate::view::error_modal::COPY_ID;

pub(super) const ERROR_DISMISS: &str = crate::view::error_modal::DISMISS_ID;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::generated::{
        BenchmarkDatasetSelection, BenchmarkDatasetVariant as Dataset,
        CoconutValidation as Validation,
    };
    use crate::integration_control::Controller;
    use iced::futures::StreamExt;

    #[test]
    fn quiet_benchmark_settlement_advances_all_choices_and_arms_perceptual_handoff() {
        let capture = reporting::Capture::new(false);
        for baseline in [false, true] {
            for dataset in [Dataset::CocoCustom, Dataset::Coconut] {
                for validation in [
                    Validation::Coconut,
                    Validation::Stock,
                    Validation::CoconutStock,
                ] {
                    let mut controller = Controller::new(
                        true,
                        false,
                        String::new(),
                        String::new(),
                        String::new(),
                        "quiet".into(),
                    );
                    assert!(controller.driver.running());
                    let original = BenchmarkDatasetSelection {
                        dataset,
                        validation,
                    };
                    controller.lifecycle.benchmark_baseline = baseline;
                    controller.lifecycle.benchmark_selection = Some(original.clone());
                    let mut model = crate::view_model::test_support::bootstrapped();
                    let mut settings = crate::view::settings::SettingsModel::default();
                    let choices = [
                        (true, dataset, validation),
                        (true, Dataset::Coconut, validation),
                        (true, Dataset::Coconut, Validation::Stock),
                        (true, Dataset::Coconut, Validation::CoconutStock),
                        (true, Dataset::Coconut, Validation::Coconut),
                        (true, Dataset::CocoCustom, Validation::Coconut),
                        (true, Dataset::Coconut, Validation::Coconut),
                        (true, Dataset::Coconut, Validation::Coconut),
                        (true, Dataset::Coconut, validation),
                        (true, dataset, validation),
                        (baseline, dataset, validation),
                        (false, dataset, validation),
                    ];
                    controller.driver.phase = Phase::BenchmarkChoice(0);
                    for (index, (enabled, dataset, validation)) in choices.into_iter().enumerate() {
                        assert_eq!(controller.driver.phase, Phase::BenchmarkChoice(index));
                        settings.install(model.settings_snapshot.as_ref().unwrap());
                        drop(controller.lifecycle.advance_lifecycle(
                            &mut controller.driver,
                            &mut controller.widgets,
                            &model,
                            &settings,
                            1.0,
                            FeatureId::Train,
                        ));
                        // Real clicks/native replies are covered by packaged
                        // acceptance. Here settle the native result and exercise
                        // the ordinary lifecycle transition with reporting off.
                        controller.widgets.location_completed();
                        controller.driver.phase = Phase::AwaitBenchmarkChoice(index);
                        let snapshot = model.settings_snapshot.as_mut().unwrap();
                        if index != 7 {
                            snapshot.revision += 1;
                        }
                        let train = &mut snapshot.settingsstate.workflows.train;
                        train.compilebenchmarkdatasetoverride = !enabled;
                        train.benchmarkselection = BenchmarkDatasetSelection {
                            dataset,
                            validation,
                        };
                        settings.install(snapshot);
                        let waiting = controller.lifecycle.advance_lifecycle(
                            &mut controller.driver,
                            &mut controller.widgets,
                            &model,
                            &settings,
                            1.0,
                            FeatureId::Train,
                        );
                        assert!(iced_runtime::task::into_stream(waiting).is_none());
                        assert_eq!(controller.driver.phase, Phase::AwaitBenchmarkChoice(index));
                        let snapshot = model.settings_snapshot.as_mut().unwrap();
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .compilebenchmarkdatasetoverride = enabled;
                        settings.install(snapshot);
                        let task = controller.lifecycle.advance_lifecycle(
                            &mut controller.driver,
                            &mut controller.widgets,
                            &model,
                            &settings,
                            1.0,
                            FeatureId::Train,
                        );
                        if index == 11 {
                            assert_eq!(controller.driver.phase, Phase::PerceptualControl(0));
                            assert!(controller.widgets.location_pending());
                            let mut actions = iced_runtime::task::into_stream(task).unwrap();
                            let mut located = 0;
                            while let Some(action) =
                                iced::futures::executor::block_on(actions.next())
                            {
                                match action {
                                    iced_runtime::Action::Widget(operation) => {
                                        let _ = operation.finish();
                                    }
                                    iced_runtime::Action::Output(RootMessage::Integration(
                                        crate::integration_control::Message::Scoped {
                                            message, ..
                                        },
                                    )) => {
                                        let crate::integration_control::Message::Located {
                                            control,
                                            ..
                                        } = *message
                                        else {
                                            panic!(
                                                "handoff must locate the first perceptual control"
                                            );
                                        };
                                        assert_eq!(control, perceptual_control_id(0));
                                        located += 1;
                                    }
                                    _ => panic!("unexpected perceptual handoff task"),
                                }
                            }
                            assert_eq!(located, 1);
                            assert_eq!(
                                controller.lifecycle.perceptual_baseline,
                                perceptual_control_values(
                                    &settings.draft.as_ref().unwrap().workflows.train,
                                ),
                            );
                        } else {
                            assert_eq!(controller.driver.phase, Phase::BenchmarkChoice(index + 1));
                            assert!(!controller.widgets.location_pending());
                            assert!(iced_runtime::task::into_stream(task).is_none());
                        }
                        if index >= 10 {
                            assert_eq!(
                                settings
                                    .draft
                                    .as_ref()
                                    .unwrap()
                                    .workflows
                                    .train
                                    .benchmarkselection,
                                original,
                            );
                        }
                        assert!(controller.driver.reporting.state_is_absent());
                        assert!(capture.records().is_empty());
                    }
                }
            }
        }
    }
}
