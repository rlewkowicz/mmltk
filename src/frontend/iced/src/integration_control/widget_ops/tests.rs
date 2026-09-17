use crate::integration_control::widget_ops::{AnnotationReveal, ControlBounds, contains_rectangle, reveal_axis};
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
