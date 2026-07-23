# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

AB_rCD = $(if $(filter en-US,$(AB_CD)),,-$(if $(filter he, $(AB_CD)),iw,$(if $(filter id, $(AB_CD)),in,$(subst -,-r,$(AB_CD)))))
