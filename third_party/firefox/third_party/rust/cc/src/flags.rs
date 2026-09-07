use crate::target::TargetInfo;
use crate::{Build, Error, ErrorKind, Tool, ToolFamily};
use std::borrow::Cow;
use std::ffi::OsString;

#[derive(Debug, PartialEq, Default)]
pub(crate) struct RustcCodegenFlags<'a> {
    branch_protection: Option<&'a str>,
    code_model: Option<&'a str>,
    no_vectorize_loops: bool,
    no_vectorize_slp: bool,
    profile_generate: Option<&'a str>,
    profile_use: Option<&'a str>,
    control_flow_guard: Option<&'a str>,
    lto: Option<&'a str>,
    relocation_model: Option<&'a str>,
    embed_bitcode: Option<bool>,
    force_frame_pointers: Option<bool>,
    no_redzone: Option<bool>,
    soft_float: Option<bool>,
    dwarf_version: Option<u32>,
}

impl<'this> RustcCodegenFlags<'this> {
    pub(crate) fn parse(rustflags_env: &'this str) -> Result<Self, Error> {
        fn is_flag_prefix(flag: &str) -> bool {
            [
                "-Z",
                "-C",
                "--codegen",
                "-L",
                "-l",
                "-o",
                "-W",
                "--warn",
                "-A",
                "--allow",
                "-D",
                "--deny",
                "-F",
                "--forbid",
            ]
            .contains(&flag)
        }

        fn handle_flag_prefix<'a>(prev: &'a str, curr: &'a str) -> (&'a str, &'a str) {
            match prev {
                "--codegen" | "-C" => ("-C", curr),
                _ if curr.starts_with("--codegen=") => ("-C", &curr[10..]),
                "-Z" => ("-Z", curr),
                "-L" | "-l" | "-o" => (prev, curr),
                "-W" | "--warn" => ("-W", curr),
                "-A" | "--allow" => ("-A", curr),
                "-D" | "--deny" => ("-D", curr),
                "-F" | "--forbid" => ("-F", curr),
                _ => ("", curr),
            }
        }

        let mut codegen_flags = Self::default();

        let mut prev_prefix = None;
        for curr in rustflags_env.split("\u{1f}") {
            let prev = prev_prefix.take().unwrap_or("");
            if prev.is_empty() && is_flag_prefix(curr) {
                prev_prefix = Some(curr);
                continue;
            }

            let (prefix, rustc_flag) = handle_flag_prefix(prev, curr);
            codegen_flags.set_rustc_flag(prefix, rustc_flag)?;
        }

