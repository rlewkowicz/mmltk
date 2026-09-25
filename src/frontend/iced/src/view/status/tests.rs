use super::*;
use crate::view_model::notices::{Origin, failure};
#[test]
fn hover_latch_click_leave_and_keyboard_touch_opening_have_independent_lifetimes() {
    let mut component = Component::default();
    component.hover(true, true); assert!(component.open); assert!(component.focus.is_none());
    component.close(); component.hover(true, true); assert!(!component.open);
    component.hover(false, true); component.hover(true, true); assert!(component.open);
    component.activate(Opening::Mouse, true); component.hover(false, true); assert!(!component.open);
    for opening in [Opening::Keyboard, Opening::Touch] {
        component.activate(opening, true); component.hover(true, true); component.hover(false, true); assert!(component.open); component.close();
    }
}
#[test]
fn popup_dimensions_remain_inside_narrow_and_wide_viewports() {
    for width in [320.0, 700.0, 1020.0, 1500.0, 2200.0] {
        let rect = overlay::panel_bounds(iced::Size::new(width, 720.0), 600.0);
        assert_eq!(rect.width, (0.6 * width).min(width - 24.0));
        assert!(rect.x >= 12.0 && rect.x + rect.width <= width - 12.0);
        assert!(rect.y >= 12.0 && rect.y + rect.height <= 708.0);
    }
    assert_eq!((STATUS_WIDTH, SETTINGS_WIDTH), (192.0, 96.0));
}
#[test]
fn healthy_and_alert_themes_preserve_exact_border_and_fill_rules() {
    for dark in [false, true] {
        let theme = crate::fluent_theme::app_theme(dark);
        let healthy = alert_style(&theme, 0.0, false);
        assert_eq!(healthy.border.width, 0.0);
        let alert = alert_style(&theme, 0.0, true);
        assert_eq!(alert.border.width, 1.0); assert_eq!(alert.border.color, iced::Color::WHITE);
        assert_ne!(alert.background, alert_style(&theme, 1.0, true).background);
    }
}
#[test]
fn only_visible_controls_participate_in_status_traversal() {
    let mut component = Component::default(); let mut notices = NoticeStore::default();
    notices.observe(Origin::Provider, 0, 1, Some(failure("one")));
    let id = notices.latest().unwrap().id;
    assert_eq!(component.controls(&notices), [Control::Trigger]);
    component.activate(Opening::Keyboard, true); component.focus = Some(Control::Trigger);
    assert_eq!(component.traverse(false, &notices), Control::Close);
    assert_eq!(component.traverse(true, &notices), Control::Dismiss(id));
    notices.dismiss(id); component.close(); assert_eq!(component.controls(&notices), [Control::Trigger]);
}

#[test]
fn detail_selection_is_retained_read_only_and_replaced_only_with_its_content_version() {
    let mut component = Component::default();
    let mut notices = NoticeStore::default();
    notices.observe(Origin::Provider, 0, 1, Some(failure("first\n\nmiddle\nlast")));
    let id = notices.latest().unwrap().id;
    component.sync(&notices);
    component.select_detail(id, iced::widget::text_editor::Action::SelectAll);
    assert_eq!(component.details[0].content.selection().as_deref(), Some("first\n\nmiddle\nlast"));
    component.select_detail(id, iced::widget::text_editor::Action::Edit(iced::widget::text_editor::Edit::Insert('x')));
    assert_eq!(component.details[0].content.text(), "first\n\nmiddle\nlast");
    component.sync(&notices);
    assert_eq!(component.details[0].content.selection().as_deref(), Some("first\n\nmiddle\nlast"));
    notices.observe(Origin::Provider, 0, 1, Some(failure("updated")));
    component.sync(&notices);
    assert_eq!(component.details[0].content.text(), "updated");
    notices.dismiss(id); component.sync(&notices); assert!(component.details.is_empty());
}
