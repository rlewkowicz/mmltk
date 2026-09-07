# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


varize = $(subst -,_,$(subst a,A,$(subst b,B,$(subst c,C,$(subst d,D,$(subst e,E,$(subst f,F,$(subst g,G,$(subst h,H,$(subst i,I,$(subst j,J,$(subst k,K,$(subst l,L,$(subst m,M,$(subst n,N,$(subst o,O,$(subst p,P,$(subst q,Q,$(subst r,R,$(subst s,S,$(subst t,T,$(subst u,U,$(subst v,V,$(subst w,W,$(subst x,X,$(subst y,Y,$(subst z,Z,$1)))))))))))))))))))))))))))

ifdef INCLUDED_CONFIG_MK
$(error Do not include config.mk twice!)
endif
INCLUDED_CONFIG_MK = 1

EXIT_ON_ERROR = set -e; # Shell loops continue past errors without this.

ifndef topsrcdir
topsrcdir	= $(DEPTH)
endif

ifndef INCLUDED_AUTOCONF_MK
include $(DEPTH)/config/autoconf.mk
endif

-include $(DEPTH)/.mozconfig.mk

MDDEPDIR := .deps

ifndef EXTERNALLY_MANAGED_MAKE_FILE
ifndef STANDALONE_MAKEFILE
GLOBAL_DEPS += backend.mk
include backend.mk

$(foreach tier,$(RUNNABLE_TIERS),$(eval $(if $(filter .,$(DEPTH)),recurse_$(tier):,$(tier)::) $($(call varize,$(tier))_TARGETS)))
endif

endif

space = $(NULL) $(NULL)

__depth := $(subst /, ,$(DEPTH))
ifeq (.,$(__depth))
__depth :=
endif
$(foreach __d,$(__depth) .,$(eval __depth = $(wordlist 2,$(words $(__depth)),$(__depth))$(eval -include $(subst $(space),/,$(strip $(srcdir) $(__depth) defs.mk)))))

COMMA = ,

CHECK_VARS := \
 XPI_NAME \
 LIBRARY_NAME \
 MODULE \
 DEPTH \
 XPI_PKGNAME \
 SHARED_LIBRARY_NAME \
 SONAME \
 STATIC_LIBRARY_NAME \
 $(NULL)

check-variable = $(if $(filter-out 0 1,$(words $($(x))z)),$(error Spaces are not allowed in $(x)))

$(foreach x,$(CHECK_VARS),$(check-variable))

ifndef INCLUDED_FUNCTIONS_MK
include $(MOZILLA_DIR)/config/makefiles/functions.mk
endif

RM = rm -f

FINAL_TARGET ?= $(if $(XPI_NAME),$(DIST)/xpi-stage/$(XPI_NAME),$(DIST)/bin)$(DIST_SUBDIR:%=/%)
FINAL_TARGET_FROZEN := '$(FINAL_TARGET)'

ifdef XPI_NAME
ACDEFINES += -DXPI_NAME=$(XPI_NAME)
endif

CC := $(CC_WRAPPER) $(CC)
CXX := $(CXX_WRAPPER) $(CXX)
MKDIR ?= mkdir
SLEEP ?= sleep
TOUCH ?= touch

ifdef SIMPLE_PROGRAMS
NO_PROFILE_GUIDED_OPTIMIZE = 1
endif

ifdef MOZ_PROFILE_GENERATE
ifneq (1,$(NO_PROFILE_GUIDED_OPTIMIZE))
PGO_CFLAGS += -DNS_FREE_PERMANENT_DATA=1
PGO_CFLAGS += $(if $(filter $(notdir $<),$(notdir $(NO_PROFILE_GUIDED_OPTIMIZE))),,$(PROFILE_GEN_CFLAGS))
endif 
PGO_LDFLAGS += $(PROFILE_GEN_LDFLAGS)
endif 

