/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "2D.h"
#include "Swizzle.h"

#if defined(USE_CAIRO)
#  include "DrawTargetCairo.h"
#  include "PathCairo.h"
#  include "SourceSurfaceCairo.h"
#endif

#include "DrawTargetSkia.h"
#include "PathSkia.h"
#include "ScaledFontBase.h"



#if defined(MOZ_WIDGET_GTK)
#  include "ScaledFontFontconfig.h"
#  include "NativeFontResourceFreeType.h"
#  include "UnscaledFontFreeType.h"
#endif



#include "DrawTargetOffset.h"
#include "DrawTargetRecording.h"
#include "PathRecording.h"

#include "SourceSurfaceRawData.h"

#include "mozilla/CheckedInt.h"

#if defined(MOZ_ENABLE_FREETYPE)
#  include "ft2build.h"
#  include FT_FREETYPE_H
#endif
#include "mozilla/StaticPrefs_gfx.h"

#if defined(MOZ_LOGGING)
GFX2D_API mozilla::LogModule* GetGFX2DLog() {
  static mozilla::LazyLogModule sLog("gfx2d");
  return sLog;
}
#endif

#if defined(MOZ_ENABLE_FREETYPE)
extern "C" {

void mozilla_AddRefSharedFTFace(void* aContext) {
  if (aContext) {
    static_cast<mozilla::gfx::SharedFTFace*>(aContext)->AddRef();
  }
}

void mozilla_ReleaseSharedFTFace(void* aContext, void* aOwner) {
  if (aContext) {
    auto* sharedFace = static_cast<mozilla::gfx::SharedFTFace*>(aContext);
    sharedFace->ForgetLockOwner(aOwner);
    sharedFace->Release();
  }
}

void mozilla_ForgetSharedFTFaceLockOwner(void* aContext, void* aOwner) {
  static_cast<mozilla::gfx::SharedFTFace*>(aContext)->ForgetLockOwner(aOwner);
}

int mozilla_LockSharedFTFace(void* aContext,
                             void* aOwner) MOZ_NO_THREAD_SAFETY_ANALYSIS {
  return int(static_cast<mozilla::gfx::SharedFTFace*>(aContext)->Lock(aOwner));
}

void mozilla_UnlockSharedFTFace(void* aContext) MOZ_NO_THREAD_SAFETY_ANALYSIS {
  static_cast<mozilla::gfx::SharedFTFace*>(aContext)->Unlock();
}

FT_Error mozilla_LoadFTGlyph(FT_Face aFace, uint32_t aGlyphIndex,
                             int32_t aFlags) {
  return mozilla::gfx::Factory::LoadFTGlyph(aFace, aGlyphIndex, aFlags);
}

void mozilla_LockFTLibrary(FT_Library aFTLibrary) {
  mozilla::gfx::Factory::LockFTLibrary(aFTLibrary);
}

void mozilla_UnlockFTLibrary(FT_Library aFTLibrary) {
  mozilla::gfx::Factory::UnlockFTLibrary(aFTLibrary);
}
}
#endif

