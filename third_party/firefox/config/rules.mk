# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

ifndef topsrcdir
$(error topsrcdir was not set))
endif

ifdef INCLUDED_RULES_MK
$(error Do not include rules.mk twice!)
endif
INCLUDED_RULES_MK = 1

ifndef INCLUDED_CONFIG_MK
include $(topsrcdir)/config/config.mk
endif

USE_AUTOTARGETS_MK = 1
include $(MOZILLA_DIR)/config/makefiles/makeutils.mk

ifdef REBUILD_CHECK
REPORT_BUILD = $(info $(shell $(PYTHON3) $(MOZILLA_DIR)/config/rebuild_check.py $@ $?))
REPORT_BUILD_VERBOSE = $(REPORT_BUILD)
else
REPORT_BUILD = $(info $(relativesrcdir)/$(notdir $@))

ifdef BUILD_VERBOSE_LOG
REPORT_BUILD_VERBOSE = $(REPORT_BUILD)
else
REPORT_BUILD_VERBOSE = $(call BUILDSTATUS,BUILD_VERBOSE $(relativesrcdir))
endif

endif

EXEC			= exec


ifndef LIBRARY
ifdef REAL_LIBRARY
ifdef NO_EXPAND_LIBS
LIBRARY			:= $(REAL_LIBRARY)
endif
endif
endif

ifdef FORCE_SHARED_LIB
ifdef MKSHLIB

ifdef LIB_IS_C_ONLY
MKSHLIB			= $(MKCSHLIB)
endif

endif 
endif 

ifeq ($(OS_ARCH),WINNT)

LINK_PDBFILE ?= $(basename $(@F)).pdb

ifdef MOZ_COPY_PDBS
ifneq ($(CC_TYPE),clang)
LINK_PDBFILE = $(basename $@).pdb
endif
endif

ifeq ($(CC_TYPE),clang-cl)

ifdef SIMPLE_PROGRAMS
COMPILE_PDB_FLAG ?= -Fd$(basename $(@F)).pdb
COMPILE_CFLAGS += $(COMPILE_PDB_FLAG)
COMPILE_CXXFLAGS += $(COMPILE_PDB_FLAG)
endif

ifdef MOZ_DEBUG
CODFILE=$(basename $(@F)).cod
endif

endif 
endif 

ifeq (arm-Darwin,$(TARGET_CPU)-$(OS_TARGET))
ifdef PROGRAM
MOZ_PROGRAM_LDFLAGS += -Wl,-rpath -Wl,@executable_path/Frameworks
endif
endif

ifeq ($(OS_ARCH),Darwin)
MOZ_PROGRAM_LDFLAGS += -Wl,-rpath,@executable_path
endif

ifeq ($(OS_ARCH),WINNT)
ifeq ($(CC_TYPE),clang)
MOZ_PROGRAM_LDFLAGS += -Wl,-pdb,$(dir $@)/$(LINK_PDBFILE)
endif
endif

ifeq ($(HOST_OS_ARCH),WINNT)
HOST_PDBFILE=$(basename $(@F)).pdb
HOST_PDB_FLAG ?= -PDB:$(HOST_PDBFILE)
HOST_C_LDFLAGS += -DEBUG $(HOST_PDB_FLAG)
HOST_CXX_LDFLAGS += -DEBUG $(HOST_PDB_FLAG)
endif

ifdef MOZ_PROFILE_GENERATE
$(foreach category,$(INSTALL_TARGETS),\
  $(eval $(category)_FILES := $(foreach file,$($(category)_FILES),$(if $(filter $(SIMPLE_PROGRAMS),$(notdir $(file))),,$(file)))))
SIMPLE_PROGRAMS :=
endif

ifdef COMPILE_ENVIRONMENT
ifndef TARGETS
TARGETS			= $(LIBRARY) $(SHARED_LIBRARY) $(PROGRAM) $(SIMPLE_PROGRAMS) $(HOST_PROGRAM) $(HOST_SIMPLE_PROGRAMS) $(HOST_SHARED_LIBRARY)
endif

COBJS = $(notdir $(CSRCS:.c=.$(OBJ_SUFFIX)))
CWASMOBJS = $(notdir $(WASM_CSRCS:.c=.$(WASM_OBJ_SUFFIX)))
SOBJS = $(notdir $(SSRCS:.S=.$(OBJ_SUFFIX)))
CPPOBJS = $(notdir $(addsuffix .$(OBJ_SUFFIX),$(basename $(CPPSRCS))))
CPPWASMOBJS = $(notdir $(addsuffix .$(WASM_OBJ_SUFFIX),$(basename $(WASM_CPPSRCS))))
CMOBJS = $(notdir $(CMSRCS:.m=.$(OBJ_SUFFIX)))
CMMOBJS = $(notdir $(CMMSRCS:.mm=.$(OBJ_SUFFIX)))
ASOBJS = $(notdir $(addsuffix .$(OBJ_SUFFIX),$(basename $(ASFILES))))
RS_STATICLIB_CRATE_OBJ = $(addprefix lib,$(notdir $(RS_STATICLIB_CRATE_SRC:.rs=.$(LIB_SUFFIX))))
ifndef OBJS
_OBJS = $(COBJS) $(SOBJS) $(CPPOBJS) $(CMOBJS) $(CMMOBJS) $(ASOBJS) $(CWASMOBJS) $(CPPWASMOBJS)
OBJS = $(strip $(_OBJS))
endif

