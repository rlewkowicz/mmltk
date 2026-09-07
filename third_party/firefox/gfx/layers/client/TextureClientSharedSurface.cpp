/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TextureClientSharedSurface.h"

#include "GLContext.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Logging.h"  // for gfxDebug
#include "mozilla/layers/ISurfaceAllocator.h"
#include "nsThreadUtils.h"
#include "SharedSurface.h"


using namespace mozilla::gl;

namespace mozilla {
namespace layers {

already_AddRefed<TextureClient> SharedSurfaceTextureData::CreateTextureClient(
    const layers::SurfaceDescriptor& aDesc, const gfx::SurfaceFormat aFormat,
    gfx::IntSize aSize, TextureFlags aFlags, LayersIPCChannel* aAllocator) {
  auto data = MakeUnique<SharedSurfaceTextureData>(aDesc, aFormat, aSize);
  return TextureClient::CreateWithData(data.release(), aFlags, aAllocator);
}

SharedSurfaceTextureData::SharedSurfaceTextureData(
    const SurfaceDescriptor& desc, const gfx::SurfaceFormat format,
    const gfx::IntSize size)
    : mDesc(desc), mFormat(format), mSize(size) {}

SharedSurfaceTextureData::~SharedSurfaceTextureData() = default;

void SharedSurfaceTextureData::Deallocate(LayersIPCChannel*) {}

void SharedSurfaceTextureData::FillInfo(TextureData::Info& aInfo) const {
  aInfo.size = mSize;
  aInfo.format = mFormat;
  aInfo.hasSynchronization = false;
  aInfo.supportsMoz2D = false;
  aInfo.canExposeMappedData = false;
}

bool SharedSurfaceTextureData::Serialize(SurfaceDescriptor& aOutDescriptor) {
  aOutDescriptor = mDesc;
  return true;
}

TextureFlags SharedSurfaceTextureData::GetTextureFlags() const {
  TextureFlags flags = TextureFlags::NO_FLAGS;
  return flags;
}

Maybe<uint64_t> SharedSurfaceTextureData::GetBufferId() const {
  return Nothing();
}

UniqueFileHandle SharedSurfaceTextureData::GetAcquireFence() {
  return UniqueFileHandle();
}

}  
}  
