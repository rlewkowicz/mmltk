use super::{filter_edit, settings_edit};
use crate::fluent_theme::Element;
use crate::view::settings::{EditSchedule, SettingsModel};
use crate::view_model::{ApplicationModel, ExploreModel};
use iced::widget::{button, checkbox, column, container, row, rule, space, text, text_input};
use iced::{Center, Fill};

#[derive(Debug, Clone)]
pub enum Message {
    Loading(crate::view::workflow::loading::Message),
    DeviceChanged(i32),
    OpenRequested,
    StopRequested,
    DialogRequested(u64),
    SourceSelected(crate::generated::ExploreDatasetSource),
    CustomSourceChanged(String),
    MinimumInstancesChanged(u32),
    MaximumInstancesChanged(u32),
    OrderSelected(crate::generated::ExploreOrder),
    ShuffleSeedChanged(u64),
    MinimumCompiledIndexChanged(u64),
    MaximumCompiledIndexChanged(u64),
    UnlimitedCompiledIndex,
    RequireBoxesToggled(bool),
    RequireMasksToggled(bool),
    ClassToggled(u32),
    AllClasses,
    NoClasses,
}

#[derive(Debug, Clone)]
pub(super) enum Outcome {
    OpenRequested,
    StopRequested,
    DialogRequested(u64),
    SettingsEdited(EditSchedule),
    FilterEdited(crate::generated::ExploreFilterUpdate),
}

pub(super) fn update(
    state: &super::state::State,
    model: &ExploreModel,
    settings: &mut SettingsModel,
    message: Message,
) -> Result<Outcome, String> {
    Ok(match message {
        Message::DeviceChanged(device) => {
            Outcome::SettingsEdited(settings_edit(settings, |draft| {
                crate::generated::edit_workflowsexploredeviceid(draft, device)
            })?)
        }
        Message::Loading(message) => {
            Outcome::SettingsEdited(crate::view::workflow::loading::update(
                crate::generated::FeatureId::Explore,
                settings,
                message,
            )?)
        }
        Message::OpenRequested => Outcome::OpenRequested,
        Message::StopRequested => Outcome::StopRequested,
        Message::DialogRequested(id) => Outcome::DialogRequested(id),
        Message::SourceSelected(source) => {
            Outcome::SettingsEdited(settings_edit(settings, |draft| {
                crate::generated::edit_workflowsexploredatasetsource(draft, source)
            })?)
        }
        Message::CustomSourceChanged(value) => {
            Outcome::SettingsEdited(settings_edit(settings, |draft| {
                crate::generated::edit_workflowsexplorecustomcompiledpath(draft, value)
            })?)
        }
        Message::MinimumInstancesChanged(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.minimuminstances = value.min(request.filter.maximuminstances);
            })?)
        }
        Message::MaximumInstancesChanged(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.maximuminstances = value.max(request.filter.minimuminstances);
            })?)
        }
        Message::OrderSelected(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.order = value;
            })?)
        }
        Message::ShuffleSeedChanged(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.shuffleseed = value
            })?)
        }
        Message::MinimumCompiledIndexChanged(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.minimumcompiledindex =
                    value.min(request.filter.maximumcompiledindex);
            })?)
        }
        Message::MaximumCompiledIndexChanged(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.maximumcompiledindex =
                    value.max(request.filter.minimumcompiledindex);
            })?)
        }
        Message::UnlimitedCompiledIndex => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.maximumcompiledindex = u64::MAX;
            })?)
        }
        Message::RequireBoxesToggled(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.requireboxes = value;
            })?)
        }
        Message::RequireMasksToggled(value) => {
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                request.filter.requiremasks = value;
            })?)
        }
        Message::ClassToggled(index) => {
            let class_count = model
                .snapshot
                .as_ref()
                .map_or(0, |snapshot| snapshot.dataset.classnames.len());
            if index as usize >= class_count {
                return Err("Explore class identity is outside the native catalog".to_owned());
            }
            Outcome::FilterEdited(filter_edit(state, model, |request| {
                toggle_selection(&mut request.filter.classselection, class_count, index);
            })?)
        }
        Message::AllClasses => Outcome::FilterEdited(filter_edit(state, model, |request| {
            request.filter.classselection =
                class_selection(crate::generated::ExploreClassSelectionMode::All);
        })?),
        Message::NoClasses => Outcome::FilterEdited(filter_edit(state, model, |request| {
            request.filter.classselection =
                class_selection(crate::generated::ExploreClassSelectionMode::None);
        })?),
    })
}

