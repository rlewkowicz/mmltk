# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.



cargo_host_flag := --target=$(RUST_HOST_TARGET)
cargo_target_flag := --target=$(RUST_TARGET)

cargo_build_flags = $(CARGOFLAGS)

ifneq (,$(findstring megazord,$(RUST_LIBRARY_FILE)))
ifdef MOZ_DEBUG_RUST
cargo_build_flags += --profile dev-megazord
else
cargo_build_flags += --profile release-megazord
endif
else
ifndef MOZ_DEBUG_RUST
cargo_build_flags += --release
endif
endif

cargo_crate_type_flag := $(if $(findstring megazord,$(RUST_LIBRARY_FILE)),--crate-type staticlib,)

ifndef JS_STANDALONE
cargo_build_flags += --frozen
endif

cargo_build_flags += --manifest-path $(CARGO_FILE)
ifdef BUILD_VERBOSE_LOG
cargo_build_flags += -vv
endif

ifneq (,$(USE_CARGO_JSON_MESSAGE_FORMAT))
cargo_build_flags += --message-format=json
endif

ifdef MACH_STDOUT_ISATTY
ifeq (,$(findstring --color,$(cargo_build_flags)))
ifdef NO_ANSI
cargo_build_flags += --color=never
else
cargo_build_flags += --color=always
endif
endif
endif

cargo_build_flags += $(filter -j1,$(MAKEFLAGS))

ifdef MOZ_TSAN
cargo_build_flags += -Zbuild-std=std,panic_abort
RUSTFLAGS += -Zsanitizer=thread
endif

rustflags_sancov =

cargo_rustc_flags = $(CARGO_RUSTCFLAGS)
ifndef DEVELOPER_OPTIONS
ifndef MOZ_DEBUG_RUST
ifndef MOZ_LTO_RUST_CROSS
ifndef rustflags_sancov
cargo_rustc_flags += -Clto$(if $(filter full,$(MOZ_LTO_RUST_CROSS)),=fat)
RUSTFLAGS += -Cembed-bitcode=yes
endif
endif
endif
endif

ifdef CARGO_INCREMENTAL
export CARGO_INCREMENTAL
endif

rustflags_neon =
ifeq (neon,$(MOZ_FPU))
ifneq (,$(filter thumbv7neon-,$(RUST_TARGET)))
rustflags_neon += -C target_feature=+neon,-d16
endif
endif

rustflags_override = $(MOZ_RUST_DEFAULT_FLAGS) $(rustflags_neon)

ifdef extra_rustflags
rustflags_override += $(extra_rustflags)
endif

ifdef DEVELOPER_OPTIONS
rustflags_override += -Clto=off
endif

ifneq (,$(or $(MOZ_USING_SCCACHE),$(MOZ_USING_BUILDCACHE)))
export RUSTC_WRAPPER=$(CCACHE)
endif

ifndef CROSS_COMPILE
ifdef MOZ_TSAN
PASS_ONLY_BASE_CFLAGS_TO_RUST=1
else
ifneq (,$(MOZ_ASAN)$(MOZ_UBSAN))
ifneq ($(OS_ARCH), Linux)
PASS_ONLY_BASE_CFLAGS_TO_RUST=1
endif 
endif 
endif 
endif 

ifeq (WINNT,$(HOST_OS_ARCH))
ifdef MOZ_CODE_COVERAGE
PASS_ONLY_BASE_CFLAGS_TO_RUST=1
endif 
endif 

