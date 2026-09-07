pub fn fmax(x: f64, y: f64) -> f64 {
    (if x.is_nan() || x < y { y } else { x }) * 1.0
}