pub(super) fn view<'a>(
    state: &'a super::state::State,
    model: &'a ApplicationModel,
    settings_model: &'a SettingsModel,
) -> Element<'a, Message> {
    let snapshot = model.explore.snapshot.as_ref();
    let settings = settings_model
        .draft
        .as_ref()
        .map(|draft| &draft.workflows.explore);
    let filter = state.presented_filter(snapshot);
    let settings_available = settings_model.draft.is_some() && model.settings_edit_available();
    let mutation_available =
        !settings_model.has_local_edits() && model.explore_mutation_available();
    let dialog = custom_compiled_dialog();
    let last_compiled_index =
        snapshot.map_or(0, |value| value.dataset.imagecount.saturating_sub(1) as u64);

    let sources = crate::generated::EXPLORE_DATASET_SOURCE_VALUES
        .iter()
        .copied()
        .fold(column![].spacing(5), |column, source| {
            column.push(
                button(text(source_label(source)))
                    .on_press_maybe(settings_available.then_some(Message::SourceSelected(source)))
                    .style(
                        if settings.is_some_and(|value| value.datasetsource == source) {
                            crate::fluent_theme::button_selected
                        } else {
                            crate::fluent_theme::button_secondary
                        },
                    )
                    .width(Fill),
            )
        });
    let classes = class_checklist(
        snapshot,
        filter.as_ref().map(|filter| &filter.filter.classselection),
        mutation_available,
        Message::ClassToggled,
    );
    let custom_controls: Element<'a, Message> = if custom_controls_visible(settings) {
        column![
            container(
                text_input(
                    "Custom compiled dataset",
                    settings.map_or("", |value| value.customcompiledpath.as_str())
                )
                .on_input_maybe(settings_available.then_some(Message::CustomSourceChanged))
            )
            .id(super::CUSTOM_SOURCE_ID),
            container(
                button("Browse")
                    .on_press_maybe(dialog.and_then(|fact| {
                        model
                            .file_dialog_open_available(fact, crate::generated::FeatureId::Explore)
                            .then_some(Message::DialogRequested(fact.stable_field_id))
                    }))
                    .style(crate::fluent_theme::button_primary)
                    .width(Fill)
            )
            .id(super::CUSTOM_BROWSE_ID)
            .width(Fill),
        ]
        .spacing(5)
        .into()
    } else {
        space::vertical().height(0).into()
    };

    super::sidebar(
        super::DATASET_SCROLL_ID,
        "Dataset & filters",
        column![
            text("Dataset").size(20),
            sources,
            custom_controls,
            container(
                button(if snapshot.is_some_and(|value| value.ready) {
                    "Reopen Dataset"
                } else {
                    "Open Dataset"
                })
                .on_press_maybe(
                    (model.explore_open_available() && !settings_model.has_local_edits())
                        .then_some(Message::OpenRequested),
                )
                .style(crate::fluent_theme::button_primary)
                .width(Fill)
            )
            .id(super::OPEN_ID)
            .width(Fill),
            container(
                button("Stop")
                    .on_press_maybe(
                        model
                            .explore_stop_available()
                            .then_some(Message::StopRequested),
                    )
                    .style(crate::fluent_theme::button_danger)
                    .width(Fill)
            )
            .id(super::STOP_ID)
            .width(Fill),
            rule::horizontal(1),
            crate::view::workflow::loading::view(
                crate::generated::FeatureId::Explore,
                settings_model,
                settings_available
            )
            .map(Message::Loading),
            crate::view::workflow::fields::number_i32(
                "CUDA device",
                settings.map_or_else(
                    || crate::generated::default_workflowsexploredeviceid()
                        .expect("native device default"),
                    |value| value.deviceid
                ),
                crate::generated::constraint_workflowsexploredeviceid(),
                settings_available,
                Message::DeviceChanged
            ),
            rule::horizontal(1),
            text("Sample filters").size(18),
            checkbox(
                filter
                    .as_ref()
                    .is_some_and(|request| request.filter.requireboxes)
            )
            .label("Require boxes")
            .on_toggle_maybe(mutation_available.then_some(Message::RequireBoxesToggled)),
            checkbox(
                filter
                    .as_ref()
                    .is_some_and(|request| request.filter.requiremasks)
            )
            .label("Require masks")
            .on_toggle_maybe(mutation_available.then_some(Message::RequireMasksToggled)),
            instance_number(
                "Minimum instances",
                filter
                    .as_ref()
                    .map_or(0, |request| request.filter.minimuminstances),
                crate::generated::constraint_workflowsexploremininstances(),
                mutation_available,
                Message::MinimumInstancesChanged,
            ),
            text("Order & range").size(18),
            container(
                row![
                    order_button(
                        "Sequential",
                        crate::generated::ExploreOrder::Sequential,
                        filter.as_ref().map(|request| request.filter.order),
                        mutation_available,
                    ),
                    container(order_button(
                        "Shuffled",
                        crate::generated::ExploreOrder::Shuffled,
                        filter.as_ref().map(|request| request.filter.order),
                        mutation_available,
                    ))
                    .id(super::ORDER_SHUFFLED_ID),
                ]
                .spacing(5)
            )
            .id(super::ORDER_CONTROL_ID),
            labeled_number(
                "Shuffle seed",
                crate::generated::constraint_workflowsexploreshuffleseed().stable_field_id,
                filter
                    .as_ref()
                    .map_or(0, |request| request.filter.shuffleseed),
                u64::MAX,
                mutation_available,
                Message::ShuffleSeedChanged,
            ),
            container(
                column![
                    labeled_number(
                        "First compiled index",
                        crate::generated::constraint_workflowsexploremincompiledindex()
                            .stable_field_id,
                        filter
                            .as_ref()
                            .map_or(0, |request| request.filter.minimumcompiledindex)
                            .min(last_compiled_index),
                        last_compiled_index,
                        mutation_available,
                        Message::MinimumCompiledIndexChanged,
                    ),
                    labeled_number(
                        "Last compiled index",
                        crate::generated::constraint_workflowsexploremaxcompiledindex()
                            .stable_field_id,
                        filter
                            .as_ref()
                            .map_or(last_compiled_index, |request| request
                                .filter
                                .maximumcompiledindex
                                .min(last_compiled_index)),
                        last_compiled_index,
                        mutation_available,
                        Message::MaximumCompiledIndexChanged,
                    ),
                    button("Use complete dataset").on_press_maybe(
                        mutation_available.then_some(Message::UnlimitedCompiledIndex)
                    ),
                    container(
                        button("Start at 1").on_press_maybe(
                            (mutation_available && last_compiled_index >= 1)
                                .then_some(Message::MinimumCompiledIndexChanged(1)),
                        ),
                    )
                    .id(super::RANGE_START_ONE_ID),
                ]
                .spacing(5)
            )
            .id(super::RANGE_CONTROL_ID),
            instance_number(
                "Maximum instances",
                filter
                    .as_ref()
                    .map_or(0, |request| request.filter.maximuminstances),
                crate::generated::constraint_workflowsexploremaxinstances(),
                mutation_available,
                Message::MaximumInstancesChanged,
            ),
            rule::horizontal(1),
            row![
                text("Included classes").size(18),
                space::horizontal(),
                button("All").on_press_maybe(mutation_available.then_some(Message::AllClasses)),
                button("None").on_press_maybe(mutation_available.then_some(Message::NoClasses)),
            ]
            .align_y(Center),
            classes,
        ]
        .spacing(9),
    )
}

