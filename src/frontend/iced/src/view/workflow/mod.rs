pub mod fields;
pub mod loading;
pub mod model_card;
pub mod progress;

use crate::fluent_theme::Element;
use iced::widget::{button, column, container, text};
use iced::{Fill, Font, Length, Padding};

pub const SIDEBAR_PORTION: f32 = 0.19;
pub const WORKSPACE_PORTION: f32 = 0.62;
pub const CARD_PADDING: f32 = 10.0;
pub const SECTION_SPACING: f32 = 10.0;
pub const FIELD_SPACING: f32 = 4.0;
pub const PRIMARY_ACTION_HEIGHT: f32 = 48.0;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Region {
    Setup,
    Center,
    Workspace,
    Advanced,
    Diagnostics,
    PrimaryProgress,
    PrimaryAction,
    Status,
}

const SHELL_REGIONS: [Region; 3] = [Region::Setup, Region::Center, Region::Diagnostics];
const CENTER_REGIONS: [Region; 2] = [Region::Workspace, Region::Advanced];
const AUDIT_REGIONS: [Region; 8] = [
    Region::Setup,
    Region::Center,
    Region::Workspace,
    Region::Advanced,
    Region::Diagnostics,
    Region::PrimaryProgress,
    Region::PrimaryAction,
    Region::Status,
];
pub fn ordinary_pages() -> impl Iterator<Item = crate::generated::FeatureId> {
    crate::view::navigation::ORDER
        .into_iter()
        .filter(|page| *page != crate::generated::FeatureId::Explore)
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Layout {
    pub setup_width: f32,
    pub workspace_width: f32,
    pub diagnostics_width: f32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Composition {
    page: crate::generated::FeatureId,
    layout: Layout,
}

impl Composition {
    pub const fn new(page: crate::generated::FeatureId, page_width: f32) -> Self {
        Self {
            page,
            layout: Layout {
                setup_width: page_width * SIDEBAR_PORTION,
                workspace_width: page_width * WORKSPACE_PORTION,
                diagnostics_width: page_width * SIDEBAR_PORTION,
            },
        }
    }

    pub const fn layout(self) -> Layout {
        self.layout
    }

    pub const fn center_width(self) -> f32 {
        self.layout.workspace_width
    }

    pub const fn shell_regions(self) -> &'static [Region] {
        &SHELL_REGIONS
    }

    pub const fn center_regions(self) -> &'static [Region] {
        &CENTER_REGIONS
    }

    pub const fn audit_regions(self) -> &'static [Region] {
        &AUDIT_REGIONS
    }

    pub const fn stable_id(self, region: Region) -> &'static str {
        match region {
            Region::Setup => "workflow.setup",
            Region::Center => "workflow.workspace_and_advanced",
            Region::Workspace => "workflow.workspace",
            Region::Advanced => "workflow.advanced",
            Region::Diagnostics => "workflow.diagnostics",
            Region::PrimaryProgress => match self.page {
                crate::generated::FeatureId::Train => "train.primary.progress",
                crate::generated::FeatureId::Validate => "validate.primary.progress",
                crate::generated::FeatureId::Predict => "predict.primary.progress",
                crate::generated::FeatureId::Live => "live.primary.progress",
                crate::generated::FeatureId::Annotate => "annotation.save.progress",
                crate::generated::FeatureId::Export => "export.primary.progress",
                crate::generated::FeatureId::Explore => "explore.open.progress",
            },
            Region::PrimaryAction => match self.page {
                crate::generated::FeatureId::Train => "train.primary",
                crate::generated::FeatureId::Validate => "validate.primary",
                crate::generated::FeatureId::Predict => "predict.primary",
                crate::generated::FeatureId::Live => "live.primary",
                crate::generated::FeatureId::Annotate => "annotation.save",
                crate::generated::FeatureId::Export => "export.primary",
                crate::generated::FeatureId::Explore => "explore.open",
            },
            Region::Status => match self.page {
                crate::generated::FeatureId::Train => "train.status",
                crate::generated::FeatureId::Validate => "validate.status",
                crate::generated::FeatureId::Predict => "predict.status",
                crate::generated::FeatureId::Live => "live.status",
                crate::generated::FeatureId::Annotate => "annotation.status",
                crate::generated::FeatureId::Export => "export.status",
                crate::generated::FeatureId::Explore => "explore.card.status",
            },
        }
    }

    pub fn next_ordinary_page(self) -> Option<crate::generated::FeatureId> {
        let mut pages = ordinary_pages();
        pages.find(|page| *page == self.page)?;
        pages.next()
    }
}

