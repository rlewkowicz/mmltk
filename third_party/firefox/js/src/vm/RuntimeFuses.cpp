/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/RuntimeFuses.h"

#include <stddef.h>
#include <stdint.h>

#include "builtin/String.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"

using namespace js;

int32_t js::RuntimeFuses::fuseOffsets[uint8_t(
    RuntimeFuses::FuseIndex::LastFuseIndex)] = {
#define FUSE(Name, LowerName) offsetof(RuntimeFuses, LowerName),
    FOR_EACH_RUNTIME_FUSE(FUSE)
#undef FUSE
};

int32_t js::RuntimeFuses::offsetOfFuseWordRelativeToRuntime(
    RuntimeFuses::FuseIndex index) {
  int32_t base_offset = offsetof(JSRuntime, runtimeFuses);
  int32_t fuse_offset = RuntimeFuses::fuseOffsets[uint8_t(index)];
  int32_t fuseWordOffset = GuardFuse::fuseOffset();

  return base_offset + fuse_offset + fuseWordOffset;
}

const char* js::RuntimeFuses::fuseNames[] = {
#define FUSE(Name, LowerName) #LowerName,
    FOR_EACH_RUNTIME_FUSE(FUSE)
#undef FUSE
};

const char* js::RuntimeFuses::getFuseName(RuntimeFuses::FuseIndex index) {
  uint8_t rawIndex = uint8_t(index);
  MOZ_ASSERT(index < RuntimeFuses::FuseIndex::LastFuseIndex);
  return fuseNames[rawIndex];
}

void js::HasSeenObjectEmulateUndefinedFuse::popFuse(JSContext* cx) {
  js::InvalidatingRuntimeFuse::popFuse(cx);
}

void js::HasSeenArrayExceedsInt32LengthFuse::popFuse(JSContext* cx) {
  js::InvalidatingRuntimeFuse::popFuse(cx);
}

bool js::DefaultLocaleHasDefaultCaseMappingFuse::checkInvariant(JSContext* cx) {
#if JS_HAS_INTL_API
  auto locale = cx->runtime()->getDefaultLocaleIfInitialized();
  return LocaleHasDefaultCaseMapping(locale);
#else
  return true;
#endif
}

void js::DefaultLocaleHasDefaultCaseMappingFuse::popFuse(JSContext* cx) {
  js::InvalidatingRuntimeFuse::popFuse(cx);
}