ifdef MOZ_PROFILE_USE
ifneq (1,$(NO_PROFILE_GUIDED_OPTIMIZE))
PGO_CFLAGS += $(if $(filter $(notdir $<),$(notdir $(NO_PROFILE_GUIDED_OPTIMIZE))),,$(PROFILE_USE_CFLAGS))
endif 
PGO_LDFLAGS += $(PROFILE_USE_LDFLAGS)
endif 

LOCALE_TOPDIR ?= $(topsrcdir)
MAKE_JARS_FLAGS = \
	-t $(LOCALE_TOPDIR) \
	-f $(MOZ_JAR_MAKER_FILE_FORMAT) \
	$(NULL)

ifdef USE_EXTENSION_MANIFEST
MAKE_JARS_FLAGS += -e
endif

TAR_CREATE_FLAGS = -chf

CCC = $(CXX)

INCLUDES = \
  -I$(srcdir) \
  -I$(CURDIR) \
  $(LOCAL_INCLUDES) \
  -I$(ABS_DIST)/include \
  $(NULL)

include $(MOZILLA_DIR)/config/static-checking-config.mk

ifndef MOZ_LTO
MOZ_LTO_CFLAGS :=
MOZ_LTO_LDFLAGS :=
endif

LDFLAGS		= $(MOZ_LTO_LDFLAGS) $(COMPUTED_LDFLAGS) $(PGO_LDFLAGS)

COMPILE_CFLAGS	= $(MOZ_LTO_CFLAGS) $(COMPUTED_CFLAGS) $(PGO_CFLAGS) $(_DEPEND_CFLAGS) $(MK_COMPILE_DEFINES)
COMPILE_CXXFLAGS = $(MOZ_LTO_CFLAGS) $(COMPUTED_CXXFLAGS) $(PGO_CFLAGS) $(_DEPEND_CFLAGS) $(MK_COMPILE_DEFINES)
COMPILE_CMFLAGS = $(MOZ_LTO_CFLAGS) $(OS_COMPILE_CMFLAGS) $(MOZBUILD_CMFLAGS)
COMPILE_CMMFLAGS = $(MOZ_LTO_CFLAGS) $(OS_COMPILE_CMMFLAGS) $(MOZBUILD_CMMFLAGS)
ASFLAGS = $(COMPUTED_ASFLAGS)
SFLAGS = $(COMPUTED_SFLAGS)

HOST_CFLAGS = $(COMPUTED_HOST_CFLAGS) $(_HOST_DEPEND_CFLAGS)
HOST_CXXFLAGS = $(COMPUTED_HOST_CXXFLAGS) $(_HOST_DEPEND_CFLAGS)
HOST_C_LDFLAGS = $(COMPUTED_HOST_LDFLAGS) $(COMPUTED_HOST_C_LDFLAGS)
HOST_CXX_LDFLAGS = $(COMPUTED_HOST_LDFLAGS) $(COMPUTED_HOST_CXX_LDFLAGS)

WASM_CFLAGS = $(COMPUTED_WASM_CFLAGS) $(_DEPEND_CFLAGS) $(MK_COMPILE_DEFINES)
WASM_CXXFLAGS = $(COMPUTED_WASM_CXXFLAGS) $(_DEPEND_CFLAGS) $(MK_COMPILE_DEFINES)

ifdef MOZ_LTO
ifeq (Darwin,$(OS_TARGET))
LDFLAGS += -Wl,-object_path_lto,$(@F).lto.o/
endif
endif

define colorize_flags
ifeq (,$(filter $(COLOR_CFLAGS:-f%=-fno-%),$$(1))$(findstring $(COLOR_CFLAGS),$$(1)))
$(1) += $(COLOR_CFLAGS)
endif
endef

color_flags_vars := \
  COMPILE_CFLAGS \
  COMPILE_CXXFLAGS \
  COMPILE_CMFLAGS \
  COMPILE_CMMFLAGS \
  WASM_CFLAGS \
  WASM_CXXFLAGS \
  $(NULL)

ifdef MACH_STDOUT_ISATTY
ifdef COLOR_CFLAGS
ifneq ($(TERM_PROGRAM),iTerm.app)
$(foreach var,$(color_flags_vars),$(eval $(call colorize_flags,$(var))))
endif
endif
endif


