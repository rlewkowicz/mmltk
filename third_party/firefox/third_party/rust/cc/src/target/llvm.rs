use std::borrow::Cow;

use super::{generated, TargetInfo};

impl TargetInfo<'_> {
    /// The LLVM/Clang target triple.
    ///
    /// See <https://clang.llvm.org/docs/CrossCompilation.html#target-triple>.
    ///
    /// Rust and Clang don't really agree on target naming, so we first try to
    /// find the matching trible based on `rustc`'s output, but if no such
    /// triple exists, we attempt to construct the triple from scratch.
    ///
    /// NOTE: You should never need to match on this explicitly, use the
    /// fields on [`TargetInfo`] instead.
    pub(crate) fn llvm_target(
        &self,
        rustc_target: &str,
        version: Option<&str>,
    ) -> Cow<'static, str> {
        if rustc_target == "armv7-apple-ios" {
            return Cow::Borrowed("armv7-apple-ios");
        } else if self.os == "uefi" {
            return Cow::Owned(format!("{}-unknown-windows-gnu", self.full_arch));
        }

        if version.is_none() {
            if let Ok(index) = generated::LLVM_TARGETS
                .binary_search_by_key(&rustc_target, |(rustc_target, _)| rustc_target)
            {
                let (_, llvm_target) = &generated::LLVM_TARGETS[index];
                return Cow::Borrowed(llvm_target);
            }
        }


        let arch = match self.full_arch {
            riscv32 if riscv32.starts_with("riscv32") => "riscv32",
            riscv64 if riscv64.starts_with("riscv64") => "riscv64",
            "aarch64" if self.vendor == "apple" => "arm64",
            "armv7" if self.vendor == "sony" => "thumbv7a", 
            arch => arch,
        };
        let vendor = match self.vendor {
            "kmc" | "nintendo" => "unknown",
            "unknown" if self.os == "android" => "linux",
            "uwp" => "pc",
            "espressif" => "",
            _ if self.arch == "msp430" => "",
            vendor => vendor,
        };
        let os = match self.os {
            "macos" => "macosx",
            "visionos" => "xros",
            "uefi" => "windows",
            "solid_asp3" | "horizon" | "teeos" | "nuttx" | "espidf" => "none",
            "nto" => "unknown",    
            "trusty" => "unknown", 
            os => os,
        };
        let version = version.unwrap_or("");
        let env = match self.env {
            "newlib" | "nto70" | "nto71" | "nto71_iosock" | "p1" | "p2" | "relibc" | "sgx"
            | "uclibc" => "",
            env => env,
        };
        let abi = match self.abi {
            "sim" => "simulator",
            "llvm" | "softfloat" | "uwp" | "vec-extabi" => "",
            "ilp32" => "_ilp32",
            "abi64" => "",
            abi => abi,
        };
        Cow::Owned(match (vendor, env, abi) {
            ("", "", "") => format!("{arch}-{os}{version}"),
            ("", env, abi) => format!("{arch}-{os}{version}-{env}{abi}"),
            (vendor, "", "") => format!("{arch}-{vendor}-{os}{version}"),
            (vendor, env, abi) => format!("{arch}-{vendor}-{os}{version}-{env}{abi}"),
        })
    }
}
