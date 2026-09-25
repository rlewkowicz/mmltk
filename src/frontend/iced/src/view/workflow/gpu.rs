use crate::fluent_theme::Element;
use crate::generated::{self, FeatureId};
use crate::view_model::selected_gpu_ordinals;
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use crate::view_model::ApplicationModel;
use iced::widget::{checkbox, column, container, text};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Message(pub i32);


pub fn update(
    feature: FeatureId,
    settings: &mut SettingsModel,
    Message(device): Message,
) -> Result<EditSchedule, String> {
    if device < 0 {
        return Err("CUDA device ID must be nonnegative".into());
    }
    if feature == FeatureId::Train {
        let request = &settings
            .draft
            .as_ref()
            .ok_or("Settings are unavailable")?
            .workflows
            .train
            .request;
        let mut devices = request.deviceids.clone();
        let mut nodes: Vec<_> = devices
            .iter()
            .enumerate()
            .map(|(rank, _)| {
                request
                    .numanodes
                    .get(rank)
                    .copied()
                    .unwrap_or(if devices.len() == 1 {
                        request.numanode
                    } else {
                        -1
                    })
            })
            .collect();
        if let Some(rank) = devices.iter().position(|value| *value == device) {
            if devices.len() == 1 {
                return Err("Select at least one training GPU".into());
            }
            devices.remove(rank);
            nodes.remove(rank);
        } else {
            let constraint = generated::constraint_workflowstrainrequestdeviceids();
            if devices.len() >= constraint.maximum_items {
                return Err("Training GPU selection exceeds capacity".into());
            }
            devices.push(device);
            nodes.push(-1);
        }
        return settings.edit_group(EditCadence::Immediate, |draft| {
            [
                generated::edit_workflowstrainrequestdeviceids(draft, devices),
                generated::edit_workflowstrainrequestnumanodes(draft, nodes),
                generated::edit_workflowstrainrequestnumanode(draft, -1),
            ]
        });
    }
    settings.edit(EditCadence::Immediate, |draft| match feature {
        FeatureId::Validate => generated::edit_workflowsvalidaterequestdeviceid(draft, device),
        FeatureId::Predict => generated::edit_workflowspredictrequestdeviceid(draft, device),
        FeatureId::Export => generated::edit_workflowsexportstatedeviceid(draft, device),
        _ => unreachable!(),
    })
}