namespace mozilla::gfx {

#if defined(MOZ_ENABLE_FREETYPE)
FT_Library Factory::mFTLibrary = nullptr;
StaticMutex Factory::mFTLock;

already_AddRefed<SharedFTFace> FTUserFontData::CloneFace(int aFaceIndex) {
  if (mFontData) {
    RefPtr<SharedFTFace> face = Factory::NewSharedFTFaceFromData(
        nullptr, mFontData, mLength, aFaceIndex, this);
    if (!face ||
        (FT_Select_Charmap(face->GetFace(), FT_ENCODING_UNICODE) != FT_Err_Ok &&
         FT_Select_Charmap(face->GetFace(), FT_ENCODING_MS_SYMBOL) !=
             FT_Err_Ok)) {
      return nullptr;
    }
    return face.forget();
  }
  FT_Face face = Factory::NewFTFace(nullptr, mFilename.c_str(), aFaceIndex);
  if (face) {
    return MakeAndAddRef<SharedFTFace>(face, this);
  }
  return nullptr;
}
#endif


SubpixelOrder Factory::mSubpixelOrder = SubpixelOrder::UNKNOWN;

mozilla::gfx::Config* Factory::sConfig = nullptr;

void Factory::Init(const Config& aConfig) {
  MOZ_ASSERT(!sConfig);
  sConfig = new Config(aConfig);

  NativeFontResource::RegisterMemoryReporter();

  SourceSurfaceAlignedRawData::RegisterMemoryReporter();
}

void Factory::ShutDown() {
  if (sConfig) {
    delete sConfig->mLogForwarder;
    delete sConfig;
    sConfig = nullptr;
  }

#if defined(MOZ_ENABLE_FREETYPE)
  mFTLibrary = nullptr;
#endif
}

inline int LoggerOptionsBasedOnSize(const IntSize& aSize) {
  return CriticalLog::DefaultOptions(Factory::ReasonableSurfaceSize(aSize));
}

bool Factory::ReasonableSurfaceSize(const IntSize& aSize) {
  return Factory::CheckSurfaceSize(aSize, kReasonableSurfaceSize);
}

bool Factory::AllowedSurfaceSize(const IntSize& aSize) {
  if (sConfig) {
    return Factory::CheckSurfaceSize(aSize, sConfig->mMaxTextureSize,
                                     sConfig->mMaxAllocSize);
  }

  return CheckSurfaceSize(aSize);
}

bool Factory::CheckSurfaceSize(const IntSize& sz, int32_t extentLimit,
                               int32_t allocLimit) {
  if (sz.width <= 0 || sz.height <= 0) {
    return false;
  }

  if (extentLimit && (sz.width > extentLimit || sz.height > extentLimit)) {
    gfxDebug() << "Surface size too large (exceeds extent limit)!";
    return false;
  }

  auto stride = GetAlignedStride<16>(sz.width, 4);
  if (stride.isNothing()) {
    gfxDebug() << "Surface size too large (stride is invalid)!";
    return false;
  }

  CheckedInt<int32_t> numBytes =
      CheckedInt<int32_t>(stride.value()) * sz.height;
  if (!numBytes.isValid()) {
    gfxDebug()
        << "Surface size too large (allocation size would overflow int32_t)!";
    return false;
  }

  if (allocLimit && allocLimit < numBytes.value()) {
    gfxDebug() << "Surface size too large (exceeds allocation limit)!";
    return false;
  }

  return true;
}

already_AddRefed<DrawTarget> Factory::CreateDrawTarget(BackendType aBackend,
                                                       const IntSize& aSize,
                                                       SurfaceFormat aFormat) {
  if (!AllowedSurfaceSize(aSize)) {
    gfxCriticalError(LoggerOptionsBasedOnSize(aSize))
        << "Failed to allocate a surface due to invalid size (CDT) " << aSize;
    return nullptr;
  }

  RefPtr<DrawTarget> retVal;
  switch (aBackend) {
    case BackendType::SKIA: {
      RefPtr<DrawTargetSkia> newTarget;
      newTarget = new DrawTargetSkia();
      if (newTarget->Init(aSize, aFormat)) {
        retVal = newTarget;
      }
      break;
    }
#if defined(USE_CAIRO)
    case BackendType::CAIRO: {
      RefPtr<DrawTargetCairo> newTarget;
      newTarget = new DrawTargetCairo();
      if (newTarget->Init(aSize, aFormat)) {
        retVal = newTarget;
      }
      break;
    }
#endif
    default:
      return nullptr;
  }

  if (!retVal) {
    gfxCriticalError(LoggerOptionsBasedOnSize(aSize))
        << "Failed to create DrawTarget, Type: " << int(aBackend)
        << " Size: " << aSize;
  }

  return retVal.forget();
}

already_AddRefed<PathBuilder> Factory::CreatePathBuilder(BackendType aBackend,
                                                         FillRule aFillRule) {
  switch (aBackend) {
    case BackendType::SKIA:
    case BackendType::WEBGL:
      return PathBuilderSkia::Create(aFillRule);
#if defined(USE_CAIRO)
    case BackendType::CAIRO:
      return PathBuilderCairo::Create(aFillRule);
#endif
    case BackendType::RECORDING:
      return do_AddRef(new PathBuilderRecording(BackendType::SKIA, aFillRule));
    default:
      gfxCriticalNote << "Invalid PathBuilder type specified: "
                      << (int)aBackend;
      return nullptr;
  }
}

already_AddRefed<PathBuilder> Factory::CreateSimplePathBuilder() {
  return CreatePathBuilder(BackendType::SKIA);
}

already_AddRefed<DrawTarget> Factory::CreateRecordingDrawTarget(
    DrawEventRecorder* aRecorder, DrawTarget* aDT, IntRect aRect) {
  return MakeAndAddRef<DrawTargetRecording>(aRecorder, aDT, aRect);
}

already_AddRefed<DrawTarget> Factory::CreateDrawTargetForData(
    BackendType aBackend, unsigned char* aData, const IntSize& aSize,
    int32_t aStride, SurfaceFormat aFormat, bool aUninitialized,
    bool aIsClear) {
  MOZ_ASSERT(aData);
  if (!AllowedSurfaceSize(aSize)) {
    gfxCriticalError(LoggerOptionsBasedOnSize(aSize))
        << "Failed to allocate a surface due to invalid size (DTD) " << aSize;
    return nullptr;
  }

  RefPtr<DrawTarget> retVal;

  switch (aBackend) {
    case BackendType::SKIA: {
      RefPtr<DrawTargetSkia> newTarget;
      newTarget = new DrawTargetSkia();
      if (newTarget->Init(aData, aSize, aStride, aFormat, aUninitialized,
                          aIsClear)) {
        retVal = newTarget;
      }
      break;
    }
#if defined(USE_CAIRO)
    case BackendType::CAIRO: {
      RefPtr<DrawTargetCairo> newTarget;
      newTarget = new DrawTargetCairo();
      if (newTarget->Init(aData, aSize, aStride, aFormat)) {
        retVal = std::move(newTarget);
      }
      break;
    }
#endif
    default:
      gfxCriticalNote << "Invalid draw target type specified: "
                      << (int)aBackend;
      return nullptr;
  }

  if (!retVal) {
    gfxCriticalNote << "Failed to create DrawTarget, Type: " << int(aBackend)
                    << " Size: " << aSize << ", Data: " << hexa((void*)aData)
                    << ", Stride: " << aStride;
  }

  return retVal.forget();
}

already_AddRefed<DrawTarget> Factory::CreateOffsetDrawTarget(
    DrawTarget* aDrawTarget, IntPoint aTileOrigin) {
  RefPtr dt = MakeRefPtr<DrawTargetOffset>();

  if (!dt->Init(aDrawTarget, aTileOrigin)) {
    return nullptr;
  }

  return dt.forget();
}

bool Factory::DoesBackendSupportDataDrawtarget(BackendType aType) {
  switch (aType) {
    case BackendType::RECORDING:
    case BackendType::NONE:
    case BackendType::BACKEND_LAST:
    case BackendType::WEBRENDER_TEXT:
    case BackendType::WEBGL:
      return false;
    case BackendType::CAIRO:
    case BackendType::SKIA:
      return true;
  }

  return false;
}

size_t Factory::GetMaxSurfaceSize(BackendType aType) {
  switch (aType) {
    case BackendType::CAIRO:
      return DrawTargetCairo::GetMaxSurfaceSize();
    case BackendType::SKIA:
      return DrawTargetSkia::GetMaxSurfaceSize();
    default:
      return 0;
  }
}

size_t Factory::GetMaxSurfaceArea(BackendType aType) {
  switch (aType) {
    case BackendType::CAIRO:
      return DrawTargetCairo::GetMaxSurfaceArea();
    case BackendType::SKIA:
      return DrawTargetSkia::GetMaxSurfaceArea();
    default:
      return 0;
  }
}

already_AddRefed<NativeFontResource> Factory::CreateNativeFontResource(
    const uint8_t* aData, uint32_t aSize, FontType aFontType,
    void* aFontContext) {
  switch (aFontType) {
#if defined(MOZ_WIDGET_GTK)
    case FontType::FONTCONFIG:
      return NativeFontResourceFontconfig::Create(
          aData, aSize, static_cast<FT_Library>(aFontContext));
#endif
    default:
      gfxWarning()
          << "Unable to create requested font resource from truetype data";
      return nullptr;
  }
}

already_AddRefed<UnscaledFont> Factory::CreateUnscaledFontFromFontDescriptor(
    FontType aType, const uint8_t* aData, uint32_t aDataLength,
    uint32_t aIndex) {
  switch (aType) {
#if defined(MOZ_WIDGET_GTK)
    case FontType::FONTCONFIG:
      return UnscaledFontFontconfig::CreateFromFontDescriptor(
          aData, aDataLength, aIndex);
#endif
    default:
      gfxWarning() << "Invalid type specified for UnscaledFont font descriptor";
      return nullptr;
  }
}


#if defined(MOZ_WIDGET_GTK)
already_AddRefed<ScaledFont> Factory::CreateScaledFontForFontconfigFont(
    const RefPtr<UnscaledFont>& aUnscaledFont, Float aSize,
    RefPtr<SharedFTFace> aFace, FcPattern* aPattern) {
  return MakeAndAddRef<ScaledFontFontconfig>(std::move(aFace), aPattern,
                                             aUnscaledFont, aSize);
}
#endif


void Factory::SetSubpixelOrder(SubpixelOrder aOrder) {
  mSubpixelOrder = aOrder;
}

SubpixelOrder Factory::GetSubpixelOrder() { return mSubpixelOrder; }

#if defined(MOZ_ENABLE_FREETYPE)
SharedFTFace::SharedFTFace(FT_Face aFace, SharedFTFaceData* aData)
    : mFace(aFace),
      mData(aData),
      mLock("SharedFTFace::mLock"),
      mLastLockOwner(nullptr) {
  if (mData) {
    mData->BindData();
  }
}

SharedFTFace::~SharedFTFace() {
  Factory::ReleaseFTFace(mFace);
  if (mData) {
    mData->ReleaseData();
  }
}

void Factory::SetFTLibrary(FT_Library aFTLibrary) { mFTLibrary = aFTLibrary; }

FT_Library Factory::GetFTLibrary() {
  MOZ_ASSERT(mFTLibrary);
  return mFTLibrary;
}

FT_Library Factory::NewFTLibrary() {
  FT_Library library;
  if (FT_Init_FreeType(&library) != FT_Err_Ok) {
    return nullptr;
  }
  return library;
}

void Factory::ReleaseFTLibrary(FT_Library aFTLibrary) {
  FT_Done_FreeType(aFTLibrary);
}

void Factory::LockFTLibrary(FT_Library aFTLibrary)
    MOZ_CAPABILITY_ACQUIRE(mFTLock) MOZ_NO_THREAD_SAFETY_ANALYSIS {
  mFTLock.Lock();
}

void Factory::UnlockFTLibrary(FT_Library aFTLibrary)
    MOZ_CAPABILITY_RELEASE(mFTLock) MOZ_NO_THREAD_SAFETY_ANALYSIS {
  mFTLock.Unlock();
}

FT_Face Factory::NewFTFace(FT_Library aFTLibrary, const char* aFileName,
                           int aFaceIndex) {
  StaticMutexAutoLock lock(mFTLock);
  if (!aFTLibrary) {
    aFTLibrary = mFTLibrary;
  }
  FT_Face face;
  if (FT_New_Face(aFTLibrary, aFileName, aFaceIndex, &face) != FT_Err_Ok) {
    return nullptr;
  }
  return face;
}

already_AddRefed<SharedFTFace> Factory::NewSharedFTFace(FT_Library aFTLibrary,
                                                        const char* aFilename,
                                                        int aFaceIndex) {
  FT_Face face = NewFTFace(aFTLibrary, aFilename, aFaceIndex);
  if (!face) {
    return nullptr;
  }

  RefPtr<FTUserFontData> data;
  return MakeAndAddRef<SharedFTFace>(face, data);
}

FT_Face Factory::NewFTFaceFromData(FT_Library aFTLibrary, const uint8_t* aData,
                                   size_t aDataSize, int aFaceIndex) {
  StaticMutexAutoLock lock(mFTLock);
  if (!aFTLibrary) {
    aFTLibrary = mFTLibrary;
  }
  FT_Face face;
  if (FT_New_Memory_Face(aFTLibrary, aData, aDataSize, aFaceIndex, &face) !=
      FT_Err_Ok) {
    return nullptr;
  }
  return face;
}

already_AddRefed<SharedFTFace> Factory::NewSharedFTFaceFromData(
    FT_Library aFTLibrary, const uint8_t* aData, size_t aDataSize,
    int aFaceIndex, SharedFTFaceData* aSharedData) {
  if (FT_Face face =
          NewFTFaceFromData(aFTLibrary, aData, aDataSize, aFaceIndex)) {
    return MakeAndAddRef<SharedFTFace>(face, aSharedData);
  } else {
    return nullptr;
  }
}

void Factory::ReleaseFTFace(FT_Face aFace) {
  StaticMutexAutoLock lock(mFTLock);
  FT_Done_Face(aFace);
}

FT_Error Factory::LoadFTGlyph(FT_Face aFace, uint32_t aGlyphIndex,
                              int32_t aFlags) {
  StaticMutexAutoLock lock(mFTLock);
  return FT_Load_Glyph(aFace, aGlyphIndex, aFlags);
}
#endif


already_AddRefed<DrawTarget> Factory::CreateDrawTargetWithSkCanvas(
    SkCanvas* aCanvas) {
  RefPtr newTarget = MakeRefPtr<DrawTargetSkia>();
  if (!newTarget->Init(aCanvas)) {
    return nullptr;
  }
  return newTarget.forget();
}

void Factory::PurgeAllCaches() {}

already_AddRefed<DrawTarget> Factory::CreateDrawTargetForCairoSurface(
    cairo_surface_t* aSurface, const IntSize& aSize, SurfaceFormat* aFormat) {
  if (!AllowedSurfaceSize(aSize)) {
    gfxWarning() << "Allowing surface with invalid size (Cairo) " << aSize;
  }

  RefPtr<DrawTarget> retVal;

#if defined(USE_CAIRO)
  RefPtr newTarget = MakeRefPtr<DrawTargetCairo>();

  if (newTarget->Init(aSurface, aSize, aFormat)) {
    retVal = newTarget;
  }
#endif
  return retVal.forget();
}

already_AddRefed<SourceSurface> Factory::CreateSourceSurfaceForCairoSurface(
    cairo_surface_t* aSurface, const IntSize& aSize, SurfaceFormat aFormat) {
  if (aSize.width <= 0 || aSize.height <= 0) {
    gfxWarning() << "Can't create a SourceSurface without a valid size";
    return nullptr;
  }

#if defined(USE_CAIRO)
  return MakeAndAddRef<SourceSurfaceCairo>(aSurface, aSize, aFormat);
#else
  return nullptr;
#endif
}

already_AddRefed<DataSourceSurface> Factory::CreateWrappingDataSourceSurface(
    uint8_t* aData, int32_t aStride, const IntSize& aSize,
    SurfaceFormat aFormat,
    SourceSurfaceDeallocator aDeallocator ,
    void* aClosure ) {
  if (aSize.width <= 0 || aSize.height <= 0) {
    return nullptr;
  }
  if (!aDeallocator && aClosure) {
    return nullptr;
  }

  MOZ_ASSERT(aData);

  RefPtr newSurf = MakeRefPtr<SourceSurfaceRawData>();
  newSurf->InitWrappingData(aData, aSize, aStride, aFormat, aDeallocator,
                            aClosure);

  return newSurf.forget();
}

already_AddRefed<DataSourceSurface> Factory::CreateDataSourceSurface(
    const IntSize& aSize, SurfaceFormat aFormat, bool aZero) {
  if (!AllowedSurfaceSize(aSize)) {
    gfxCriticalError(LoggerOptionsBasedOnSize(aSize))
        << "Failed to allocate a surface due to invalid size (DSS) " << aSize;
    return nullptr;
  }

  bool clearSurface = aZero || aFormat == SurfaceFormat::B8G8R8X8;
  uint8_t clearValue = aFormat == SurfaceFormat::B8G8R8X8 ? 0xFF : 0;

  RefPtr newSurf = MakeRefPtr<SourceSurfaceAlignedRawData>();
  if (newSurf->Init(aSize, aFormat, clearSurface, clearValue)) {
    return newSurf.forget();
  }

  gfxWarning() << "CreateDataSourceSurface failed in init";
  return nullptr;
}

already_AddRefed<DataSourceSurface> Factory::CreateDataSourceSurfaceWithStride(
    const IntSize& aSize, SurfaceFormat aFormat, int32_t aStride, bool aZero) {
  if (!AllowedSurfaceSize(aSize) ||
      aStride < aSize.width * BytesPerPixel(aFormat)) {
    gfxCriticalError(LoggerOptionsBasedOnSize(aSize))
        << "CreateDataSourceSurfaceWithStride failed with bad stride "
        << aStride << ", " << aSize << ", " << aFormat;
    return nullptr;
  }

  bool clearSurface = aZero || aFormat == SurfaceFormat::B8G8R8X8;
  uint8_t clearValue = aFormat == SurfaceFormat::B8G8R8X8 ? 0xFF : 0;

  RefPtr newSurf = MakeRefPtr<SourceSurfaceAlignedRawData>();
  if (newSurf->Init(aSize, aFormat, clearSurface, clearValue, aStride)) {
    return newSurf.forget();
  }

  gfxCriticalError(LoggerOptionsBasedOnSize(aSize))
      << "CreateDataSourceSurfaceWithStride failed to initialize " << aSize
      << ", " << aFormat << ", " << aStride << ", " << aZero;
  return nullptr;
}

already_AddRefed<DataSourceSurface> Factory::CopyDataSourceSurface(
    DataSourceSurface* aSource) {
  MOZ_ASSERT(aSource->GetFormat() == SurfaceFormat::R8G8B8A8 ||
             aSource->GetFormat() == SurfaceFormat::R8G8B8X8 ||
             aSource->GetFormat() == SurfaceFormat::B8G8R8A8 ||
             aSource->GetFormat() == SurfaceFormat::B8G8R8X8 ||
             aSource->GetFormat() == SurfaceFormat::A8);

  DataSourceSurface::ScopedMap srcMap(aSource, DataSourceSurface::READ);
  if (NS_WARN_IF(!srcMap.IsMapped())) {
    MOZ_ASSERT_UNREACHABLE("CopyDataSourceSurface: Failed to map surface.");
    return nullptr;
  }

  IntSize size = aSource->GetSize();
  SurfaceFormat format = aSource->GetFormat();

  RefPtr<DataSourceSurface> dst = CreateDataSourceSurfaceWithStride(
      size, format, srcMap.GetStride(),  false);
  if (NS_WARN_IF(!dst)) {
    return nullptr;
  }

  DataSourceSurface::ScopedMap dstMap(dst, DataSourceSurface::WRITE);
  if (NS_WARN_IF(!dstMap.IsMapped())) {
    MOZ_ASSERT_UNREACHABLE("CopyDataSourceSurface: Failed to map surface.");
    return nullptr;
  }

  SwizzleData(srcMap.GetData(), srcMap.GetStride(), format, dstMap.GetData(),
              dstMap.GetStride(), format, size);
  return dst.forget();
}

void Factory::CopyDataSourceSurface(DataSourceSurface* aSource,
                                    DataSourceSurface* aDest) {
  MOZ_ASSERT(aSource->GetSize() == aDest->GetSize());
  MOZ_ASSERT(aSource->GetFormat() == SurfaceFormat::R8G8B8A8 ||
             aSource->GetFormat() == SurfaceFormat::R8G8B8X8 ||
             aSource->GetFormat() == SurfaceFormat::B8G8R8A8 ||
             aSource->GetFormat() == SurfaceFormat::B8G8R8X8 ||
             aSource->GetFormat() == SurfaceFormat::A8);
  MOZ_ASSERT(aDest->GetFormat() == SurfaceFormat::R8G8B8A8 ||
             aDest->GetFormat() == SurfaceFormat::R8G8B8X8 ||
             aDest->GetFormat() == SurfaceFormat::B8G8R8A8 ||
             aDest->GetFormat() == SurfaceFormat::B8G8R8X8 ||
             aDest->GetFormat() == SurfaceFormat::R5G6B5_UINT16 ||
             aDest->GetFormat() == SurfaceFormat::A8);

  DataSourceSurface::MappedSurface srcMap;
  DataSourceSurface::MappedSurface destMap;
  if (!aSource->Map(DataSourceSurface::MapType::READ, &srcMap) ||
      !aDest->Map(DataSourceSurface::MapType::WRITE, &destMap)) {
    MOZ_ASSERT(false, "CopyDataSourceSurface: Failed to map surface.");
    return;
  }

  SwizzleData(srcMap.mData, srcMap.mStride, aSource->GetFormat(), destMap.mData,
              destMap.mStride, aDest->GetFormat(), aSource->GetSize());

  aSource->Unmap();
  aDest->Unmap();
}


void CriticalLogger::OutputMessage(const std::string& aString, int aLevel,
                                   bool aNoNewline) {
  if (Factory::GetLogForwarder()) {
    Factory::GetLogForwarder()->Log(aString);
  }

  BasicLogger::OutputMessage(aString, aLevel, aNoNewline);
}

void CriticalLogger::CrashAction(LogReason aReason) {
  if (Factory::GetLogForwarder()) {
    Factory::GetLogForwarder()->CrashAction(aReason);
  }
}


}  
