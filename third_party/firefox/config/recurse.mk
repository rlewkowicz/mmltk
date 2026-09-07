# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

ifndef INCLUDED_RULES_MK
include $(topsrcdir)/config/rules.mk
endif


ifeq (.,$(DEPTH))

include root.mk

$(RUNNABLE_TIERS)::
	$(if $(filter $@,$(MAKECMDGOALS)),$(call BUILDSTATUS,TIERS $@),)
	$(call BUILDSTATUS,TIER_START $@)
	+$(MAKE) recurse_$@
	$(call BUILDSTATUS,TIER_FINISH $@)

binaries::
	+$(MAKE) recurse_compile
ifdef MOZ_MACBUNDLE_NAME
	+$(MAKE) recurse_tools
endif

CURRENT_TIER := $(filter $(foreach tier,$(RUNNABLE_TIERS) $(non_default_tiers),recurse_$(tier) $(tier)-deps),$(MAKECMDGOALS))
ifneq (,$(filter-out 0 1,$(words $(CURRENT_TIER))))
$(error $(CURRENT_TIER) not supported on the same make command line)
endif
CURRENT_TIER := $(subst recurse_,,$(CURRENT_TIER:-deps=))

ifdef CURRENT_TIER
ifeq (0,$(MAKELEVEL))
export NO_RECURSE_MAKELEVEL=1
else
export NO_RECURSE_MAKELEVEL=$(word $(MAKELEVEL),2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20)
endif
endif

RECURSE = $(if $(RECURSE_TRACE_ONLY),@echo $2/$1,$(call SUBMAKE,$1,$2))

CURRENT_DIRS := $($(CURRENT_TIER)_dirs)

.PHONY: $(pre_compile_targets) $(compile_targets) $(syms_targets)
$(pre_compile_targets) $(compile_targets) $(syms_targets):
	$(if $(filter $(RECURSE_BASE_DIR)%,$@),$(call RECURSE,$(@F),$(@D)))

$(syms_targets): %/syms: %/target

ifdef MOZ_AUTOMATION
ifeq (1,$(MOZ_AUTOMATION_BUILD_SYMBOLS))
recurse_compile: $(syms_targets)
endif
endif

.PHONY: recurse_syms
recurse_syms: $(syms_targets)

ifneq ($(CURRENT_TIER),compile)

$(addsuffix /$(CURRENT_TIER),$(CURRENT_DIRS)): %/$(CURRENT_TIER):
	$(call RECURSE,$(CURRENT_TIER),$*)

.PHONY: $(addsuffix /$(CURRENT_TIER),$(CURRENT_DIRS))

$(addsuffix /Makefile,$(CURRENT_DIRS)) $(addsuffix /backend.mk,$(CURRENT_DIRS)):

ifeq ($(CURRENT_TIER),export)
$(addsuffix /$(CURRENT_TIER),$(filter-out config,$(CURRENT_DIRS))): config/$(CURRENT_TIER)

ifdef COMPILE_ENVIRONMENT
ifneq (,$(filter config/host, $(compile_targets)))
$(addsuffix /$(CURRENT_TIER),$(CURRENT_DIRS)): config/host
endif
endif
endif

endif 

else

ifeq ($(NO_RECURSE_MAKELEVEL),$(MAKELEVEL))

$(RUNNABLE_TIERS)::

else

define CREATE_SUBTIER_TRAVERSAL_RULE
.PHONY: $(1)

$(1):: $$(SUBMAKEFILES)
	$$(LOOP_OVER_DIRS)

endef

$(foreach subtier,$(filter-out compile,$(RUNNABLE_TIERS)),$(eval $(call CREATE_SUBTIER_TRAVERSAL_RULE,$(subtier))))

ifndef TOPLEVEL_BUILD
ifdef COMPILE_ENVIRONMENT
compile::
	@$(MAKE) -C $(DEPTH) compile RECURSE_BASE_DIR=$(relativesrcdir)/
endif 
endif

endif 

endif 

recurse:
	@$(RECURSED_COMMAND)
	$(LOOP_OVER_DIRS)

ifeq (.,$(DEPTH))

ifeq ($(MOZ_WIDGET_TOOLKIT),android)
recurse_pre-export: mobile/android/pre-export
endif

ifeq ($(TARGET_ENDIANNESS),big)
config/external/icu/data/target-objects: config/external/icu/data/$(MDDEPDIR)/icudt$(MOZ_ICU_VERSION)b.dat.stub
config/external/icu/data/$(MDDEPDIR)/icudt$(MOZ_ICU_VERSION)b.dat.stub: config/external/icu/icupkg/host
endif

ifdef ENABLE_CLANG_PLUGIN
$(filter %/target %/target-objects,$(filter-out config/export config/host build/unix/stdc++compat/% build/clang-plugin/%,$(compile_targets))): build/clang-plugin/host build/clang-plugin/tests/target-objects
build/clang-plugin/tests/target-objects: build/clang-plugin/host
ifdef JS_STANDALONE
ifeq ($(CURRENT_TIER),export)
build/clang-plugin/tests/target-objects: js/src/export
endif
else
build/clang-plugin/tests/target-objects: mozilla-config.h
endif
endif

$(addprefix build/unix/stdc++compat/,target host) build/clang-plugin/host: config/export

CARGO_CONFIG_DEPS = $(DEPTH)/.cargo/config.toml
ifneq (,$(wildcard $(DEPTH)/.cargo/config))
CARGO_CONFIG_DEPS += $(MDDEPDIR)/cargo-config-cleanup.stub
endif
$(rust_targets): $(CARGO_CONFIG_DEPS)
ifndef TEST_MOZBUILD
recurse_pre-export: $(CARGO_CONFIG_DEPS)
endif

$(MDDEPDIR)/cargo-config-cleanup.stub:
	rm $(DEPTH)/.cargo/config
	touch $@
endif