        Ok(codegen_flags)
    }

    fn set_rustc_flag(&mut self, prefix: &str, flag: &'this str) -> Result<(), Error> {
        fn arg_to_bool(arg: impl AsRef<str>) -> Option<bool> {
            match arg.as_ref() {
                "y" | "yes" | "on" | "true" => Some(true),
                "n" | "no" | "off" | "false" => Some(false),
                _ => None,
            }
        }

        fn arg_to_u32(arg: impl AsRef<str>) -> Option<u32> {
            arg.as_ref().parse().ok()
        }

        let (flag, value) = if let Some((flag, value)) = flag.split_once('=') {
            (flag, Some(value))
        } else {
            (flag, None)
        };
        let flag = if prefix.is_empty() {
            Cow::Borrowed(flag)
        } else {
            Cow::Owned(format!("{prefix}{flag}"))
        };

        fn flag_ok_or<'flag>(
            flag: Option<&'flag str>,
            msg: &'static str,
        ) -> Result<&'flag str, Error> {
            flag.ok_or(Error::new(ErrorKind::InvalidFlag, msg))
        }

        match flag.as_ref() {
            "-Ccode-model" => {
                self.code_model = Some(flag_ok_or(value, "-Ccode-model must have a value")?);
            }
            "-Cno-vectorize-loops" => self.no_vectorize_loops = true,
            "-Cno-vectorize-slp" => self.no_vectorize_slp = true,
            "-Cprofile-generate" => {
                self.profile_generate =
                    Some(flag_ok_or(value, "-Cprofile-generate must have a value")?);
            }
            "-Cprofile-use" => {
                self.profile_use = Some(flag_ok_or(value, "-Cprofile-use must have a value")?);
            }
            "-Ccontrol-flow-guard" => self.control_flow_guard = value.or(Some("true")),
            "-Clto" => self.lto = value.or(Some("true")),
            "-Crelocation-model" => {
                self.relocation_model =
                    Some(flag_ok_or(value, "-Crelocation-model must have a value")?);
            }
            "-Cembed-bitcode" => self.embed_bitcode = value.map_or(Some(true), arg_to_bool),
            "-Cforce-frame-pointers" => {
                self.force_frame_pointers = value.map_or(Some(true), arg_to_bool)
            }
            "-Cno-redzone" => self.no_redzone = value.map_or(Some(true), arg_to_bool),
            "-Csoft-float" => self.soft_float = value.map_or(Some(true), arg_to_bool),
            "-Zbranch-protection" | "-Cbranch-protection" => {
                self.branch_protection =
                    Some(flag_ok_or(value, "-Zbranch-protection must have a value")?);
            }
            "-Zdwarf-version" | "-Cdwarf-version" => {
                self.dwarf_version = Some(value.and_then(arg_to_u32).ok_or(Error::new(
                    ErrorKind::InvalidFlag,
                    "-Zdwarf-version must have a value",
                ))?);
            }
            _ => {}
        }
        Ok(())
    }

    pub(crate) fn cc_flags(&self, build: &Build, tool: &mut Tool, target: &TargetInfo<'_>) {
        let family = tool.family;
        let mut push_if_supported = |flag: OsString| {
            if build
                .is_flag_supported_inner(&flag, tool, target)
                .unwrap_or(false)
            {
                tool.args.push(flag);
            } else {
                build.cargo_output.print_warning(&format!(
                    "Inherited flag {flag:?} is not supported by the currently used CC"
                ));
            }
        };

        let clang_or_gnu =
            matches!(family, ToolFamily::Clang { .. }) || matches!(family, ToolFamily::Gnu);

        if clang_or_gnu {
            if let Some(value) = self.branch_protection {
                push_if_supported(
                    format!("-mbranch-protection={}", value.replace(",", "+")).into(),
                );
            }
            if let Some(value) = self.code_model {
                push_if_supported(format!("-mcmodel={value}").into());
            }
            if self.no_vectorize_loops {
                push_if_supported("-fno-vectorize".into());
            }
            if self.no_vectorize_slp {
                push_if_supported("-fno-slp-vectorize".into());
            }
            if let Some(value) = self.relocation_model {
                let cc_flag = match value {
                    "pic" => Some("-fPIC"),
                    "pie" => Some("-fPIE"),
                    "dynamic-no-pic" => Some("-mdynamic-no-pic"),
                    _ => None,
                };
                if let Some(cc_flag) = cc_flag {
                    push_if_supported(cc_flag.into());
                }
            }
            if let Some(value) = self.force_frame_pointers {
                let cc_flag = if value {
                    "-fno-omit-frame-pointer"
                } else {
                    "-fomit-frame-pointer"
                };
                push_if_supported(cc_flag.into());
            }
            if let Some(value) = self.no_redzone {
                let cc_flag = if value { "-mno-red-zone" } else { "-mred-zone" };
                push_if_supported(cc_flag.into());
            }
            if let Some(value) = self.soft_float {
                let cc_flag = if value {
                    "-msoft-float"
                } else {
                    "-mhard-float"
                };
                push_if_supported(cc_flag.into());
            }
            if let Some(value) = self.dwarf_version {
                push_if_supported(format!("-gdwarf-{value}").into());
            }
        }

        match family {
            ToolFamily::Clang { .. } => {
                if let Some(value) = self.profile_generate {
                    push_if_supported(format!("-fprofile-generate={value}").into());
                }
                if let Some(value) = self.profile_use {
                    push_if_supported(format!("-fprofile-use={value}").into());
                }

                if let Some(value) = self.embed_bitcode {
                    let cc_val = if value { "all" } else { "off" };
                    push_if_supported(format!("-fembed-bitcode={cc_val}").into());
                }

                if let Some(value) = self.lto {
                    let cc_val = match value {
                        "y" | "yes" | "on" | "true" | "fat" => Some("full"),
                        "thin" => Some("thin"),
                        _ => None,
                    };
                    if let Some(cc_val) = cc_val {
                        push_if_supported(format!("-flto={cc_val}").into());
                    }
                }
                if let Some(value) = self.control_flow_guard {
                    let cc_val = match value {
                        "y" | "yes" | "on" | "true" | "checks" => Some("cf"),
                        "nochecks" => Some("cf-nochecks"),
                        "n" | "no" | "off" | "false" => Some("none"),
                        _ => None,
                    };
                    if let Some(cc_val) = cc_val {
                        push_if_supported(format!("-mguard={cc_val}").into());
                    }
                }
            }
            ToolFamily::Gnu => {}
            ToolFamily::Msvc { .. } => {
                if let Some(value) = self.control_flow_guard {
                    let cc_val = match value {
                        "y" | "yes" | "on" | "true" | "checks" => Some("cf"),
                        "n" | "no" | "off" | "false" => Some("cf-"),
                        _ => None,
                    };
                    if let Some(cc_val) = cc_val {
                        push_if_supported(format!("/guard:{cc_val}").into());
                    }
                }
                if let Some(value) = self.force_frame_pointers {
                    if !target.arch.contains("64") {
                        let cc_flag = if value { "/Oy-" } else { "/Oy" };
                        push_if_supported(cc_flag.into());
                    }
                }
            }
        }
    }
}