DEPENDENCIES	= .md

ifdef INCLUDE
export INCLUDE
endif

ifdef LIB
export LIB
endif

ifdef SCCACHE_BASEDIRS
export SCCACHE_BASEDIRS
endif

ifeq ($(OS_ARCH),WINNT)
ifneq (,$(filter msvc clang-cl,$(CC_TYPE)))
WIN32_EXE_LDFLAGS += $(WIN32_EXE_DEFAULT_LDFLAGS)
else
MOZ_PROGRAM_LDFLAGS += $(WIN32_EXE_DEFAULT_LDFLAGS)
endif
endif

-include $(topsrcdir)/$(MOZ_BUILD_APP)/app-config.mk


ifeq ($(OS_ARCH),Darwin)
ifndef NSDISTMODE
NSDISTMODE=absolute_symlink
endif
PWD := $(CURDIR)
endif

NSINSTALL_PY := $(PYTHON3) $(abspath $(MOZILLA_DIR)/config/nsinstall.py)
ifneq (,$(or $(filter WINNT,$(HOST_OS_ARCH)),$(if $(COMPILE_ENVIRONMENT),,1)))
NSINSTALL = $(NSINSTALL_PY)
else
NSINSTALL = $(DEPTH)/config/nsinstall$(HOST_BIN_SUFFIX)
endif 


ifeq (,$(CROSS_COMPILE)$(filter-out WINNT, $(OS_ARCH)))
INSTALL = $(NSINSTALL) -t

else

INSTALL         = $(if $(filter copy, $(NSDISTMODE)), $(NSINSTALL) -t, $(if $(filter absolute_symlink, $(NSDISTMODE)), $(NSINSTALL) -L $(PWD), $(NSINSTALL) -R))

endif 

install_cmd ?= $(INSTALL) $(1)

SYSINSTALL	= $(NSINSTALL) -t
sysinstall_cmd = install_cmd


AB_CD = $(MOZ_UI_LOCALE)

include $(MOZILLA_DIR)/config/AB_rCD.mk

ACDEFINES += -DAB_CD=$(AB_CD)

EXPAND_LOCALE_SRCDIR = $(if $(filter en-US,$(AB_CD)),$(LOCALE_TOPDIR)/$(1)/en-US,$(REAL_LOCALE_MERGEDIR)/$(subst /locales,,$(1)))

ifdef relativesrcdir
LOCALE_RELATIVEDIR ?= $(relativesrcdir)
endif

ifdef LOCALE_RELATIVEDIR
LOCALE_SRCDIR ?= $(call EXPAND_LOCALE_SRCDIR,$(LOCALE_RELATIVEDIR))
endif

ifdef relativesrcdir
MAKE_JARS_FLAGS += --relativesrcdir=$(LOCALE_RELATIVEDIR)
ifneq (en-US,$(AB_CD))
ifdef IS_LANGUAGE_REPACK
MAKE_JARS_FLAGS += --l10n-base=$(REAL_LOCALE_MERGEDIR)
endif
else
MAKE_JARS_FLAGS += -c $(LOCALE_SRCDIR)
endif 
else
MAKE_JARS_FLAGS += -c $(LOCALE_SRCDIR)
endif 

MERGE_FILE = $(LOCALE_SRCDIR)/$(1)
MERGE_RELATIVE_FILE = $(call EXPAND_LOCALE_SRCDIR,$(2))/$(1)

# The per-locale l10n coverage index is generated by the merge step into the
INSTALL_L10N_COVERAGE = \
  test ! -f $(REAL_LOCALE_MERGEDIR)/coverage.json || { \
    $(NSINSTALL) -D $(1)/localization/$(AB_CD) && \
    cp $(REAL_LOCALE_MERGEDIR)/coverage.json $(1)/localization/$(AB_CD)/coverage.json; \
  }

ifeq (,$(findstring s, $(filter-out --%, $(MAKEFLAGS))))
export BUILD_VERBOSE_LOG = 1
endif
