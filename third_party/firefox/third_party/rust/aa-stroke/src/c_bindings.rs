use crate::{filled_circle_with_path_builder, PathBuilder, Point, StrokeStyle, Stroker};

type OutputVertex = crate::Vertex;

#[repr(C)]
pub struct VertexBuffer {
    data: *const OutputVertex,
    len: usize
}

#[no_mangle]
pub extern "C" fn aa_stroke_new(
    style: &StrokeStyle,
    output_ptr: *mut OutputVertex,
    output_capacity: usize,
) -> *mut Stroker {
    let mut s = Stroker::new(style);
    if output_ptr != std::ptr::null_mut() {
        let slice = unsafe { std::slice::from_raw_parts_mut(output_ptr, output_capacity) };
        s.set_output_buffer(slice);
    }
    Box::into_raw(Box::new(s))
}

#[no_mangle]
pub extern "C" fn aa_stroke_move_to(s: &mut Stroker, x: f32, y: f32, closed: bool) {
    s.move_to(Point::new(x, y), closed);
}

#[no_mangle]
pub extern "C" fn aa_stroke_line_to(s: &mut Stroker, x: f32, y: f32, end: bool) {
    if end {
        s.line_to_capped(Point::new(x, y))
    } else {
        s.line_to(Point::new(x, y));
    }
}

#[no_mangle]
pub extern "C" fn aa_stroke_curve_to(s: &mut Stroker, c1x: f32, c1y: f32, c2x: f32, c2y: f32, x: f32, y: f32, end: bool) {
    if end {
        s.curve_to_capped(Point::new(c1x, c1y), Point::new(c2x, c2y), Point::new(x, y));
    } else {
        s.curve_to(Point::new(c1x, c1y), Point::new(c2x, c2y), Point::new(x, y));
    }
}


#[no_mangle]
pub extern "C" fn aa_stroke_close(s: &mut Stroker) {
    s.close();
}

#[no_mangle]
pub extern "C" fn aa_stroke_finish(s: &mut Stroker) -> VertexBuffer {
    let stroked_path = s.get_stroked_path();
    if let Some(output_buffer_size) = stroked_path.get_output_buffer_size() {
        VertexBuffer {
            data: std::ptr::null(),
            len: output_buffer_size,
        }
    } else {
        let result = s.finish();
        let len = result.len();
        let vb = VertexBuffer { data: Box::leak(result).as_ptr(), len };
        vb
    }
}

#[no_mangle]
pub extern "C" fn aa_stroke_vertex_buffer_release(vb: VertexBuffer)
{
    if vb.data != std::ptr::null() {
        unsafe {
            drop(Box::from_raw(std::slice::from_raw_parts_mut(vb.data as *mut OutputVertex, vb.len)));
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn aa_stroke_release(s: *mut Stroker) {
    drop(Box::from_raw(s));
}

#[no_mangle]
pub extern "C" fn aa_stroke_filled_circle(
    cx: f32, cy: f32, radius: f32, output_ptr: *mut OutputVertex, output_capacity: usize
) -> VertexBuffer {
    let mut path_builder = PathBuilder::new(1.);
    if output_ptr != std::ptr::null_mut() {
        let slice = unsafe { std::slice::from_raw_parts_mut(output_ptr, output_capacity) };
        path_builder.set_output_buffer(slice);
    }

    filled_circle_with_path_builder(&mut path_builder, Point::new(cx, cy), radius);

    if let Some(output_buffer_size) = path_builder.get_output_buffer_size() {
        VertexBuffer {
            data: std::ptr::null(),
            len: output_buffer_size,
        }
    } else {
        let result = path_builder.finish();
        let len = result.len();
        let vb = VertexBuffer { data: Box::leak(result).as_ptr(), len };
        vb
    }
}