HOST_COBJS = $(addprefix host_,$(notdir $(HOST_CSRCS:.c=.$(OBJ_SUFFIX))))
HOST_CPPOBJS = $(addprefix host_,$(notdir $(addsuffix .$(OBJ_SUFFIX),$(basename $(HOST_CPPSRCS)))))
ifndef HOST_OBJS
_HOST_OBJS = $(HOST_COBJS) $(HOST_CPPOBJS)
HOST_OBJS = $(strip $(_HOST_OBJS))
endif
else
LIBRARY :=
SHARED_LIBRARY :=
IMPORT_LIBRARY :=
REAL_LIBRARY :=
PROGRAM :=
SIMPLE_PROGRAMS :=
HOST_SHARED_LIBRARY :=
HOST_PROGRAM :=
HOST_SIMPLE_PROGRAMS :=
endif

ifdef MACH
ifndef NO_BUILDSTATUS_MESSAGES
define BUILDSTATUS
@echo 'BUILDSTATUS@$(relativesrcdir) $1'

endef
endif
endif

define SUBMAKE 
+@$(MAKE) $(if $(2),-C $(2)) $(1)

endef 

define TIER_DIR_SUBMAKE
$(call SUBMAKE,$(4),$(3),$(5))

endef 

ifneq (,$(strip $(DIRS)))
LOOP_OVER_DIRS = \
  $(foreach dir,$(DIRS),$(call SUBMAKE,$@,$(dir)))
endif

ifndef PROGOBJS
PROGOBJS		= $(OBJS)
endif

ifndef HOST_PROGOBJS
HOST_PROGOBJS		= $(HOST_OBJS)
endif

TAG_PROGRAM		= xargs etags -a

ifneq ($(HOST_CPPSRCS),)
HOST_CPP_PROG_LINK	= 1
endif


ifeq ($(OS_ARCH),Darwin)
ifneq (,$(SHARED_LIBRARY))
_LOADER_PATH := @rpath
EXTRA_DSO_LDOPTS	+= -dynamiclib -install_name $(_LOADER_PATH)/$(@F) -compatibility_version 1 -current_version 1
endif
endif


ifeq ($(OS_ARCH),GNU)
OS_CPPFLAGS += -DPATH_MAX=1024 -DMAXPATHLEN=1024
endif

ifeq ($(OS_ARCH),WINNT)
ifneq ($(CC_TYPE),clang-cl)
DSO_LDOPTS += -Wl,--out-implib -Wl,$(IMPORT_LIBRARY)
endif
endif

ifeq ($(USE_TVFS),1)
IFLAGS1 = -rb
IFLAGS2 = -rb
else
IFLAGS1 = -m 644
IFLAGS2 = -m 755
endif

ifeq (clang-cl_WINNT,$(CC_TYPE)_$(OS_ARCH))
OUTOPTION = -Fo# eol
else
OUTOPTION = -o 
endif 

ifeq (,$(CROSS_COMPILE))
HOST_OUTOPTION = $(OUTOPTION)
else
ifeq (WINNT_WINNT,$(HOST_OS_ARCH)_$(OS_ARCH))
ifneq (,$(filter-out clang-cl,$(HOST_CC_TYPE)))
$(error MSVC-style compilers should be used for host compilations!)
endif
HOST_OUTOPTION = -Fo# eol
else
HOST_OUTOPTION = -o 
endif
endif

ifndef MOZBUILD_BACKEND_CHECKED
ifndef MACH
ifndef TOPLEVEL_BUILD
BUILD_BACKEND_FILES := $(addprefix $(DEPTH)/backend.,$(addsuffix Backend,$(BUILD_BACKENDS)))
$(DEPTH)/backend.%Backend:
	$(error Build configuration changed. Build with |mach build| or run |mach build-backend| to regenerate build config)

define build_backend_rule
$(1): $$(shell cat $(1).in)

endef
$(foreach file,$(BUILD_BACKEND_FILES),$(eval $(call build_backend_rule,$(file))))

default:: $(BUILD_BACKEND_FILES)

export MOZBUILD_BACKEND_CHECKED=1
endif
endif
endif

default all::
	$(foreach tier,$(TIERS),$(call SUBMAKE,$(tier)))

ifdef BUILD_VERBOSE_LOG
ECHO := echo
QUIET :=
else
ECHO := true
QUIET := -q
endif

GLOBAL_DEPS += Makefile $(addprefix $(DEPTH)/config/,$(INCLUDED_AUTOCONF_MK)) $(MOZILLA_DIR)/config/config.mk

ifeq ($(MOZ_WIDGET_TOOLKIT),windows)
# We always build .res files for programs and shared libraries
resfile = $(notdir $1).res
resfile_for_manifest = $(if $(wildcard $(srcdir)/$(notdir $1).manifest),$(call resfile,$1))
else
resfile =
resfile_for_manifest =
endif

ifdef COMPILE_ENVIRONMENT
compile:: host target