pub struct Regions<'a, Message> {
    page: crate::generated::FeatureId,
    setup: Element<'a, Message>,
    workspace: Element<'a, Message>,
    advanced: Element<'a, Message>,
    diagnostics: Element<'a, Message>,
}

impl<'a, Message: 'a> Regions<'a, Message> {
    pub fn new(
        page: crate::generated::FeatureId,
        setup: Element<'a, Message>,
        workspace: Element<'a, Message>,
        advanced: Element<'a, Message>,
        diagnostics: Element<'a, Message>,
    ) -> Self {
        Self {
            page,
            setup,
            workspace,
            advanced,
            diagnostics,
        }
    }

    pub fn render(self, page_width: f32) -> Element<'a, Message> {
        view(page_width, self)
    }
}

pub fn workspace<'a, Message: 'a>(
    surface: Option<crate::presentation_surface::Surface>,
    settings: &'a crate::view::settings::SettingsModel,
    settings_enabled: bool,
    page: crate::generated::FeatureId,
    page_width: f32,
    map: impl Fn(crate::view::workspace::Message) -> Message + 'a,
    input: crate::workspace_input::Binding,
) -> Element<'a, Message> {
    let aspect = settings.draft.as_ref().map_or(
        crate::generated::WorkspaceAspectRatio::Widescreen,
        |draft| draft.ui.workspaceaspectratio,
    );
    crate::view::workspace::view(
        surface,
        aspect,
        settings_enabled,
        Composition::new(page, page_width).center_width(),
        input,
        crate::workspace_fps::enabled(settings),
    )
    .map(map)
}

pub fn update_workspace(
    settings: &mut crate::view::settings::SettingsModel,
    message: crate::view::workspace::Message,
) -> Result<Option<crate::view::settings::EditSchedule>, String> {
    match crate::view::workspace::update(message) {
        crate::view::workspace::Outcome::Gesture(_) => Ok(None),
        crate::view::workspace::Outcome::AspectSelected(aspect) => {
            crate::view::workspace::edit_aspect(settings, aspect).map(Some)
        }
    }
}

pub fn view<'a, Message: 'a>(
    page_width: f32,
    regions: Regions<'a, Message>,
) -> Element<'a, Message> {
    let composition = Composition::new(regions.page, page_width);
    let layout = composition.layout();
    let setup = iced::widget::keyed_column([(regions.page, regions.setup)]);
    let mut workspace = Some(regions.workspace);
    let mut advanced = Some(iced::widget::keyed_column([(
        regions.page,
        regions.advanced,
    )]));
    let center_content = composition.center_regions().iter().fold(
        iced::widget::Column::new()
            .spacing(SECTION_SPACING)
            .width(Fill),
        |column, region| match region {
            Region::Workspace => column.push(
                container(workspace.take().expect("one workflow workspace"))
                    .id(composition.stable_id(*region)),
            ),
            Region::Advanced => column.push(
                container(advanced.take().expect("one workflow Advanced region"))
                    .id(composition.stable_id(*region)),
            ),
            _ => unreachable!("center composition contains only center regions"),
        },
    );
    let diagnostics_content: Element<'a, Message> = container(regions.diagnostics)
        .id(composition.stable_id(Region::Status))
        .into();
    let diagnostics = iced::widget::keyed_column([(regions.page, diagnostics_content)]);
    let sidebar = |region, content, width| {
        container(content)
            .id(composition.stable_id(region))
            .padding(Padding {
                right: CARD_PADDING,
                bottom: CARD_PADDING,
                left: CARD_PADDING,
                ..Padding::ZERO
            })
            .width(Length::Fixed(width))
            .height(Length::Shrink)
            .style(crate::fluent_theme::container_sidebar)
    };
    let center = container(center_content)
        .id(composition.stable_id(Region::Center))
        .padding(Padding {
            right: CARD_PADDING,
            bottom: CARD_PADDING,
            left: CARD_PADDING,
            ..Padding::ZERO
        })
        .width(Length::Fixed(layout.workspace_width))
        .height(Length::Shrink)
        .style(crate::fluent_theme::container_sidebar);

    let mut setup = Some(sidebar(Region::Setup, setup, layout.setup_width));
    let mut center = Some(center);
    let mut diagnostics = Some(sidebar(
        Region::Diagnostics,
        diagnostics,
        layout.diagnostics_width,
    ));
    composition
        .shell_regions()
        .iter()
        .fold(
            iced::widget::Row::new()
                .spacing(0)
                .width(Length::Fixed(page_width))
                .height(Length::Shrink),
            |row, region| match region {
                Region::Setup => row.push(setup.take().expect("one setup region")),
                Region::Center => row.push(center.take().expect("one center region")),
                Region::Diagnostics => {
                    row.push(diagnostics.take().expect("one diagnostics region"))
                }
                _ => unreachable!("workflow shell contains only shell regions"),
            },
        )
        .into()
}

