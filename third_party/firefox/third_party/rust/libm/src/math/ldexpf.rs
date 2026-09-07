pub fn ldexpf(x: f32, n: i32) -> f32 {
    super::scalbnf(x, n)
}
