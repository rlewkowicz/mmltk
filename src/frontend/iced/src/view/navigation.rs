use crate::fluent_theme::Element;
use crate::generated::FeatureId;
use crate::view_model::Typography;
use iced::widget::{button, container, row, space, text};
use iced::{Center, Fill};

pub const ORDER: [FeatureId; 7] = [
    FeatureId::Train,
    FeatureId::Validate,
    FeatureId::Predict,
    FeatureId::Live,
    FeatureId::Annotate,
    FeatureId::Export,
    FeatureId::Explore,
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Message {
    PageSelected(FeatureId),
    SettingsRequested,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    PageSelected(FeatureId),
    SettingsRequested,
}

pub fn update(message: Message) -> Outcome {
    match message {
        Message::PageSelected(page) => Outcome::PageSelected(page),
        Message::SettingsRequested => Outcome::SettingsRequested,
    }
}

pub fn view(
    selected: FeatureId,
    connection: &'static str,
    typography: Typography,
) -> Element<'static, Message> {
    let tabs = ORDER
        .into_iter()
        .fold(row![].spacing(6).align_y(Center), |tabs, feature| {
            let tab = button(text(label(feature)).size(typography.primary))
                .on_press(Message::PageSelected(feature))
                .style(if feature == selected {
                    crate::fluent_theme::button_selected
                } else {
                    crate::fluent_theme::button_secondary
                });
            tabs.push(container(tab).id(stable_id(feature)))
        });
    container(
        row![
            text("mmltk").size(typography.primary * 1.8),
            tabs,
            space::horizontal(),
            text(connection)
                .size(typography.secondary)
                .style(crate::fluent_theme::text_secondary),
            container(
                button("Settings")
                    .on_press(Message::SettingsRequested)
                    .style(crate::fluent_theme::button_secondary)
            )
            .id("navigation.settings"),
        ]
        .spacing(16)
        .align_y(Center),
    )
    .padding([6, 10])
    .width(Fill)
    .into()
}

pub const fn label(feature: FeatureId) -> &'static str {
    match feature {
        FeatureId::Train => "Train",
        FeatureId::Validate => "Validate",
        FeatureId::Predict => "Predict",
        FeatureId::Live => "Live",
        FeatureId::Annotate => "Annotate",
        FeatureId::Export => "Export",
        FeatureId::Explore => "Explore",
    }
}

pub const fn stable_id(feature: FeatureId) -> &'static str {
    match feature {
        FeatureId::Train => "navigation.train",
        FeatureId::Validate => "navigation.validate",
        FeatureId::Predict => "navigation.predict",
        FeatureId::Live => "navigation.live",
        FeatureId::Annotate => "navigation.annotate",
        FeatureId::Export => "navigation.export",
        FeatureId::Explore => "navigation.explore",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn navigation_reduces_to_domain_page_outcomes() {
        assert_eq!(
            update(Message::PageSelected(FeatureId::Explore)),
            Outcome::PageSelected(FeatureId::Explore)
        );
        assert_eq!(
            update(Message::SettingsRequested),
            Outcome::SettingsRequested
        );
    }

    #[test]
    fn visual_inventory_is_an_exhaustive_generated_feature_permutation() {
        assert_eq!(ORDER.len(), crate::generated::FEATURE_ID_VALUES.len());
        for feature in crate::generated::FEATURE_ID_VALUES {
            assert_eq!(
                ORDER
                    .iter()
                    .filter(|candidate| *candidate == feature)
                    .count(),
                1
            );
            assert!(!label(*feature).is_empty());
            assert!(stable_id(*feature).starts_with("navigation."));
        }
    }
}