pub(super) fn class_selection(
    mode: crate::generated::ExploreClassSelectionMode,
) -> crate::generated::ExploreClassSelection {
    crate::generated::ExploreClassSelection {
        mode,
        classes: Vec::new(),
    }
}

pub(crate) fn selection_contains(
    selection: &crate::generated::ExploreClassSelection,
    index: u32,
) -> bool {
    match selection.mode {
        crate::generated::ExploreClassSelectionMode::All => true,
        crate::generated::ExploreClassSelectionMode::None => false,
        crate::generated::ExploreClassSelectionMode::Subset => selection.classes.contains(&index),
    }
}

pub(super) fn class_checklist<'a, Message: Clone + 'a>(
    snapshot: Option<&crate::generated::ExploreSnapshot>,
    selection: Option<&crate::generated::ExploreClassSelection>,
    enabled: bool,
    on_toggle: impl Fn(u32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let (Some(snapshot), Some(selection)) = (snapshot, selection) else {
        return column![text("Class names appear after opening a dataset.")].into();
    };
    snapshot
        .dataset
        .classnames
        .iter()
        .enumerate()
        .fold(column![].spacing(3), |column, (index, name)| {
            let index = index as u32;
            column.push(
                checkbox(selection_contains(selection, index))
                    .label(name.value.clone())
                    .on_toggle_maybe(enabled.then_some(move |_| on_toggle(index))),
            )
        })
        .into()
}

pub(super) fn toggle_selection(
    selection: &mut crate::generated::ExploreClassSelection,
    class_count: usize,
    index: u32,
) {
    let mut classes: Vec<u32> = match selection.mode {
        crate::generated::ExploreClassSelectionMode::All => (0..class_count as u32).collect(),
        crate::generated::ExploreClassSelectionMode::None => Vec::new(),
        crate::generated::ExploreClassSelectionMode::Subset => selection.classes.clone(),
    };
    if let Some(position) = classes.iter().position(|value| *value == index) {
        classes.remove(position);
    } else {
        classes.push(index);
        classes.sort_unstable();
    }
    *selection = if classes.is_empty() {
        class_selection(crate::generated::ExploreClassSelectionMode::None)
    } else if classes.len() == class_count {
        class_selection(crate::generated::ExploreClassSelectionMode::All)
    } else {
        crate::generated::ExploreClassSelection {
            mode: crate::generated::ExploreClassSelectionMode::Subset,
            classes,
        }
    };
}

fn labeled_number(
    label: &'static str,
    id: u64,
    value: u64,
    maximum: u64,
    available: bool,
    message: fn(u64) -> Message,
) -> Element<'static, Message> {
    let input: Element<'static, Message> = if available {
        iced_aw::number_input(&value, 0..=maximum, message)
            .id(id.to_string())
            .ignore_buttons(true)
            .ignore_scroll(true)
            .width(Fill)
            .into()
    } else {
        text(value).into()
    };
    column![text(label), input,].spacing(3).into()
}

