# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

ifndef INCLUDED_AUTOTARGETS_MK 



MKDIR ?= mkdir -p
TOUCH ?= touch

space = $(NULL) $(NULL)

.PRECIOUS: %/.mkdir.done


getPathPrefix =$(if $(filter /%,$(1)),/)

_slashSqueeze =$(foreach val,$(getargv),$(call getPathPrefix,$(val))$(subst $(space),/,$(strip $(subst /,$(space),$(val)))))

slash_strip = \
  $(strip \
    $(subst <--[**]-->,$(space),\
	$(call _slashSqueeze,\
    $(subst $(space),<--[**]-->,$(1))\
  )))

mkdir_stem =$(foreach val,$(getargv),$(subst /.mkdir.done,$(NULL),$(val)))

mkdir_deps =$(foreach dir,$(getargv),$(call slash_strip,$(dir)/.mkdir.done))


%/.mkdir.done: 
	$(subst $(space)-p,$(null),$(MKDIR)) -p '$(dir $@)'
	@$(TOUCH) -t 198001030000 '$@'

.mkdir.done:
	@echo 'WARNING: $(MKDIR) -dot- requested by $(MAKE) -C $(CURDIR) $(MAKECMDGOALS)'
	@$(TOUCH) -t 198001030000 '$@'

INCLUDED_AUTOTARGETS_MK = 1
endif 


ifneq (,$(GENERATED_DIRS))
  GENERATED_DIRS := $(strip $(sort $(GENERATED_DIRS)))
  tmpauto :=$(call mkdir_deps,GENERATED_DIRS)
  GENERATED_DIRS_DEPS +=$(tmpauto)
endif


AUTO_DEPS +=$(GENERATED_DIRS_DEPS)
AUTO_DEPS := $(strip $(sort $(AUTO_DEPS)))

$(call requiredfunction,getargv)
