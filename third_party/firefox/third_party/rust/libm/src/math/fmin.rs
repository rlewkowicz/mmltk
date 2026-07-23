pub fn fmin(x: f64, y: f64) -> f64 {
    (if y.is_nan() || x < y { x } else { y }) * 1.0
}
