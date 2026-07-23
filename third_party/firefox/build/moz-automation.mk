# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

MOZ_AUTOMATION_PACKAGE_TESTS = 0

ifdef CROSS_COMPILE
ifeq ($(HOST_OS_ARCH),$(OS_TARGET))
ifneq (,$(filter x86%,$(TARGET_CPU)))
FUZZY_CROSS_COMPILE =
else
FUZZY_CROSS_COMPILE = 1
endif
else
FUZZY_CROSS_COMPILE = 1
endif
endif

ifneq (,$(USE_ARTIFACT)$(FUZZY_CROSS_COMPILE)$(MOZ_PROFILE_GENERATE))
MOZ_AUTOMATION_CHECK := 0
endif

ifneq (,$(filter automation/%,$(MAKECMDGOALS)))
ifneq (4.0,$(firstword $(sort 4.0 $(MAKE_VERSION))))
.NOTPARALLEL:
endif
endif

ifndef JS_STANDALONE
include $(topsrcdir)/toolkit/mozapps/installer/package-name.mk
include $(topsrcdir)/toolkit/mozapps/installer/upload-files.mk

DIST_FILES =
endif

tier_MOZ_AUTOMATION_BUILD_SYMBOLS = buildsymbols
tier_MOZ_AUTOMATION_PACKAGE = package
tier_MOZ_AUTOMATION_PACKAGE_TESTS = package-tests
tier_MOZ_AUTOMATION_PACKAGE_GENERATED_SOURCES = package-generated-sources
tier_MOZ_AUTOMATION_UPLOAD_SYMBOLS = uploadsymbols
tier_MOZ_AUTOMATION_UPLOAD = upload
tier_MOZ_AUTOMATION_CHECK = check

moz_automation_symbols = \
  MOZ_AUTOMATION_PACKAGE_TESTS \
  MOZ_AUTOMATION_UPLOAD \
  $(NULL)

ifneq (,$(COMPILE_ENVIRONMENT)$(MOZ_ARTIFACT_BUILDS))
moz_automation_symbols += \
  MOZ_AUTOMATION_BUILD_SYMBOLS \
  MOZ_AUTOMATION_UPLOAD_SYMBOLS \
  MOZ_AUTOMATION_PACKAGE \
  MOZ_AUTOMATION_PACKAGE_GENERATED_SOURCES \
  MOZ_AUTOMATION_CHECK \
  $(NULL)
endif
MOZ_AUTOMATION_TIERS := $(foreach sym,$(moz_automation_symbols),$(if $(filter 1,$($(sym))),$(tier_$(sym))))

automation/uploadsymbols: automation/buildsymbols

automation/upload: automation/package
automation/upload: automation/package-tests
automation/upload: automation/buildsymbols
automation/upload: automation/package-generated-sources

automation/check: $(addprefix automation/,$(filter-out check,$(MOZ_AUTOMATION_TIERS)))

automation/build: $(addprefix automation/,$(MOZ_AUTOMATION_TIERS))
	@echo Automation steps completed.

AUTOMATION_EXTRA_CMDLINE-check = --keep-going

define automation_commands
@+$(PYTHON3) $(topsrcdir)/config/run-and-prefix.py $1 $(MAKE) $1 $(AUTOMATION_EXTRA_CMDLINE-$1)
endef

automation/%:
	$(if $(filter $*,$(filter-out $(ALL_TIERS),$(MOZ_AUTOMATION_TIERS))),$(call BUILDSTATUS,TIER_START $*))
	$(if $(filter $*,$(MOZ_AUTOMATION_TIERS)),$(call automation_commands,$*))
	$(if $(filter $*,$(filter-out $(ALL_TIERS),$(MOZ_AUTOMATION_TIERS))),$(call BUILDSTATUS,TIER_FINISH $*))