fn order_button(
    label: &'static str,
    value: crate::generated::ExploreOrder,
    selected: Option<crate::generated::ExploreOrder>,
    available: bool,
) -> Element<'static, Message> {
    button(label)
        .on_press_maybe(available.then_some(Message::OrderSelected(value)))
        .style(if selected == Some(value) {
            crate::fluent_theme::button_selected
        } else {
            crate::fluent_theme::button_secondary
        })
        .into()
}

fn instance_number<'a>(
    label: &'static str,
    value: u32,
    constraint: crate::generated::SettingsLeafConstraint,
    available: bool,
    on_change: fn(u32) -> Message,
) -> Element<'a, Message> {
    let minimum = constraint.minimum.unwrap_or(0.0).max(0.0) as u32;
    let maximum = constraint
        .maximum
        .filter(|value| value.is_finite() && *value >= minimum as f64)
        .map_or(u32::MAX, |value| value as u32);
    let input: Element<'a, Message> = if available {
        iced_aw::number_input(&value, minimum..=maximum, on_change)
            .id(constraint.stable_field_id.to_string())
            .ignore_buttons(true)
            .ignore_scroll(true)
            .width(Fill)
            .into()
    } else {
        text("Available after Explore is ready").size(11).into()
    };
    column![text(label), input,].spacing(3).into()
}

fn source_label(source: crate::generated::ExploreDatasetSource) -> &'static str {
    match source {
        crate::generated::ExploreDatasetSource::Train => "Train",
        crate::generated::ExploreDatasetSource::Validation => "Validation",
        crate::generated::ExploreDatasetSource::Test => "Test",
        crate::generated::ExploreDatasetSource::Custom => "Custom",
    }
}

fn custom_controls_visible(settings: Option<&crate::generated::ExploreViewState>) -> bool {
    settings
        .is_some_and(|value| value.datasetsource == crate::generated::ExploreDatasetSource::Custom)
}

