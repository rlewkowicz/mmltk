/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(PDMFactorySupport_h_)
#  define PDMFactorySupport_h_

#  include "MediaCodecsSupport.h"
#  include "PDMFactory.h"
#  include "mozilla/RefPtr.h"
#  include "mozilla/StaticMutex.h"
#  include "nsStringFwd.h"

namespace mozilla {

class DecoderDoctorDiagnostics;
struct SupportDecoderParams;

class PDMFactorySupport final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(PDMFactorySupport)

  static media::DecodeSupportSet IsTypeSupported(const nsACString& aMimeType);
  static media::DecodeSupportSet IsSupported(
      const SupportDecoderParams& aParams,
      DecoderDoctorDiagnostics* aDiagnostics);

  static RefPtr<PDMFactorySupport> Instance();

  static void Invalidate();

 private:
  PDMFactorySupport();
  ~PDMFactorySupport() = default;

  media::DecodeSupportSet SupportsMimeType(const nsACString& aMimeType) const {
    return mFactory->SupportsMimeType(aMimeType);
  }

  media::DecodeSupportSet Supports(
      const SupportDecoderParams& aParams,
      DecoderDoctorDiagnostics* aDiagnostics) const {
    return mFactory->Supports(aParams, aDiagnostics);
  }

  static bool EnsureInvalidationListenersRegistered();
  static void OnInvalidatingPrefChanged(const char* aPref, void* aData);
  static void OnInvalidatingGfxVarChanged();

  const RefPtr<PDMFactory> mFactory;
};

}  

#endif  // PDMFactorySupport_h_
