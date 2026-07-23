use std::env;

use crate::{target::TargetInfo, utilities::OnceLock, Error, ErrorKind};

#[derive(Debug)]
struct TargetInfoParserInner {
    full_arch: Box<str>,
    arch: Box<str>,
    vendor: Box<str>,
    os: Box<str>,
    env: Box<str>,
    abi: Box<str>,
}
impl TargetInfoParserInner {
    fn from_cargo_environment_variables() -> Result<Self, Error> {
        #[allow(clippy::disallowed_methods)]
        let target_name = env::var("TARGET").map_err(|err| {
            Error::new(
                ErrorKind::EnvVarNotFound,
                format!("failed reading TARGET: {err}"),
            )
        })?;

        let (full_arch, _rest) = target_name.split_once('-').ok_or(Error::new(
            ErrorKind::InvalidTarget,
            format!("target `{target_name}` only had a single component (at least two required)"),
        ))?;

        let cargo_env = |name, fallback: Option<&str>| -> Result<Box<str>, Error> {
            #[allow(clippy::disallowed_methods)]
            match env::var(name) {
                Ok(var) => Ok(var.into_boxed_str()),
                Err(err) => match fallback {
                    Some(fallback) => Ok(fallback.into()),
                    None => Err(Error::new(
                        ErrorKind::EnvVarNotFound,
                        format!("did not find fallback information for target `{target_name}`, and failed reading {name}: {err}"),
                    )),
                },
            }
        };

        let fallback_target = TargetInfo::from_rustc_target(&target_name).ok();
        let ft = fallback_target.as_ref();
        let arch = cargo_env("CARGO_CFG_TARGET_ARCH", ft.map(|t| t.arch))?;
        let vendor = cargo_env("CARGO_CFG_TARGET_VENDOR", ft.map(|t| t.vendor))?;
        let os = cargo_env("CARGO_CFG_TARGET_OS", ft.map(|t| t.os))?;
        let env = cargo_env("CARGO_CFG_TARGET_ENV", ft.map(|t| t.env))?;
        let abi = cargo_env("CARGO_CFG_TARGET_ABI", ft.map(|t| t.abi))
            .unwrap_or_else(|_| String::default().into_boxed_str());

        Ok(Self {
            full_arch: full_arch.to_string().into_boxed_str(),
            arch,
            vendor,
            os,
            env,
            abi,
        })
    }
}

/// Parser for [`TargetInfo`], contains cached information.
#[derive(Default, Debug)]
pub(crate) struct TargetInfoParser(OnceLock<Result<TargetInfoParserInner, Error>>);

impl TargetInfoParser {
    pub fn parse_from_cargo_environment_variables(&self) -> Result<TargetInfo<'_>, Error> {
        match self
            .0
            .get_or_init(TargetInfoParserInner::from_cargo_environment_variables)
        {
            Ok(TargetInfoParserInner {
                full_arch,
                arch,
                vendor,
                os,
                env,
                abi,
            }) => Ok(TargetInfo {
                full_arch,
                arch,
                vendor,
                os,
                env,
                abi,
            }),
            Err(e) => Err(e.clone()),
        }
    }
}

/// Parse the full architecture in the target name into the simpler
/// `cfg(target_arch = "...")` that `rustc` exposes.
fn parse_arch(full_arch: &str) -> Option<&str> {
    Some(match full_arch {
        arch if arch.starts_with("mipsisa32r6") => "mips32r6", 
        arch if arch.starts_with("mipsisa64r6") => "mips64r6", 

        arch if arch.starts_with("mips64") => "mips64", 
        arch if arch.starts_with("mips") => "mips",     

        arch if arch.starts_with("loongarch64") => "loongarch64",
        arch if arch.starts_with("loongarch32") => "loongarch32",

        arch if arch.starts_with("powerpc64") => "powerpc64", 
        arch if arch.starts_with("powerpc") => "powerpc",
        arch if arch.starts_with("ppc64") => "powerpc64",
        arch if arch.starts_with("ppc") => "powerpc",

        arch if arch.starts_with("x86_64") => "x86_64", 
        arch if arch.starts_with("i") && arch.ends_with("86") => "x86", 

        "arm64ec" => "arm64ec", 
        arch if arch.starts_with("aarch64") => "aarch64", 
        arch if arch.starts_with("arm64") => "aarch64", 

        arch if arch.starts_with("arm") => "arm", 
        arch if arch.starts_with("thumb") => "arm", 

        arch if arch.starts_with("riscv64") => "riscv64",
        arch if arch.starts_with("riscv32") => "riscv32",

        arch if arch.starts_with("wasm64") => "wasm64",
        arch if arch.starts_with("wasm32") => "wasm32", 
        "asmjs" => "wasm32",

        arch if arch.starts_with("nvptx64") => "nvptx64",
        arch if arch.starts_with("nvptx") => "nvptx",

        arch if arch.starts_with("bpf") => "bpf", 

        arch if arch.starts_with("pulley64") => "pulley64",
        arch if arch.starts_with("pulley32") => "pulley32",

        arch if arch.starts_with("clever") => "clever",

        "sparc" | "sparcv7" | "sparcv8" => "sparc",
        "sparc64" | "sparcv9" => "sparc64",

        "amdgcn" => "amdgpu",
        "avr" => "avr",
        "csky" => "csky",
        "hexagon" => "hexagon",
        "m68k" => "m68k",
        "msp430" => "msp430",
        "r600" => "r600",
        "s390x" => "s390x",
        "xtensa" => "xtensa",

        _ => return None,
    })
}