ifeq (WINNT,$(HOST_OS_ARCH))
normalize_sep = $(patsubst //?/%,%,$(subst \,/,$(1)))
else
normalize_sep = $(1)
endif


rust_host_cc_env_name := $(subst -,_,$(RUST_HOST_TARGET))

export CC_$(rust_host_cc_env_name)=$(filter-out $(HOST_CC_BASE_FLAGS),$(HOST_CC))
export CXX_$(rust_host_cc_env_name)=$(filter-out $(HOST_CXX_BASE_FLAGS),$(HOST_CXX))
export AR_$(rust_host_cc_env_name)=$(HOST_AR)

rust_cc_env_name := $(subst -,_,$(RUST_TARGET))

export CC_$(rust_cc_env_name)=$(filter-out $(CC_BASE_FLAGS),$(CC))
export CXX_$(rust_cc_env_name)=$(filter-out $(CXX_BASE_FLAGS),$(CXX))
export AR_$(rust_cc_env_name)=$(AR)

ifeq (WINNT,$(HOST_OS_ARCH))
HOST_CC_BASE_FLAGS += -DUNICODE
HOST_CXX_BASE_FLAGS += -DUNICODE
endif
ifeq (WINNT,$(OS_ARCH))
CC_BASE_FLAGS += -DUNICODE
CXX_BASE_FLAGS += -DUNICODE
endif

ifneq (1,$(PASS_ONLY_BASE_CFLAGS_TO_RUST))
export CFLAGS_$(rust_host_cc_env_name)=$(HOST_CC_BASE_FLAGS) $(COMPUTED_HOST_CFLAGS)
export CXXFLAGS_$(rust_host_cc_env_name)=$(HOST_CXX_BASE_FLAGS) $(COMPUTED_HOST_CXXFLAGS)
ifneq (,$(filter clang%,$(CC_TYPE)))
RUST_LTO_CFLAGS=$(MOZ_LTO_CFLAGS)
endif
export CFLAGS_$(rust_cc_env_name)=$(CC_BASE_FLAGS) $(RUST_LTO_CFLAGS) $(COMPUTED_CFLAGS) $(filter-out -fprofile-generate%,$(PGO_CFLAGS))
export CXXFLAGS_$(rust_cc_env_name)=$(CXX_BASE_FLAGS) $(RUST_LTO_CFLAGS) $(COMPUTED_CXXFLAGS) $(filter-out -fprofile-generate%,$(PGO_CFLAGS))
else
export CFLAGS_$(rust_host_cc_env_name)=$(HOST_CC_BASE_FLAGS)
export CXXFLAGS_$(rust_host_cc_env_name)=$(HOST_CXX_BASE_FLAGS)
export CFLAGS_$(rust_cc_env_name)=$(CC_BASE_FLAGS)
export CXXFLAGS_$(rust_cc_env_name)=$(CXX_BASE_FLAGS) $(filter -fno-aligned-new -fno-sized-deallocation,$(COMPUTED_CXXFLAGS))
endif

ifeq ($(RUST_TARGET),$(RUST_HOST_TARGET))
define sanitizer_options
ifdef MOZ_$1
export $1_OPTIONS:=$$($1_OPTIONS:%=%:)intercept_tls_get_addr=0
endif
endef
$(foreach san,ASAN TSAN UBSAN,$(eval $(call sanitizer_options,$(san))))
endif

export BINDGEN_EXTRA_CLANG_ARGS:=$(filter --target=%,$(BINDGEN_SYSTEM_FLAGS))
export CARGO_TARGET_DIR
export RUSTFLAGS
export RUSTC
export RUSTDOC
export RUSTDOCFLAGS
export RUSTFMT
export LIBCLANG_PATH=$(MOZ_LIBCLANG_PATH)
export CLANG_PATH=$(MOZ_CLANG_PATH)
export PKG_CONFIG
export PKG_CONFIG_ALLOW_CROSS=1
export PKG_CONFIG_PATH
ifneq (,$(PKG_CONFIG_SYSROOT_DIR))
export PKG_CONFIG_SYSROOT_DIR
endif
ifneq (,$(PKG_CONFIG_LIBDIR))
export PKG_CONFIG_LIBDIR
endif
export RUST_BACKTRACE=full
export MOZ_TOPOBJDIR=$(topobjdir)
export MOZ_FOLD_LIBS
export PYTHON3
export CARGO_PROFILE_RELEASE_OPT_LEVEL
export CARGO_PROFILE_DEV_OPT_LEVEL

ifeq ($(OS_ARCH), Darwin)
ifdef MACOS_SDK_DIR
export COREAUDIO_SDK_PATH=$(MACOS_SDK_DIR)
endif
ifdef IPHONEOS_SDK_DIR
export COREAUDIO_SDK_PATH=$(IPHONEOS_SDK_DIR)
export IPHONEOS_SDK_DIR
PATH := $(topsrcdir)/build/macosx:$(PATH)
endif
endif
export LIBZ_RS_SYS_PREFIX=MOZ_Z_

ifndef RUSTC_BOOTSTRAP
RUSTC_BOOTSTRAP := mozglue_static,qcms
ifdef MOZ_RUST_SIMD
RUSTC_BOOTSTRAP := $(RUSTC_BOOTSTRAP),encoding_rs,any_all_workaround
endif
export RUSTC_BOOTSTRAP
endif

target_rust_ltoable := force-cargo-library-build $(ADD_RUST_LTOABLE)
target_rust_nonltoable := force-cargo-test-run force-cargo-program-build

ifdef MOZ_PGO_RUST
ifdef MOZ_PROFILE_GENERATE
rust_pgo_flags := -C profile-generate=$(topobjdir)
ifeq (1,$(words $(filter 5.% 6.% 7.% 8.% 9.% 10.% 11.%,$(CC_VERSION) $(RUSTC_LLVM_VERSION))))
rust_pgo_flags += -C llvm-args=--disable-vp=true
endif
rust_pgo_flags += $(patsubst -mllvm:%,-C llvm-args=%,$(filter -mllvm:%,$(subst -mllvm ,-mllvm:,$(PROFILE_GEN_CFLAGS))))
else 
rust_pgo_flags := -C profile-use=$(PGO_PROFILE_PATH)
endif
endif

ifdef MOZ_DEBUG_RUST
ifneq (,$(filter i686-pc-windows-%,$(RUST_TARGET)))
RUSTFLAGS += -Zmir-enable-passes=-CheckAlignment
RUSTC_BOOTSTRAP := 1
endif
endif

$(target_rust_ltoable): RUSTFLAGS:=$(rustflags_override) $(rustflags_sancov) $(RUSTFLAGS) $(rust_pgo_flags) \
								$(if $(MOZ_LTO_RUST_CROSS),\
								    -Clinker-plugin-lto \
									,)
$(target_rust_nonltoable): RUSTFLAGS:=$(rustflags_override) $(rustflags_sancov) $(RUSTFLAGS)

TARGET_RECIPES := $(target_rust_ltoable) $(target_rust_nonltoable)

HOST_RECIPES := \
  $(foreach a,library program,$(foreach b,build check udeps clippy,force-cargo-host-$(a)-$(b)))

$(HOST_RECIPES): RUSTFLAGS:=$(rustflags_override)

define RUN_CARGO_INNER
$(if $(findstring n,$(filter-out --%, $(MAKEFLAGS))),,+)$(CARGO) $(1) $(cargo_build_flags) $(CARGO_EXTRA_FLAGS) $(cargo_extra_cli_flags)
endef

ifdef CARGO_CONTINUE_ON_ERROR
define RUN_CARGO
-$(RUN_CARGO_INNER)
endef
else
define RUN_CARGO
$(RUN_CARGO_INNER)
endef
endif

define CARGO_BUILD
$(call RUN_CARGO,rustc$(if $(BUILDSTATUS), --timings)$(if $(findstring k,$(filter-out --%, $(MAKEFLAGS))), --keep-going))
endef

cargo_host_linker_env_var := CARGO_TARGET_$(call varize,$(RUST_HOST_TARGET))_LINKER
cargo_linker_env_var := CARGO_TARGET_$(call varize,$(RUST_TARGET))_LINKER

export MOZ_CLANG_NEWER_THAN_RUSTC_LLVM
export MOZ_CARGO_WRAP_LDFLAGS
export MOZ_CARGO_WRAP_LD
export MOZ_CARGO_WRAP_LD_CXX
export MOZ_CARGO_WRAP_HOST_LDFLAGS
export MOZ_CARGO_WRAP_HOST_LD
export MOZ_CARGO_WRAP_HOST_LD_CXX
ifeq (WINNT,$(HOST_OS_ARCH))
export $(cargo_host_linker_env_var):=$(topsrcdir)/build/cargo-host-linker.bat
export $(cargo_linker_env_var):=$(topsrcdir)/build/cargo-linker.bat
WRAP_HOST_LINKER_LIBPATHS:=$(HOST_LINKER_LIBPATHS_BAT)
else
export $(cargo_host_linker_env_var):=$(topsrcdir)/build/cargo-host-linker
export $(cargo_linker_env_var):=$(topsrcdir)/build/cargo-linker
WRAP_HOST_LINKER_LIBPATHS:=$(HOST_LINKER_LIBPATHS)
endif

$(TARGET_RECIPES): MOZ_CARGO_WRAP_LDFLAGS:=$(filter-out -fsanitize=cfi% -framework Cocoa -lobjc AudioToolbox ExceptionHandling -fprofile-% -Wl$(COMMA)--build-id=uuid,$(LDFLAGS))

ifneq (,$(filter -Zsanitizer=%,$(RUSTFLAGS)))
$(if $(filter -Zsanitizer=thread,$(RUSTFLAGS)),$(TARGET_RECIPES),force-cargo-program-build): MOZ_CARGO_WRAP_LDFLAGS:=$(filter-out -fsanitize=%,$(MOZ_CARGO_WRAP_LDFLAGS))
endif

ifeq (WINNT_clang,$(OS_ARCH)_$(CC_TYPE))
force-cargo-program-build: MOZ_CARGO_WRAP_LDFLAGS+=-L$(topobjdir)/build/win32 -lunwind
force-cargo-program-build: CARGO_RUSTCFLAGS += -C default-linker-libraries=yes
endif

ifeq (Darwin,$(OS_ARCH))
ifeq (,$(filter -Zsanitizer=%,$(RUSTFLAGS)))
ifneq (,$(filter -fsanitize=%,$(LDFLAGS)))
$(TARGET_RECIPES): RUSTFLAGS += -C default-linker-libraries=yes
endif
endif
endif

$(HOST_RECIPES): MOZ_CARGO_WRAP_LDFLAGS:=$(HOST_LDFLAGS) $(WRAP_HOST_LINKER_LIBPATHS)
$(TARGET_RECIPES) $(HOST_RECIPES): MOZ_CARGO_WRAP_HOST_LDFLAGS:=$(HOST_LDFLAGS) $(WRAP_HOST_LINKER_LIBPATHS)

ifeq (,$(filter clang-cl,$(CC_TYPE)))
$(TARGET_RECIPES): MOZ_CARGO_WRAP_LD:=$(CC)
$(TARGET_RECIPES): MOZ_CARGO_WRAP_LD_CXX:=$(CXX)
else
$(TARGET_RECIPES): MOZ_CARGO_WRAP_LD:=$(LINKER)
$(TARGET_RECIPES): MOZ_CARGO_WRAP_LD_CXX:=$(LINKER)
endif

ifeq (,$(filter clang-cl,$(HOST_CC_TYPE)))
$(HOST_RECIPES): MOZ_CARGO_WRAP_LD:=$(HOST_CC)
$(HOST_RECIPES): MOZ_CARGO_WRAP_LD_CXX:=$(HOST_CXX)
$(TARGET_RECIPES) $(HOST_RECIPES): MOZ_CARGO_WRAP_HOST_LD:=$(HOST_CC)
$(TARGET_RECIPES) $(HOST_RECIPES): MOZ_CARGO_WRAP_HOST_LD_CXX:=$(HOST_CXX)
else
$(HOST_RECIPES): MOZ_CARGO_WRAP_LD:=$(HOST_LINKER)
$(HOST_RECIPES): MOZ_CARGO_WRAP_LD_CXX:=$(HOST_LINKER)
$(TARGET_RECIPES) $(HOST_RECIPES): MOZ_CARGO_WRAP_HOST_LD:=$(HOST_LINKER)
$(TARGET_RECIPES) $(HOST_RECIPES): MOZ_CARGO_WRAP_HOST_LD_CXX:=$(HOST_LINKER)
endif

define make_default_rule
$(1):

endef

escape_sequence=_^_^_^_
escape_spaces = $(subst \ ,$(escape_sequence),$(1))
unescape_spaces = $(subst $(escape_sequence),\ ,$(1))

define make_cargo_rule
$(notdir $(1))_deps := $$(call unescape_spaces,$$(call normalize_sep,$$(wordlist 2, 10000000, $$(call escape_spaces,$$(if $$(wildcard $(basename $(1)).d),$$(shell cat $(basename $(1)).d))))))
$(1): $(CARGO_FILE) $(3) $(topsrcdir)/Cargo.lock $$(if $$($(notdir $(1))_deps),$$($(notdir $(1))_deps),$(2))
	$$(REPORT_BUILD)
	$$(if $$($(notdir $(1))_deps),+$(MAKE) $(2),:)
	@touch $$@

$$(foreach dep, $$(call escape_spaces,$$($(notdir $(1))_deps)),$$(eval $$(call make_default_rule,$$(call unescape_spaces,$$(dep)))))
endef

ifdef RUST_LIBRARY_FILE

rust_features_flag := --features '$(addsuffix $(COMMA),$(RUST_LIBRARY_FEATURES))mozilla-central-workspace-hack'

ifeq (WASI,$(OS_ARCH))
force-cargo-library-build: CARGO_RUSTCFLAGS += -C target-feature=-crt-static
endif

force-cargo-library-build:
	$(call BUILDSTATUS,START_Rust $(notdir $(RUST_LIBRARY_FILE)))
	$(call CARGO_BUILD) --lib $(cargo_crate_type_flag) $(cargo_target_flag) $(rust_features_flag) -- $(cargo_rustc_flags)
	$(call BUILDSTATUS,END_Rust $(notdir $(RUST_LIBRARY_FILE)))
ifndef MOZ_PROFILE_GENERATE
ifeq ($(OS_ARCH), Linux)
ifeq (,$(rustflags_sancov)$(MOZ_ASAN)$(MOZ_TSAN)$(MOZ_UBSAN))
ifndef MOZ_LTO_RUST_CROSS
ifneq (,$(filter -Clto,$(cargo_rustc_flags)))
	$(call py_action,check_binary $(@F),--networking $(RUST_LIBRARY_FILE))
endif
endif
endif
endif
endif

$(eval $(call make_cargo_rule,$(RUST_LIBRARY_FILE),force-cargo-library-build))

SUGGEST_INSTALL_ON_FAILURE = (ret=$$?; if [ $$ret = 101 ]; then echo If $1 is not installed, install it using: cargo install $1; fi; exit $$ret)

ifndef CARGO_NO_AUTO_ARG
force-cargo-library-%:
	$(call RUN_CARGO,$*) --lib $(cargo_target_flag) $(rust_features_flag) || $(call SUGGEST_INSTALL_ON_FAILURE,cargo-$*)
else
force-cargo-library-%:
	$(call RUN_CARGO,$*) || $(call SUGGEST_INSTALL_ON_FAILURE,cargo-$*)
endif

else
force-cargo-library-%:
	@true

endif 

ifdef HOST_RUST_LIBRARY_FILE

host_rust_features_flag := --features '$(addsuffix $(COMMA),$(HOST_RUST_LIBRARY_FEATURES))mozilla-central-workspace-hack'

force-cargo-host-library-build:
	$(call BUILDSTATUS,START_Rust $(notdir $(HOST_RUST_LIBRARY_FILE)))
	$(call CARGO_BUILD) --lib $(cargo_host_flag) $(host_rust_features_flag)
	$(call BUILDSTATUS,END_Rust $(notdir $(HOST_RUST_LIBRARY_FILE)))

$(eval $(call make_cargo_rule,$(HOST_RUST_LIBRARY_FILE),force-cargo-host-library-build))

ifndef CARGO_NO_AUTO_ARG
force-cargo-host-library-%:
	$(call RUN_CARGO,$*) --lib $(cargo_host_flag) $(host_rust_features_flag)
else
force-cargo-host-library-%:
	$(call RUN_CARGO,$*) --lib $(filter-out --release $(cargo_host_flag)) $(host_rust_features_flag)
endif

else
force-cargo-host-library-%:
	@true
endif 

ifdef RUST_PROGRAMS

program_features_flag := --features '$(addsuffix $(COMMA),$(RUST_PROGRAM_FEATURES))mozilla-central-workspace-hack'

force-cargo-program-build: $(call resfile,module)
	$(call BUILDSTATUS,START_Rust $(RUST_CARGO_PROGRAMS))
	$(call CARGO_BUILD) $(addprefix --bin ,$(RUST_CARGO_PROGRAMS)) $(cargo_target_flag) $(program_features_flag) -- $(addprefix -C link-arg=$(CURDIR)/,$(call resfile,module)) $(CARGO_RUSTCFLAGS)
	$(call BUILDSTATUS,END_Rust $(RUST_CARGO_PROGRAMS))

$(foreach RUST_PROGRAM,$(RUST_PROGRAMS), $(eval $(call make_cargo_rule,$(RUST_PROGRAM),force-cargo-program-build,$(call resfile,module))))

ifndef CARGO_NO_AUTO_ARG
force-cargo-program-%:
	$(call RUN_CARGO,$*) $(addprefix --bin ,$(RUST_CARGO_PROGRAMS)) $(cargo_target_flag) $(program_features_flag)
else
force-cargo-program-%:
	$(call RUN_CARGO,$*)
endif

else
force-cargo-program-%:
	@true
endif 
ifdef HOST_RUST_PROGRAMS

host_program_features_flag := --features '$(addsuffix $(COMMA),$(HOST_RUST_PROGRAM_FEATURES))mozilla-central-workspace-hack'

force-cargo-host-program-build:
	$(call BUILDSTATUS,START_Rust $(HOST_RUST_CARGO_PROGRAMS))
	$(call CARGO_BUILD) $(addprefix --bin ,$(HOST_RUST_CARGO_PROGRAMS)) $(cargo_host_flag) $(host_program_features_flag)
	$(call BUILDSTATUS,END_Rust $(HOST_RUST_CARGO_PROGRAMS))

$(foreach HOST_RUST_PROGRAM,$(HOST_RUST_PROGRAMS), $(eval $(call make_cargo_rule,$(HOST_RUST_PROGRAM),force-cargo-host-program-build)))

ifndef CARGO_NO_AUTO_ARG
force-cargo-host-program-%:
	$(call BUILDSTATUS,START_Rust $(HOST_RUST_CARGO_PROGRAMS))
	$(call RUN_CARGO,$*) $(addprefix --bin ,$(HOST_RUST_CARGO_PROGRAMS)) $(cargo_host_flag) $(host_program_features_flag)
	$(call BUILDSTATUS,END_Rust $(HOST_RUST_CARGO_PROGRAMS))
else
force-cargo-host-program-%:
	$(call RUN_CARGO,$*) $(addprefix --bin ,$(HOST_RUST_CARGO_PROGRAMS)) $(filter-out --release $(cargo_target_flag))
endif

else
force-cargo-host-program-%:
	@true

endif 
