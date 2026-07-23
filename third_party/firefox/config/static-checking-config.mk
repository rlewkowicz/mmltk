# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


ifdef ENABLE_CLANG_PLUGIN
ifeq ($(OS_ARCH),WINNT)
CC := $(subst clang-cl.exe,clang.exe --driver-mode=cl,$(CC:.EXE=.exe))
CXX := $(subst clang-cl.exe,clang.exe --driver-mode=cl,$(CXX:.EXE=.exe))
$(notdir $(addsuffix .$(WASM_OBJ_SUFFIX),$(basename $(WASM_CPPSRCS)))): WASM_CXX := $(subst clang++.exe,clang.exe --driver-mode=g++,$(WASM_CXX:.EXE=.exe))
endif
endif