pub fn primary_action<'a, Message: Clone + 'a>(
    page: crate::generated::FeatureId,
    label: &'static str,
    on_press: Option<Message>,
    progress: Element<'a, Message>,
) -> Element<'a, Message> {
    let composition = Composition::new(page, 0.0);
    let label = text(label)
        .font(Font::new("Bitstream Vera Sans").weight(iced::font::Weight::Bold))
        .size(16)
        .width(Fill)
        .height(Fill)
        .align_x(iced::Center)
        .align_y(iced::Center);
    let action = button(label)
        .on_press_maybe(on_press)
        .style(crate::fluent_theme::button_workflow_primary)
        .padding(0)
        .width(Fill)
        .height(Fill);
    let framed = container(action)
        .id(composition.stable_id(Region::PrimaryAction))
        .padding(1)
        .width(Fill)
        .height(Length::Fixed(PRIMARY_ACTION_HEIGHT))
        .style(crate::fluent_theme::container_primary_frame);
    column![
        container(progress)
            .id(composition.stable_id(Region::PrimaryProgress))
            .padding(Padding {
                bottom: 1.0,
                ..Padding::ZERO
            })
            .width(Fill),
        framed
    ]
    .spacing(FIELD_SPACING)
    .into()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ordinary_pages_use_the_reference_column_order_and_widths() {
        for width in [1020.0, 1200.0, 1500.0] {
            let composition = Composition::new(crate::generated::FeatureId::Train, width);
            let layout = composition.layout();
            assert_eq!(layout.setup_width, width * 0.19);
            assert_eq!(layout.workspace_width, width * 0.62);
            assert_eq!(layout.diagnostics_width, width * 0.19);
            assert!(
                (layout.setup_width + layout.workspace_width + layout.diagnostics_width - width)
                    .abs()
                    < 0.001
            );
        }
        let composition = Composition::new(crate::generated::FeatureId::Train, 1200.0);
        assert_eq!(
            composition.shell_regions(),
            &[Region::Setup, Region::Center, Region::Diagnostics]
        );
        assert_eq!(
            composition.center_regions(),
            &[Region::Workspace, Region::Advanced]
        );
        assert_eq!(
            composition.audit_regions(),
            &[
                Region::Setup,
                Region::Center,
                Region::Workspace,
                Region::Advanced,
                Region::Diagnostics,
                Region::PrimaryProgress,
                Region::PrimaryAction,
                Region::Status,
            ]
        );
    }

    #[test]
    fn all_six_ordinary_pages_have_the_complete_region_inventory() {
        assert_eq!(ordinary_pages().count(), 6);
        let pages = ordinary_pages().collect::<Vec<_>>();
        for (index, page) in pages.iter().enumerate() {
            assert_eq!(
                Composition::new(*page, 1020.0).next_ordinary_page(),
                pages.get(index + 1).copied()
            );
        }
        assert_eq!(
            Composition::new(crate::generated::FeatureId::Explore, 1020.0).next_ordinary_page(),
            None
        );
        for page in ordinary_pages() {
            assert_ne!(page, crate::generated::FeatureId::Explore);
            let composition = Composition::new(page, 1200.0);
            assert_eq!(composition.audit_regions().len(), 8);
            assert!(!composition.stable_id(Region::PrimaryAction).is_empty());
            assert!(!composition.stable_id(Region::PrimaryProgress).is_empty());
            assert!(!composition.stable_id(Region::Status).is_empty());
        }
        assert_eq!(
            Composition::new(crate::generated::FeatureId::Annotate, 0.0).next_ordinary_page(),
            None
        );
    }
}