pub fn view<'a>(
    feature: FeatureId,
    model: &'a ApplicationModel,
    settings: &'a SettingsModel,
) -> Element<'a, Message> {
    let name = match feature {
        FeatureId::Train => "train",
        FeatureId::Validate => "validate",
        FeatureId::Predict => "predict",
        FeatureId::Export => "export",
        _ => unreachable!(),
    };
    let selection = settings
        .draft
        .as_ref()
        .map(|state| selected_gpu_ordinals(feature, state))
        .unwrap_or_default();
    let inventory = model
        .settings_snapshot
        .as_ref()
        .map(|state| state.cudadevices.as_slice())
        .unwrap_or_default();
    let enabled = model.settings_edit_available() && !model.primary_action_active(feature);
    let mut content = column![].spacing(super::FIELD_SPACING);
    for device in inventory {
        let chosen = selection.contains(&device.ordinal);
        let editable =
            enabled && !(chosen && (feature != FeatureId::Train || selection.len() == 1));
        let label = format!(
            "GPU {} · {} · {:.2} GiB",
            device.ordinal,
            device.name,
            device.totalvram as f64 / 1_073_741_824.0
        );
        content = content.push(
            container(
                checkbox(chosen)
                    .label(label)
                    .on_toggle_maybe(editable.then_some(move |_| Message(device.ordinal))),
            )
            .id(format!("{name}.gpu.device.{}", device.ordinal)),
        );
    }
    for device in selection {
        if inventory.iter().all(|fact| fact.ordinal != *device) {
            let ordinal = *device;
            content = content.push(
                container(
                    checkbox(true)
                        .label(format!("GPU {ordinal} · unavailable"))
                        .on_toggle_maybe(
                            (enabled && feature == FeatureId::Train && selection.len() > 1)
                                .then_some(move |_| Message(ordinal)),
                        ),
                )
                .id(format!("{name}.gpu.device.{ordinal}")),
            );
        }
    }
    if feature == FeatureId::Train {
        content = content.push(
            text(format!(
                "Rank order: {}",
                selection
                    .iter()
                    .map(i32::to_string)
                    .collect::<Vec<_>>()
                    .join(", ")
            ))
            .size(12),
        );
    }
    container(crate::view::shared::card("GPU", "", content))
        .id(format!("{name}.card.gpu"))
        .width(iced::Fill)
        .into()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn selections_preserve_drafts_rank_order_and_locality() {
        let mut settings = crate::view::settings::installed_settings_model();
        settings
            .edit(EditCadence::Debounced, |draft| {
                generated::edit_workflowstrainrequestbatchsize(draft, 2)
            })
            .unwrap();
        let train = &mut settings.draft.as_mut().unwrap().workflows.train.request;
        train.deviceids = vec![3];
        train.numanode = 2;
        update(FeatureId::Train, &mut settings, Message(7)).unwrap();
        update(FeatureId::Train, &mut settings, Message(1)).unwrap();
        update(FeatureId::Train, &mut settings, Message(7)).unwrap();
        let train = &settings.draft.as_ref().unwrap().workflows.train.request;
        assert_eq!(train.deviceids, vec![3, 1]);
        assert_eq!(train.numanodes, vec![2, -1]);
        assert_eq!(train.numanode, -1);
        assert_eq!(train.batchsize, 2);
        for feature in [FeatureId::Validate, FeatureId::Predict, FeatureId::Export] {
            update(feature, &mut settings, Message(5)).unwrap();
            assert_eq!(selected_gpu_ordinals(feature, settings.draft.as_ref().unwrap()), vec![5]);
        }
        update(FeatureId::Train, &mut settings, Message(1)).unwrap();
        assert!(update(FeatureId::Train, &mut settings, Message(3)).is_err());
        assert_eq!(
            settings
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .numanodes,
            vec![2]
        );
        assert!(settings.has_local_edits());
    }
    #[test]
    fn cards_keep_native_device_identity_and_disable_active_run_edits() {
        use iced::advanced::{Layout, Shell, layout, renderer::Headless, widget};
        use iced::{Event, Point, Rectangle, Size, mouse, window};
        let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(),
            Some("wgpu"),
        ))
        .expect("GPU cards require the container renderer");
        struct Target {
            id: widget::Id,
            bounds: Option<Rectangle>,
        }
        impl widget::Operation for Target {
            fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn widget::Operation)) {
                operate(self);
            }
            fn container(&mut self, id: Option<&widget::Id>, bounds: Rectangle) {
                if id == Some(&self.id) {
                    self.bounds = Some(bounds);
                }
            }
        }
        for (feature, name) in [
            (FeatureId::Train, "train"),
            (FeatureId::Validate, "validate"),
            (FeatureId::Predict, "predict"),
            (FeatureId::Export, "export"),
        ] {
            for active in [false, true] {
                for width in [160.0, 360.0] {
                    let mut model = crate::view_model::test_support::bootstrapped();
                    model.settings_snapshot.as_mut().unwrap().cudadevices = vec![
                        generated::CudaDeviceFact {
                            ordinal: 0,
                            name: "First GPU".into(),
                            totalvram: 12 << 30,
                        },
                        generated::CudaDeviceFact {
                            ordinal: 1,
                            name: "Second GPU".into(),
                            totalvram: 24 << 30,
                        },
                    ];
                    match feature {
                        FeatureId::Train => {
                            model.workflow.training.as_mut().unwrap().local.active = active
                        }
                        FeatureId::Validate => {
                            model.workflow.validation.as_mut().unwrap().operation.active = active
                        }
                        FeatureId::Predict => {
                            model.predict_snapshot.as_mut().unwrap().operation.active = active
                        }
                        FeatureId::Export => {
                            model.workflow.export.as_mut().unwrap().active = active
                        }
                        _ => unreachable!(),
                    }
                    let settings = crate::view::settings::installed_settings_model();
                    let mut element = view(feature, &model, &settings);
                    let mut tree = widget::Tree::new(&element);
                    tree.diff(&mut element);
                    let viewport = Rectangle::new(Point::ORIGIN, Size::new(width, 1000.0));
                    let node = element.as_widget_mut().layout(
                        &mut tree,
                        &renderer,
                        &layout::Limits::new(Size::ZERO, viewport.size()),
                    );
                    let mut target = Target {
                        id: widget::Id::from(format!("{name}.gpu.device.1")),
                        bounds: None,
                    };
                    element.as_widget_mut().operate(
                        &mut tree,
                        Layout::new(&node),
                        &renderer,
                        &mut target,
                    );
                    let bounds = target
                        .bounds
                        .expect("native ordinal retains its stable widget identity");
                    assert!(bounds.width <= width);
                    let cursor =
                        mouse::Cursor::Available(Point::new(bounds.x + 8.0, bounds.y + 8.0));
                    let mut messages = Vec::new();
                    for event in [
                        mouse::Event::ButtonPressed(mouse::Button::Left),
                        mouse::Event::ButtonReleased(mouse::Button::Left),
                    ] {
                        let mut shell = Shell::new(
                            &window::Headless,
                            iced_runtime::core::shell::Waker::new(|| {}),
                            &mut messages,
                        );
                        element.as_widget_mut().update(
                            &mut tree,
                            &Event::Mouse(event),
                            Layout::new(&node),
                            cursor,
                            &renderer,
                            &mut shell,
                            &viewport,
                        );
                    }
                    assert_eq!(messages, if active { vec![] } else { vec![Message(1)] });
                }
            }
        }
    }
}