fn custom_compiled_dialog() -> Option<&'static crate::generated::FileDialogFact> {
    let stable_field_id =
        crate::generated::constraint_workflowsexplorecustomcompiledpath().stable_field_id;
    crate::generated::FILE_DIALOGS
        .iter()
        .find(|fact| fact.stable_field_id == stable_field_id)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view::settings::installed_settings_model;
    use crate::view_model::test_support::explore_snapshot;

    fn ready_explore_model() -> ExploreModel {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        ExploreModel {
            snapshot: Some(snapshot),
            ..ExploreModel::default()
        }
    }

    #[test]
    fn live_filter_edit_is_one_direct_native_request() {
        let mut settings = installed_settings_model();
        let model = ready_explore_model();
        let state = super::super::state::State::default();
        let Outcome::FilterEdited(request) = update(
            &state,
            &model,
            &mut settings,
            Message::MinimumInstancesChanged(4),
        )
        .unwrap() else {
            panic!("direct Explore filter");
        };
        assert_eq!(request.filter.minimuminstances, 4);
        assert_eq!(settings.queued_len(), 0);
    }

    #[test]
    fn order_seed_and_range_edits_share_the_generated_filter_request() {
        let mut settings = installed_settings_model();
        let model = ready_explore_model();
        let state = super::super::state::State::default();
        let Outcome::FilterEdited(order) = update(
            &state,
            &model,
            &mut settings,
            Message::OrderSelected(crate::generated::ExploreOrder::Shuffled),
        )
        .unwrap() else {
            panic!("order request");
        };
        assert_eq!(order.filter.order, crate::generated::ExploreOrder::Shuffled);
        let Outcome::FilterEdited(seed) = update(
            &state,
            &model,
            &mut settings,
            Message::ShuffleSeedChanged(73),
        )
        .unwrap() else {
            panic!("seed request");
        };
        assert_eq!(seed.filter.shuffleseed, 73);
        let Outcome::FilterEdited(range) = update(
            &state,
            &model,
            &mut settings,
            Message::MaximumCompiledIndexChanged(19),
        )
        .unwrap() else {
            panic!("range request");
        };
        assert_eq!(range.filter.maximumcompiledindex, 19);
    }

    #[test]
    fn integer_filter_edits_preserve_unsigned_precision_and_crossed_bounds() {
        let mut settings = installed_settings_model();
        let mut model = ready_explore_model();
        let state = super::super::state::State::default();
        for value in [(1_u64 << 53) + 1, u64::MAX - 1, u64::MAX] {
            for message in [
                Message::ShuffleSeedChanged(value),
                Message::MaximumCompiledIndexChanged(value),
            ] {
                let Outcome::FilterEdited(request) =
                    update(&state, &model, &mut settings, message.clone()).unwrap()
                else {
                    panic!("typed filter request");
                };
                assert_eq!(
                    match message {
                        Message::ShuffleSeedChanged(_) => request.filter.shuffleseed,
                        _ => request.filter.maximumcompiledindex,
                    },
                    value
                );
            }
        }
        model.snapshot.as_mut().unwrap().filter.minimuminstances = 4;
        model.snapshot.as_mut().unwrap().filter.maximuminstances = 8;
        for (message, minimum, maximum) in [
            (Message::MinimumInstancesChanged(10_000), 8, 8),
            (Message::MaximumInstancesChanged(0), 4, 4),
            (Message::MinimumInstancesChanged(0), 0, 8),
            (Message::MaximumInstancesChanged(10_000), 4, 10_000),
        ] {
            let Outcome::FilterEdited(request) =
                update(&state, &model, &mut settings, message).unwrap()
            else {
                panic!("typed filter request");
            };
            assert_eq!(
                (
                    request.filter.minimuminstances,
                    request.filter.maximuminstances
                ),
                (minimum, maximum)
            );
        }
        let Outcome::FilterEdited(request) = update(
            &state,
            &model,
            &mut settings,
            Message::UnlimitedCompiledIndex,
        )
        .unwrap() else {
            panic!("typed filter request");
        };
        assert_eq!(request.filter.maximumcompiledindex, u64::MAX);
    }

    #[test]
    fn custom_source_controls_are_conditional() {
        let mut settings = installed_settings_model();
        assert!(!custom_controls_visible(Some(
            &settings.draft.as_ref().unwrap().workflows.explore
        )));
        settings_edit(&mut settings, |draft| {
            crate::generated::edit_workflowsexploredatasetsource(
                draft,
                crate::generated::ExploreDatasetSource::Custom,
            )
        })
        .unwrap();
        assert!(custom_controls_visible(Some(
            &settings.draft.as_ref().unwrap().workflows.explore
        )));
    }

    #[test]
    fn custom_dataset_dialog_uses_the_generated_settings_identity() {
        let constraint = crate::generated::constraint_workflowsexplorecustomcompiledpath();
        let dialog = custom_compiled_dialog().expect("custom compiled dataset dialog");
        assert_eq!(dialog.stable_field_id, constraint.stable_field_id);
        assert!(
            dialog
                .workflows
                .contains(&crate::generated::FeatureId::Explore)
        );
    }
}
