fn main() {
    cfg_aliases::cfg_aliases! {
        dot_out: { feature = "dot-out" },
        glsl_out: { feature = "glsl-out" },
        spv_out: { feature = "spv-out" },
        wgsl_out: { feature = "wgsl-out" },
        std: { any(test, feature = "stderr", feature = "fs") },
        no_std: { not(std) },
    }
}
