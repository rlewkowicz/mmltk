use crate::integration_control::widget_ops::{
    AnnotationReveal, ControlBounds, contains_rectangle, reveal_axis,
};
use iced::Rectangle;
#[test]
fn measured_reveal_handles_both_edges_visible_and_oversized_controls() {
    assert_eq!(reveal_axis(120.0, 40.0, 100.0, 200.0), 0.0);
    assert_eq!(reveal_axis(80.0, 40.0, 100.0, 200.0), -21.0);
    assert_eq!(reveal_axis(280.0, 40.0, 100.0, 200.0), 21.0);
    assert_eq!(reveal_axis(120.0, 400.0, 100.0, 200.0), 20.0);
    assert_eq!(reveal_axis(100.0, 200.0, 100.0, 200.0), 0.0);
    assert_eq!(reveal_axis(500.0, 0.0, 100.0, 200.0), 0.0);
    assert_eq!(reveal_axis(500.0, 40.0, 100.0, 0.0), 0.0);
    for start in [30.2, 2598.2] {
        let offset = reveal_axis(start, 46.4, 52.0, 771.3).round();
        assert!(start - offset >= 52.0);
        assert!(start - offset + 46.4 <= 823.3);
    }
}

#[test]
fn annotation_reveal_proves_subregions_without_rescaling_full_geometry() {
    let full = Rectangle {
        x: -100.0,
        y: -200.0,
        width: 1000.0,
        height: 1600.0,
    };
    let viewport = Rectangle {
        x: 0.0,
        y: 50.0,
        width: 700.0,
        height: 500.0,
    };
    let measured = ControlBounds {
        target: full,
        page: viewport,
        horizontal: viewport,
    };
    let visible = measured.visible().unwrap();
    assert_eq!(visible, viewport);
    assert!(!contains_rectangle(
        visible,
        measured.requested(AnnotationReveal::Control).unwrap()
    ));
    let request = AnnotationReveal::Source {
        extent: [1000.0, 1600.0],
        region: Rectangle {
            x: 250.0,
            y: 300.0,
            width: 10.0,
            height: 20.0,
        },
        margin: 2.0,
    };
    let requested = measured.requested(request).unwrap();
    assert_eq!(
        requested,
        Rectangle {
            x: 148.0,
            y: 98.0,
            width: 14.0,
            height: 24.0
        }
    );
    assert!(contains_rectangle(visible, requested));
    assert_eq!(measured.target, full);
    let trailing = ControlBounds {
        target: Rectangle { y: 400.0, ..full },
        ..measured
    };
    assert!(!contains_rectangle(
        viewport,
        trailing.requested(request).unwrap()
    ));
    let clamped = ControlBounds {
        target: Rectangle {
            x: 650.0,
            width: 100.0,
            ..full
        },
        ..measured
    };
    assert!(!contains_rectangle(
        clamped.visible().unwrap(),
        clamped.target
    ));
    let missing = ControlBounds {
        page: Rectangle::default(),
        ..measured
    };
    assert!(missing.visible().is_none());
    assert!(missing.requested(request).is_none());
    let absent = ControlBounds {
        target: Rectangle::default(),
        ..measured
    };
    assert!(absent.visible().is_none());
    assert!(absent.requested(request).is_none());
}

#[test]
fn primary_action_measurement_uses_nested_scroll_coordinates_and_outer_frame() {
    use super::FindControl;
    use iced::Vector;
    use iced::advanced::widget::{Id, Operation};
    use iced::advanced::widget::operation::{Outcome, Scrollable};
    use iced::widget::operation::{AbsoluteOffset, RelativeOffset};
    struct Scroll;
    impl Scrollable for Scroll {
        fn snap_to(&mut self, _: RelativeOffset<Option<f32>>) {}
        fn scroll_to(&mut self, _: AbsoluteOffset<Option<f32>>) {}
        fn scroll_by(&mut self, _: AbsoluteOffset, _: Rectangle, _: Rectangle) {}
    }
    let outer = Rectangle { x: 450.0, y: 800.0, width: 202.0, height: 48.0 };
    for (horizontal_offset, vertical_offset, visible) in [
        (0.0, 0.0, false),
        (0.0, 600.0, false), // Right edge remains clipped until horizontal reveal.
        (300.0, 0.0, false),
        (300.0, 600.0, true),
        (300.0, 480.0, false), // Only part of the bottom perimeter is visible.
        (300.0, 600.0, true),
    ] {
        let id = Id::from("train.primary");
        let mut find = FindControl {
            target: id.clone(), translation: Vector::ZERO, pending_translation: Vector::ZERO,
            bounds: None, page: Rectangle::default(), horizontal: Rectangle::default(),
        };
        let horizontal = Rectangle { x: 0.0, y: 0.0, width: 500.0, height: 400.0 };
        let page = Rectangle { x: 0.0, y: 52.0, width: 1020.0, height: 300.0 };
        find.scrollable(Some(&Id::from(crate::view::HORIZONTAL_SCROLL_ID)), horizontal, horizontal,
            Vector::new(horizontal_offset, 0.0), &mut Scroll);
        find.traverse(&mut |operation| {
            operation.scrollable(Some(&Id::from(crate::view::PAGE_SCROLL_ID)), page, page,
                Vector::new(0.0, vertical_offset), &mut Scroll);
            operation.traverse(&mut |operation| operation.container(Some(&id), outer));
        });
        let Outcome::Some(measured) = find.finish() else { panic!("measurement missing") };
        assert_eq!(measured.target, Rectangle { x: outer.x-horizontal_offset, y: outer.y-vertical_offset, ..outer });
        assert_eq!(measured.page.x, -horizontal_offset);
        assert_eq!(measured.horizontal, horizontal);
        assert_eq!(find.translation, Vector::ZERO);
        let inner = Rectangle { x: measured.target.x+1.0, y: measured.target.y+1.0,
            width: measured.target.width-2.0, height: measured.target.height-2.0 };
        assert_eq!(inner.height, 46.0);
        assert_eq!(measured.target.height, 48.0);
        assert_eq!(measured.page.intersection(&measured.horizontal).is_some_and(|viewport| contains_rectangle(viewport,inner)),visible);
        // A sibling must not inherit either scroller's translation.
        find.container(Some(&id), outer);
        assert_eq!(find.bounds, Some(outer));
    }
}
