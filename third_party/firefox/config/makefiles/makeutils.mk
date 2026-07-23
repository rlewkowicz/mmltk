# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

istype =$(if $(value ${1}),list,scalar)
isval  =$(if $(filter-out list,$(call istype,${1})),true)
isvar  =$(if $(filter-out scalar,$(call istype,${1})),true)

argv  =$(strip
argv +=$(if $(1), $(1))$(if $(2), $(2))$(if $(3), $(3))$(if $(4), $(4))
argv +=$(if $(5), $(5))$(if $(6), $(6))$(if $(7), $(7))$(if $(8), $(8))
argv +=$(if $(9), $(9))
argv +=$(if $(10), $(error makeutils.mk::argv can only handle 9 arguments))
argv +=)

getargv = $(if $(call isvar,$(1)),$($(1)),$(argv))

subargv =$(wordlist $(1),$(words $(getargv)),$(getargv))


banner = \
$(info ) \
$(info ***************************************************************************) \
$(info ** $(getargv)) \
$(info ***************************************************************************) \
$(NULL)

is_XinY =$(filter $(1),$(call subargv,3,$(getargv)))

ifdef MAKEUTILS_UNIT_TEST
  mcg_goals=TEST_MAKECMDGOALS
else
  mcg_goals=MAKECMDGOALS
endif

isTargetStem       = $(sort \
  $(foreach var,$(getargv),\
    $(foreach pat,$(var)% %$(var),\
      $(call is_XinY,$(pat),${$(mcg_goals)})\
  )))
isTargetStemClean  = $(call isTargetStem,clean)
isTargetStemExport = $(call isTargetStem,export)
isTargetStemLibs   = $(call isTargetStem,libs)
isTargetStemTools  = $(call isTargetStem,tools)


errorifneq =$(if $(subst $(strip $(1)),$(NULL),$(strip $(2))),$(error expected [$(1)] but found [$(2)]))

requiredfunction =$(foreach func,$(1) $(2) $(3) $(4) $(5) $(6) $(7) $(8) $(9),$(if $(value $(func)),$(NULL),$(error required function [$(func)] is unavailable)))



map = $(foreach val,$(2),$(call $(1),$(val)))


ifeq (,$(filter %clean clean%,$(MAKECMDGOALS))) 

checkIfEmpty =$(foreach var,$(wordlist 2,100,$(argv)),$(if $(strip $($(var))),$(NOP),$(call $(1),Variable $(var) does not contain a value)))

errorIfEmpty =$(call checkIfEmpty,error $(argv))
warnIfEmpty  =$(call checkIfEmpty,warning $(argv))

endif 

ifdef MOZILLA_DIR
topORerr = $(MOZILLA_DIR)
else
topORerr = $(if $(topsrcdir),$(topsrcdir),$(error topsrcdir is not defined))
endif

ifdef USE_AUTOTARGETS_MK 
  include $(topORerr)/config/makefiles/autotargets.mk
endif
