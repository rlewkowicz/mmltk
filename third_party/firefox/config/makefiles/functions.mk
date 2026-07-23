# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


ifdef INCLUDED_FUNCTIONS_MK
$(error Do not include functions.mk twice!)
endif
INCLUDED_FUNCTIONS_MK = 1

core_abspath = $(error core_abspath is unsupported, use $$(abspath) instead)
core_realpath = $(error core_realpath is unsupported)

core_winabspath = $(error core_winabspath is unsupported)

# Run a named Python build action. The first argument is the name of the build
define py_action
$(call BUILDSTATUS,START_$(firstword $(1)) $(or $(word 2,$(1)),$(2)))
$(if $(3),cd $(3) && )$(4) $(PYTHON3) -m mozbuild.action.$(firstword $(1)) $(2)
$(call BUILDSTATUS,END_$(firstword $(1)) $(or $(word 2,$(1)),$(2)))

endef