host:: $(HOST_OBJS) $(HOST_PROGRAM) $(HOST_SIMPLE_PROGRAMS) $(HOST_RUST_PROGRAMS) $(HOST_SHARED_LIBRARY)

target:: $(filter-out $(MOZBUILD_NON_DEFAULT_TARGETS),$(LIBRARY) $(SHARED_LIBRARY) $(PROGRAM) $(SIMPLE_PROGRAMS) $(RUST_PROGRAMS))

ifndef LIBRARY
ifdef OBJS
target:: $(OBJS)
endif
endif

target-objects: $(OBJS) $(PROGOBJS) $(filter-out $(MOZBUILD_NON_DEFAULT_TARGETS),$(RUST_LIBRARY_FILE))
host-objects: $(HOST_OBJS) $(HOST_PROGOBJS) $(HOST_RUST_LIBRARY_FILE)

syms::

include $(MOZILLA_DIR)/config/makefiles/target_binaries.mk
endif

alltags:
	$(RM) TAGS
	find $(topsrcdir) -name dist -prune -o \( -name '*.[hc]' -o -name '*.cp' -o -name '*.cpp' -o -name '*.idl' \) -print | $(TAG_PROGRAM)

define EXPAND_CC_OR_CXX
$(if $(PROG_IS_C_ONLY_$(notdir $(1))),$(CC),$(CCC))
endef

$(PROGRAM): $(PROGOBJS) $(STATIC_LIBS) $(call resfile,$(PROGRAM)) $(GLOBAL_DEPS) $(call mkdir_deps,$(FINAL_TARGET))
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_Program $(@F))
ifeq (clang-cl_WINNT,$(CC_TYPE)_$(OS_ARCH))
	$(LINKER) -OUT:$@ -PDB:$(LINK_PDBFILE) -IMPLIB:$(basename $(@F)).lib $(WIN32_EXE_LDFLAGS) $(LDFLAGS) $(MOZ_PROGRAM_LDFLAGS) $($(notdir $@)_OBJS) $(filter %.res,$^) $(STATIC_LIBS) $(SHARED_LIBS) $(OS_LIBS)
else # !WINNT || !clang-cl
	$(call EXPAND_CC_OR_CXX,$@) -o $@ $(COMPUTED_CXX_LDFLAGS) $($(notdir $@)_OBJS) $(filter %.res,$^) $(WIN32_EXE_LDFLAGS) $(LDFLAGS) $(STATIC_LIBS) $(MOZ_PROGRAM_LDFLAGS) $(SHARED_LIBS) $(OS_LIBS)
	$(call py_action,check_binary $(@F),$@)
endif # WINNT && clang-cl

ifdef ENABLE_STRIP
	$(STRIP) $(STRIP_FLAGS) $@
endif
	$(call BUILDSTATUS,END_Program $(@F))