/// Parse environment and ABI from the last component of the target name.
fn parse_envabi(last_component: &str) -> Option<(&str, &str)> {
    let (env, abi) = match last_component {

        env_and_abi if env_and_abi.starts_with("gnu") => {
            let abi = env_and_abi.strip_prefix("gnu").unwrap();
            let abi = abi.strip_prefix("_").unwrap_or(abi);
            ("gnu", abi)
        }
        env_and_abi if env_and_abi.starts_with("musl") => {
            ("musl", env_and_abi.strip_prefix("musl").unwrap())
        }
        env_and_abi if env_and_abi.starts_with("uclibc") => {
            ("uclibc", env_and_abi.strip_prefix("uclibc").unwrap())
        }
        env_and_abi if env_and_abi.starts_with("newlib") => {
            ("newlib", env_and_abi.strip_prefix("newlib").unwrap())
        }

        "msvc" => ("msvc", ""),
        "ohos" => ("ohos", ""),
        "qnx700" => ("nto70", ""),
        "qnx710_iosock" => ("nto71_iosock", ""),
        "qnx710" => ("nto71", ""),
        "qnx800" => ("nto80", ""),
        "sgx" => ("sgx", ""),
        "threads" => ("threads", ""),
        "mlibc" => ("mlibc", ""),

        "abi64" => ("", "abi64"),
        "abiv2" => ("", "spe"),
        "eabi" => ("", "eabi"),
        "eabihf" => ("", "eabihf"),
        "macabi" => ("", "macabi"),
        "sim" => ("", "sim"),
        "softfloat" => ("", "softfloat"),
        "spe" => ("", "spe"),
        "x32" => ("", "x32"),

        "elf" => ("", ""),
        "freestanding" => ("", ""),

        _ => return None,
    };
    Some((env, abi))
}

impl<'a> TargetInfo<'a> {
    pub(crate) fn from_rustc_target(target: &'a str) -> Result<Self, Error> {
        if target == "x86_64-unknown-linux-none" {
            return Ok(Self {
                full_arch: "x86_64",
                arch: "x86_64",
                vendor: "unknown",
                os: "linux",
                env: "",
                abi: "",
            });
        }

        let mut components = target.split('-');

        let full_arch = components.next().ok_or(Error::new(
            ErrorKind::InvalidTarget,
            "target was empty".to_string(),
        ))?;
        let arch = parse_arch(full_arch).ok_or_else(|| {
            Error::new(
                ErrorKind::UnknownTarget,
                format!("target `{target}` had an unknown architecture"),
            )
        })?;

        let components: Vec<_> = components.collect();
        let (vendor, os, mut env, mut abi) = match &*components {
            [] => {
                return Err(Error::new(
                    ErrorKind::InvalidTarget,
                    format!("target `{target}` must have at least two components"),
                ))
            }
            [os] => ("unknown", *os, "", ""),
            [vendor_or_os, os_or_envabi] => {
                if let Some((env, abi)) = parse_envabi(os_or_envabi) {
                    ("unknown", *vendor_or_os, env, abi)
                } else {
                    (*vendor_or_os, *os_or_envabi, "", "")
                }
            }
            [vendor, os, envabi] => {
                let (env, abi) = parse_envabi(envabi).ok_or_else(|| {
                    Error::new(
                        ErrorKind::UnknownTarget,
                        format!("unknown environment/ABI `{envabi}` in target `{target}`"),
                    )
                })?;
                (*vendor, *os, env, abi)
            }
            _ => {
                return Err(Error::new(
                    ErrorKind::InvalidTarget,
                    format!("too many components in target `{target}`"),
                ))
            }
        };

        match full_arch {
            arch if arch.starts_with("riscv32e") => {
                abi = "ilp32e";
            }
            _ => {}
        }

        match os {
            "3ds" | "rtems" | "espidf" => env = "newlib",
            "vxworks" => env = "gnu",
            "redox" => env = "relibc",
            "aix" => abi = "vec-extabi",
            _ => {}
        }

        match target {
            "i386-apple-ios" | "x86_64-apple-ios" | "x86_64-apple-tvos" => {
                abi = "sim";
            }
            "mips64-openwrt-linux-musl" => {
                abi = "abi64";
            }
            "armv6-unknown-freebsd" | "armv6k-nintendo-3ds" | "armv7-unknown-freebsd" => {
                abi = "eabihf";
            }
            "armv7-unknown-linux-ohos" | "armv7-unknown-trusty" => {
                abi = "eabi";
            }
            _ => {}
        }

        let os = match os {
            "3ds" | "switch" => "horizon",
            "darwin" => "macos",

            os if os.starts_with("wasi") => {
                env = os.strip_prefix("wasi").unwrap();
                "wasi"
            }
            "androideabi" => {
                abi = "eabi";
                "android"
            }

            os => os,
        };

        let vendor = match vendor {
            vendor if vendor.starts_with("esp") => "espressif",
            "linux" if os == "android" || os == "androideabi" => "unknown",
            "wali" => "unknown",
            "lynx" => "unknown",
            vendor => vendor,
        };

        if vendor == "fortanix" {
            abi = "fortanix";
        }
        if vendor == "uwp" {
            abi = "uwp";
        }
        if ["powerpc64-unknown-linux-gnu", "powerpc64-wrs-vxworks"].contains(&target) {
            abi = "elfv1";
        }
        if [
            "powerpc64-unknown-freebsd",
            "powerpc64-unknown-linux-musl",
            "powerpc64-unknown-openbsd",
            "powerpc64le-unknown-freebsd",
            "powerpc64le-unknown-linux-gnu",
            "powerpc64le-unknown-linux-musl",
        ]
        .contains(&target)
        {
            abi = "elfv2";
        }

        Ok(Self {
            full_arch,
            arch,
            vendor,
            os,
            env,
            abi,
        })
    }
}
