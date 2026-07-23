fn main() {
    cfg_aliases::cfg_aliases! {
        native: { target_os = "linux" },
        send_sync: { target_os = "linux" },
        vulkan: { feature = "vulkan" },
        drm: { all(feature = "drm", target_os = "linux") },
        any_backend: { vulkan },
        supports_64bit_atomics: { target_has_atomic = "64" },
        supports_ptr_atomics: { target_has_atomic = "ptr" }
    }
}