$(HOST_PROGRAM): $(HOST_PROGOBJS) $(HOST_LIBS) $(GLOBAL_DEPS) $(call mkdir_deps,$(DEPTH)/dist/host/bin)
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_Program $(@F))
ifeq (clang-cl_WINNT,$(HOST_CC_TYPE)_$(HOST_OS_ARCH))
	$(HOST_LINKER) -OUT:$@ -PDB:$(HOST_PDBFILE) $($(notdir $@)_OBJS) $(WIN32_EXE_LDFLAGS) $(HOST_LDFLAGS) $(HOST_LINKER_LIBPATHS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
else
ifeq ($(HOST_CPP_PROG_LINK),1)
	$(HOST_CXX) -o $@ $(HOST_CXX_LDFLAGS) $(HOST_LDFLAGS) $($(notdir $@)_OBJS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
else
	$(HOST_CC) -o $@ $(HOST_C_LDFLAGS) $(HOST_LDFLAGS) $($(notdir $@)_OBJS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
endif 
endif
	$(call BUILDSTATUS,END_Program $(@F))

define simple_program_deps
$1: $(1:$(BIN_SUFFIX)=.$(OBJ_SUFFIX)) $(STATIC_LIBS) $(call resfile_for_manifest,$1) $(GLOBAL_DEPS)
endef
$(foreach p,$(SIMPLE_PROGRAMS),$(eval $(call simple_program_deps,$(p))))

$(SIMPLE_PROGRAMS):
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_Program $(@F))
ifeq (clang-cl_WINNT,$(CC_TYPE)_$(OS_ARCH))
	$(LINKER) -out:$@ -pdb:$(LINK_PDBFILE) $($@_OBJS) $(filter %.res,$^) $(WIN32_EXE_LDFLAGS) $(LDFLAGS) $(MOZ_PROGRAM_LDFLAGS) $(STATIC_LIBS) $(SHARED_LIBS) $(OS_LIBS)
else
	$(call EXPAND_CC_OR_CXX,$@) $(COMPUTED_CXX_LDFLAGS) -o $@ $($@_OBJS) $(filter %.res,$^) $(WIN32_EXE_LDFLAGS) $(LDFLAGS) $(STATIC_LIBS) $(MOZ_PROGRAM_LDFLAGS) $(SHARED_LIBS) $(OS_LIBS)
	$(call py_action,check_binary $(@F),$@)
endif 

ifdef ENABLE_STRIP
	$(STRIP) $(STRIP_FLAGS) $@
endif
	$(call BUILDSTATUS,END_Program $(@F))

$(HOST_SIMPLE_PROGRAMS): host_%$(HOST_BIN_SUFFIX): $(HOST_LIBS) $(GLOBAL_DEPS)
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_Program $(@F))
ifeq (WINNT_clang-cl,$(HOST_OS_ARCH)_$(HOST_CC_TYPE))
	$(HOST_LINKER) -OUT:$@ -PDB:$(HOST_PDBFILE) $($(notdir $@)_OBJS) $(WIN32_EXE_LDFLAGS) $(HOST_LDFLAGS) $(HOST_LINKER_LIBPATHS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
else
ifneq (,$(HOST_CPPSRCS)$(USE_HOST_CXX))
	$(HOST_CXX) $(HOST_OUTOPTION)$@ $(HOST_CXX_LDFLAGS) $(HOST_LDFLAGS) $($(notdir $@)_OBJS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
else
	$(HOST_CC) $(HOST_OUTOPTION)$@ $(HOST_C_LDFLAGS) $(HOST_LDFLAGS) $($(notdir $@)_OBJS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
endif
endif
	$(call BUILDSTATUS,END_Program $(@F))

$(LIBRARY): $(OBJS) $(STATIC_LIBS) $(GLOBAL_DEPS)
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_StaticLib $@)
	$(RM) $(REAL_LIBRARY)
	$(AR) $(AR_FLAGS) $($@_OBJS)
	$(call BUILDSTATUS,END_StaticLib $@)

$(WASM_ARCHIVE): $(CWASMOBJS) $(CPPWASMOBJS) $(STATIC_LIBS) $(GLOBAL_DEPS)
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,START_WasmLib $@)
	$(RM) $(WASM_ARCHIVE)
	$(WASM_CXX) -o $@ -Wl,--export-all -Wl,--stack-first -Wl,-z,stack-size=$(if $(MOZ_OPTIMIZE),262144,1048576) -Wl,--no-entry -Wl,--import-memory -Wl,--import-table $(CWASMOBJS) $(CPPWASMOBJS) $(addprefix -l,$(WASM_LIBS))
	$(call BUILDSTATUS,END_WasmLib $@)

ifeq ($(OS_ARCH),WINNT)
$(IMPORT_LIBRARY): $(SHARED_LIBRARY) ;
endif

$(HOST_SHARED_LIBRARY): Makefile
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_SharedLib $@)
	$(RM) $@
ifneq (,$(filter clang-cl,$(HOST_CC_TYPE)))
	$(HOST_LINKER) -DLL -OUT:$@ $($(notdir $@)_OBJS) $(HOST_CXX_LDFLAGS) $(HOST_LDFLAGS) $(HOST_LINKER_LIBPATHS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
else
	$(HOST_CXX) $(HOST_OUTOPTION)$@ $($(notdir $@)_OBJS) $(HOST_CXX_LDFLAGS) $(HOST_LDFLAGS) $(HOST_LIBS) $(HOST_EXTRA_LIBS)
endif
	$(call BUILDSTATUS,END_SharedLib $@)


$(SHARED_LIBRARY): $(OBJS) $(call resfile,$(SHARED_LIBRARY)) $(STATIC_LIBS) $(GLOBAL_DEPS) $(call mkdir_deps,$(FINAL_TARGET))
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_SharedLib $@)
	$(RM) $@
	$(MKSHLIB) $(if $(filter clang-cl_WINNT,$(CC_TYPE)_$(OS_ARCH)),-IMPLIB:$(basename $(@F)).lib )$($(notdir $@)_OBJS) $(filter %.res,$^) $(RELRHACK_LDFLAGS) $(LDFLAGS) $(STATIC_LIBS) $(SHARED_LIBS) $(EXTRA_DSO_LDOPTS) $(MOZ_GLUE_LDFLAGS) $(OS_LIBS)
	$(call py_action,check_binary,$@)

ifeq (clang-cl_WINNT,$(CC_TYPE)_$(OS_ARCH))
endif	
	chmod +x $@
ifdef ENABLE_STRIP
	$(STRIP) $(STRIP_FLAGS) $@
endif
	$(call BUILDSTATUS,END_SharedLib $@)

define src_objdep
$(basename $3$(notdir $1)).$2: $1 $$(call mkdir_deps,$$(MDDEPDIR))
endef
$(foreach f,$(CSRCS) $(SSRCS) $(CPPSRCS) $(CMSRCS) $(CMMSRCS) $(ASFILES),$(eval $(call src_objdep,$(f),$(OBJ_SUFFIX))))
$(foreach f,$(HOST_CSRCS) $(HOST_CPPSRCS),$(eval $(call src_objdep,$(f),$(OBJ_SUFFIX),host_)))
$(foreach f,$(WASM_CSRCS) $(WASM_CPPSRCS),$(eval $(call src_objdep,$(f),wasm)))

mk_global_crate_libname = $(basename lib$(notdir $1)).$(LIB_SUFFIX)
crate_src_libdep = $(call mk_global_crate_libname,$1): $1 $$(call mkdir_deps,$$(MDDEPDIR))
$(foreach f,$(RS_STATICLIB_CRATE_SRC),$(eval $(call crate_src_libdep,$(f))))

$(OBJS) $(HOST_OBJS) $(PROGOBJS) $(HOST_PROGOBJS): $(GLOBAL_DEPS)

$(HOST_COBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(HOST_CC) $(HOST_OUTOPTION)$@ -c $(HOST_CPPFLAGS) $(HOST_CFLAGS) $(NSPR_CFLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

$(HOST_CPPOBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(HOST_CXX) $(HOST_OUTOPTION)$@ -c $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(NSPR_CFLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

$(COBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(CC) $(OUTOPTION)$@ -c $(COMPILE_CFLAGS) $($(notdir $<)_FLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

$(CWASMOBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(WASM_CC) -o $@ -c $(WASM_CFLAGS) $($(notdir $<)_FLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

WINEWRAP = $(if $(and $(filter %.exe,$1),$(WINE)),$(WINE) $1,$1)

ifdef WINE
relativize = $(if $(filter /%,$1),$(DEPTH)$(subst $(space),,$(foreach d,$(subst /, ,$(topobjdir)),/..))$1,$1)
else
relativize = $1
endif

ifdef WINE
$(ASOBJS) $(SOBJS): export WINEPATH=$(subst :,;,$(PATH))
endif

ifdef ASFILES
$(ASOBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(call WINEWRAP,$(AS)) $(ASOUTOPTION)$@ $(ASFLAGS) $($(notdir $<)_FLAGS) $(AS_DASH_C_FLAG) $(call relativize,$<)
	$(call BUILDSTATUS,END_Object $@)
endif

ifneq (,$(RUST_LIBRARY_FILE)$(HOST_RUST_LIBRARY_FILE)$(RUST_PROGRAMS)$(HOST_RUST_PROGRAMS))
include $(MOZILLA_DIR)/config/makefiles/rust.mk
endif

$(SOBJS):
	$(REPORT_BUILD)
	$(call WINEWRAP,$(AS)) $(ASOUTOPTION)$@ $(SFLAGS) $($(notdir $<)_FLAGS) -c $(call relativize,$<)

$(CPPOBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(CCC) $(OUTOPTION)$@ -c $(COMPILE_CXXFLAGS) $($(notdir $<)_FLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

$(CPPWASMOBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(WASM_CXX) -o $@ -c $(WASM_CXXFLAGS) $($(notdir $<)_FLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

$(CMMOBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(CCC) -o $@ -c $(COMPILE_CXXFLAGS) $(COMPILE_CMMFLAGS) $($(notdir $<)_FLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

$(CMOBJS):
	$(REPORT_BUILD_VERBOSE)
	$(call BUILDSTATUS,OBJECT_FILE $@)
	$(CC) -o $@ -c $(COMPILE_CFLAGS) $(COMPILE_CMFLAGS) $($(notdir $<)_FLAGS) $<
	$(call BUILDSTATUS,END_Object $@)

$(filter %.s,$(CPPSRCS:%.cpp=%.s)): %.s: %.cpp $(call mkdir_deps,$(MDDEPDIR))
	$(REPORT_BUILD_VERBOSE)
	$(CCC) -S $(COMPILE_CXXFLAGS) $($(notdir $<)_FLAGS) $<

$(filter %.s,$(CPPSRCS:%.cc=%.s)): %.s: %.cc $(call mkdir_deps,$(MDDEPDIR))
	$(REPORT_BUILD_VERBOSE)
	$(CCC) -S $(COMPILE_CXXFLAGS) $($(notdir $<)_FLAGS) $<

$(filter %.s,$(CPPSRCS:%.cxx=%.s)): %.s: %.cpp $(call mkdir_deps,$(MDDEPDIR))
	$(REPORT_BUILD_VERBOSE)
	$(CCC) -S $(COMPILE_CXXFLAGS) $($(notdir $<)_FLAGS) $<

$(filter %.s,$(CSRCS:%.c=%.s)): %.s: %.c $(call mkdir_deps,$(MDDEPDIR))
	$(REPORT_BUILD_VERBOSE)
	$(CC) -S $(COMPILE_CFLAGS) $($(notdir $<)_FLAGS) $<

ifneq (,$(filter %.i,$(MAKECMDGOALS)))
_group_srcs = $(sort $(patsubst %.$1,%.i,$(filter %.$1,$2 $(notdir $2))))

define PREPROCESS_RULES
_PREPROCESSED_$1_FILES := $$(call _group_srcs,$1,$$($2))
# Make preprocessed files PHONY so they are always executed, since they are
# manual targets and we don't necessarily write to $@.
.PHONY: $$(_PREPROCESSED_$1_FILES)

# Hack up VPATH so we can reach the sources. Eg: 'make Parser.i' may need to
# reach $(srcdir)/frontend/Parser.i
vpath %.$1 $$(addprefix $$(srcdir)/,$$(sort $$(dir $$($2))))
vpath %.$1 $$(addprefix $$(CURDIR)/,$$(sort $$(dir $$($2))))

$$(_PREPROCESSED_$1_FILES): _DEPEND_CFLAGS=
$$(_PREPROCESSED_$1_FILES): %.i: %.$1
	$$(REPORT_BUILD_VERBOSE)
	$$(addprefix $$(MKDIR) -p ,$$(filter-out .,$$(@D)))
	$$($3) -C $$(PREPROCESS_OPTION)$$@ $(foreach var,$4,$$($(var))) $$($$(notdir $$<)_FLAGS) $$<

endef

$(eval $(call PREPROCESS_RULES,cpp,CPPSRCS,CCC,COMPILE_CXXFLAGS))
$(eval $(call PREPROCESS_RULES,cc,CPPSRCS,CCC,COMPILE_CXXFLAGS))
$(eval $(call PREPROCESS_RULES,cxx,CPPSRCS,CCC,COMPILE_CXXFLAGS))
$(eval $(call PREPROCESS_RULES,c,CSRCS,CC,COMPILE_CFLAGS))
$(eval $(call PREPROCESS_RULES,mm,CMMSRCS,CCC,COMPILE_CXXFLAGS COMPILE_CMMFLAGS))

PP_UNIFIED ?= 1

ifndef PP_REINVOKE

MATCH_cpp = \(cpp\|cc|cxx\)
UPPER_c = C
UPPER_cpp = CPP
UPPER_mm = CMM

_get_all_sources = $1 $(if $(filter Unified%,$1),$(shell sed -n 's/\#include "\(.*\)"$$/\1/p' $(filter Unified%,$1)))
all_cpp_sources := $(call _get_all_sources,$(CPPSRCS))
all_mm_sources := $(call _get_all_sources,$(CMMSRCS))
all_c_sources := $(call _get_all_sources,$(CSRCS))
all_sources := $(all_cpp_sources) $(all_cmm_sources) $(all_c_sources)

%.i: FORCE
ifeq ($(PP_UNIFIED),1)
	@$(MAKE) PP_REINVOKE=1 \
	    $(or $(addsuffix .i, \
              $(foreach type,c cpp mm, \
	        $(if $(filter Unified%,$($(UPPER_$(type))SRCS)), \
	          $(shell grep -l '#include "\(.*/\)\?$(basename $@).$(or $(MATCH_$(type)),$(type))"' Unified*.$(type) | sed 's/\.$(type)$$//') \
            ))),$(error "File not found for preprocessing: $@"))
else
	@$(MAKE) PP_REINVOKE=1 $@ \
	    $(foreach type,c cpp mm,$(UPPER_$(type))SRCS="$(all_$(type)_sources)")
endif

endif

endif

%.res: $(or $(RCFILE),%.rc) $(MOZILLA_DIR)/config/create_res.py
	$(REPORT_BUILD)
	$(call BUILDSTATUS,START_Res $@)
	$(PYTHON3) $(MOZILLA_DIR)/config/create_res.py $(DEFINES) $(INCLUDES) -o $@ $<
	$(call BUILDSTATUS,END_Res $@)

$(notdir $(addsuffix .rc,$(PROGRAM) $(SHARED_LIBRARY) $(SIMPLE_PROGRAMS) module)): %.rc: $(RCINCLUDE) $(MOZILLA_DIR)/config/create_rc.py
	$(call BUILDSTATUS,START_Rc $@)
	$(PYTHON3) $(MOZILLA_DIR)/config/create_rc.py '$(if $(filter module,$*),,$*)' --include '$(RCINCLUDE)' --dep-file '$(MDDEPDIR)/$@.d'
	$(call BUILDSTATUS,END_Rc $@)

-include $(addsuffix .rc.d, $(addprefix $(MDDEPDIR)/,$(PROGRAM) $(SHARED_LIBRARY) $(SIMPLE_PROGRAMS) module))

MAKEFLAGS += -r

ifneq (,$(filter WINNT,$(OS_ARCH)))
SEP := ;
else
SEP := :
endif

EMPTY :=
SPACE := $(EMPTY) $(EMPTY)


ifneq ($(XPI_NAME),)
$(FINAL_TARGET):
	$(NSINSTALL) -D $@

export:: $(FINAL_TARGET)
endif

PREF_DIR = defaults/pref

ifneq (,$(DIST_SUBDIR)$(XPI_NAME))
PREF_DIR = defaults/preferences
endif


chrome::
	$(MAKE) realchrome
	$(LOOP_OVER_DIRS)

$(FINAL_TARGET)/chrome: $(call mkdir_deps,$(FINAL_TARGET)/chrome)

ifneq (,$(JAR_MANIFEST))
ifndef NO_DIST_INSTALL

ifdef XPI_NAME
ifdef XPI_ROOT_APPID
MAKE_JARS_FLAGS += --root-manifest-entry-appid='$(XPI_ROOT_APPID)'
endif

ifneq (,$(DIST_SUBDIR))
ifndef XPI_ROOT_APPID
$(error XPI_ROOT_APPID is not defined - langpacks will break.)
endif
endif
endif

misc realchrome:: $(FINAL_TARGET)/chrome
	$(call py_action,jar_maker $(subst $(topsrcdir)/,,$(JAR_MANIFEST)),\
	  $(QUIET) -d $(FINAL_TARGET) \
	  $(MAKE_JARS_FLAGS) $(DEFINES) $(ACDEFINES) \
	  $(JAR_MANIFEST))

ifdef AB_CD
.PHONY: l10n
l10n: misc ;
endif
endif

endif

define xpi_package_rule
$$(abspath $(3))/$(1).xpi: $$(call mkdir_deps,$(3) $$(MDDEPDIR))
	@echo 'Packaging $(1).xpi...'
	$$(call py_action,zip $(1).xpi,-C $(2) $$@ '*' --dep-target $$@ --dep-file $$(MDDEPDIR)/$(1).d)

$(4):: $$(abspath $(3))/$(1).xpi

-include $$(MDDEPDIR)/$(1).d

endef

ifdef XPI_TESTDIR
$(eval $(call xpi_package_rule,$(XPI_PKGNAME),$(srcdir),$(XPI_TESTDIR),misc))
else
ifdef XPI_PKGNAME
$(eval $(call xpi_package_rule,$(XPI_PKGNAME),$(FINAL_TARGET),$(FINAL_TARGET)/..,tools realchrome))
endif
endif


_MDDEPEND_FILES :=

ifneq (,$(filter target-objects target all default,$(MAKECMDGOALS)))
_MDDEPEND_FILES += $(addsuffix .pp,$(notdir $(sort $(OBJS) $(PROGOBJS))))
endif

ifneq (,$(filter host-objects host all default,$(MAKECMDGOALS)))
_MDDEPEND_FILES += $(addsuffix .pp,$(notdir $(sort $(HOST_OBJS) $(HOST_PROGOBJS))))
endif

MDDEPEND_FILES := $(strip $(wildcard $(addprefix $(MDDEPDIR)/,$(_MDDEPEND_FILES))))
MDDEPEND_FILES += $(EXTRA_MDDEPEND_FILES)

ifneq (,$(MDDEPEND_FILES))
-include $(MDDEPEND_FILES)
endif


ifneq (,$(filter $(DEPTH)/config/nsinstall$(HOST_BIN_SUFFIX),$(install_cmd)))
ifeq (,$(wildcard $(DEPTH)/config/nsinstall$(HOST_BIN_SUFFIX)))
nsinstall_is_usable = $(if $(wildcard $(DEPTH)/config/nsinstall$(HOST_BIN_SUFFIX)),yes)

define install_cmd_override
$(1): install_cmd = $$(if $$(nsinstall_is_usable),$$(INSTALL),$$(NSINSTALL_PY) -t) $$(1)
endef
endif
endif

install_target_tier = $(or $($(1)_TARGET),libs)
INSTALL_TARGETS_TIERS := $(sort $(foreach category,$(INSTALL_TARGETS),$(call install_target_tier,$(category))))

install_target_result = $($(1)_DEST:%/=%)/$(if $($(1)_KEEP_PATH),$(2),$(notdir $(2)))
install_target_files = $(foreach file,$($(1)_FILES),$(call install_target_result,$(category),$(file)))
install_target_executables = $(foreach file,$($(1)_EXECUTABLES),$(call install_target_result,$(category),$(file)))

define create_dependency
$(1): $(2)
$(1): $(2)
endef

define install_target_template
$(call install_cmd_override,$(2))
$(call create_dependency,$(2),$(1))
endef

$(foreach category,$(INSTALL_TARGETS),\
  $(if $($(category)_DEST),,$(error Missing $(category)_DEST)) \
  $(foreach tier,$(call install_target_tier,$(category)),\
    $(eval INSTALL_TARGETS_FILES_$(tier) += $(call install_target_files,$(category))) \
    $(eval INSTALL_TARGETS_EXECUTABLES_$(tier) += $(call install_target_executables,$(category))) \
  ) \
  $(foreach file,$($(category)_FILES) $($(category)_EXECUTABLES), \
    $(eval $(call install_target_template,$(file),$(call install_target_result,$(category),$(file)))) \
  ) \
)

$(foreach tier,$(INSTALL_TARGETS_TIERS), \
  $(eval $(if $(filter .,$(DEPTH)),recurse_$(tier):,$(tier)::) $(INSTALL_TARGETS_FILES_$(tier)) $(INSTALL_TARGETS_EXECUTABLES_$(tier))) \
)

install_targets_sanity = $(if $(filter-out $(notdir $@),$(notdir $(<))),$(error Looks like $@ has an unexpected dependency on $< which breaks INSTALL_TARGETS))

$(sort $(foreach tier,$(INSTALL_TARGETS_TIERS),$(INSTALL_TARGETS_FILES_$(tier)))):
	$(install_targets_sanity)
	$(call BUILDSTATUS,START_Install $@)
	$(call install_cmd,$(IFLAGS1) '$<' '$(@D)')
	$(call BUILDSTATUS,END_Install $@)

$(sort $(foreach tier,$(INSTALL_TARGETS_TIERS),$(INSTALL_TARGETS_EXECUTABLES_$(tier)))):
	$(install_targets_sanity)
	$(call BUILDSTATUS,START_Install $@)
	$(call install_cmd,$(IFLAGS2) '$<' '$(@D)')
	$(call BUILDSTATUS,END_Install $@)

################################################################################

pp_target_tier = $(or $($(1)_TARGET),libs)
PP_TARGETS_TIERS := $(sort $(foreach category,$(PP_TARGETS),$(call pp_target_tier,$(category))))

pp_target_result = $(or $($(1)_PATH:%/=%),$(CURDIR))/$(if $($(1)_KEEP_PATH),$(2:.in=),$(notdir $(2:.in=)))
pp_target_results = $(foreach file,$($(1)),$(call pp_target_result,$(category),$(file)))

$(foreach category,$(PP_TARGETS), \
  $(foreach tier,$(call pp_target_tier,$(category)), \
    $(eval PP_TARGETS_RESULTS_$(tier) += $(call pp_target_results,$(category))) \
  ) \
  $(foreach file,$($(category)), \
    $(eval $(call create_dependency,$(call pp_target_result,$(category),$(file)), \
                                    $(file) $(GLOBAL_DEPS) $($(category)_EXTRA_DEPS))) \
  ) \
  $(eval $(call pp_target_results,$(category)): PP_TARGET_FLAGS=$($(category)_FLAGS)) \
)

$(foreach tier,$(PP_TARGETS_TIERS), \
  $(eval $(if $(filter .,$(DEPTH)),recurse_$(tier):,$(tier)::) $(PP_TARGETS_RESULTS_$(tier))) \
)

PP_TARGETS_ALL_RESULTS := $(sort $(foreach tier,$(PP_TARGETS_TIERS),$(PP_TARGETS_RESULTS_$(tier))))
$(PP_TARGETS_ALL_RESULTS):
	$(if $(filter-out $(notdir $@),$(notdir $(<:.in=))),$(error Looks like $@ has an unexpected dependency on $< which breaks PP_TARGETS))
	$(RM) '$@'
	$(call py_action,preprocessor $(@F),--depend $(MDDEPDIR)/$(@F).pp $(PP_TARGET_FLAGS) $(DEFINES) $(ACDEFINES) '$<' -o '$@')

$(filter %.css,$(PP_TARGETS_ALL_RESULTS)): PP_TARGET_FLAGS+=--marker %

PP_TARGETS_ALL_RESULT_NAMES := $(notdir $(PP_TARGETS_ALL_RESULTS))
$(foreach file,$(sort $(PP_TARGETS_ALL_RESULT_NAMES)), \
  $(if $(filter-out 1,$(words $(filter $(file),$(PP_TARGETS_ALL_RESULT_NAMES)))), \
    $(error Multiple preprocessing rules are creating a $(file) file) \
  ) \
)

ifneq (,$(filter $(PP_TARGETS_TIERS) $(PP_TARGETS_ALL_RESULTS),$(MAKECMDGOALS)))
# If the depfile for a preprocessed file doesn't exist, add a dep to force
$(foreach file,$(PP_TARGETS_ALL_RESULTS), \
  $(if $(wildcard $(MDDEPDIR)/$(notdir $(file)).pp), \
    , \
    $(eval $(file): FORCE) \
  ) \
)

MDDEPEND_FILES := $(strip $(wildcard $(addprefix $(MDDEPDIR)/,$(addsuffix .pp,$(notdir $(PP_TARGETS_ALL_RESULTS))))))

ifneq (,$(MDDEPEND_FILES))
-include $(MDDEPEND_FILES)
endif

endif

ifndef TOPLEVEL_BUILD
include $(MOZILLA_DIR)/config/makefiles/nonrecursive.mk
endif



.SUFFIXES:

.PHONY: all alltags boot chrome realchrome export install libs makefiles run_apprunner tools $(DIRS) FORCE

FORCE:

.DELETE_ON_ERROR:

tags: TAGS

TAGS: $(CSRCS) $(CPPSRCS) $(wildcard *.h)
	-etags $(CSRCS) $(CPPSRCS) $(wildcard *.h)
	$(LOOP_OVER_DIRS)

ifndef INCLUDED_DEBUGMAKE_MK 
  ifneq (,$(call isTargetStem,echo,show))
    include $(MOZILLA_DIR)/config/makefiles/debugmake.mk
  endif 
endif 

FREEZE_VARIABLES = \
  CSRCS \
  CPPSRCS \
  EXPORTS \
  DIRS \
  LIBRARY \
  MODULE \
  $(NULL)

$(foreach var,$(FREEZE_VARIABLES),$(eval $(var)_FROZEN := '$($(var))'))

CHECK_FROZEN_VARIABLES = $(foreach var,$(FREEZE_VARIABLES), \
  $(if $(subst $($(var)_FROZEN),,'$($(var))'),$(error Makefile variable '$(var)' changed value after including rules.mk. Was $($(var)_FROZEN), now $($(var)).)))

libs export::
	$(CHECK_FROZEN_VARIABLES)

.DEFAULT_GOAL := $(or $(OVERRIDE_DEFAULT_GOAL),default)


include $(MOZILLA_DIR)/config/makefiles/autotargets.mk
ifneq ($(NULL),$(AUTO_DEPS))
  default all libs tools export:: $(AUTO_DEPS)
endif
