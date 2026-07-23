use crate::app::App;
use crate::generated::WORKFLOW_LABELS;
use crate::message::Message;
use crate::model::BridgePhase;
use iced::widget::{Row, button, container, row, space, text};
use iced::{Center, Element, Fill};

pub fn view(app: &App) -> Element<'_, Message> {
    let shell = app.drafts.shell_style();
    let mut tabs: Row<'_, Message> = row![].spacing(shell.field_spacing).align_y(Center);
    for &(workflow, label) in WORKFLOW_LABELS {
        let tab = button(text(label).size(shell.primary_font_size))
            .on_press(Message::SelectWorkflow(workflow))
            .style(if workflow == app.selected_workflow {
                iced::widget::button::primary
            } else {
                iced::widget::button::secondary
            });
        tabs = tabs.push(tab);
    }

    let bridge = match app.bridge.phase {
        BridgePhase::Idle => "idle",
        BridgePhase::Polling => "polling",
        BridgePhase::Dispatch => "dispatch",
    };

    container(
        row![
            tabs,
            space::horizontal(),
            text(format!("transport: {}", app.transport_status.label()))
                .size(shell.secondary_font_size),
            text(format!("bridge: {bridge}")).size(shell.secondary_font_size),
            text(if app.ready() {
                "WebGPU ready"
            } else {
                "WebGPU pending"
            })
            .size(shell.secondary_font_size),
            button("Settings").on_press(Message::ToggleSettings)
        ]
        .spacing(shell.section_spacing)
        .align_y(Center),
    )
    .padding([shell.control_padding_y, shell.control_padding_x])
    .width(Fill)
    .center_y(Fill)
    .style(iced::widget::container::secondary)
    .into()
}
