fn main() {
    cfg_aliases::cfg_aliases! {
        send_sync: { feature = "std" },
        vulkan: { feature = "vulkan" },
        drm: { all(feature = "drm", target_os = "linux") },
        supports_64bit_atomics: { target_has_atomic = "64" }
    }
}
