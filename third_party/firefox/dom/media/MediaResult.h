/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MediaResult_h_
#define MediaResult_h_

#include "mozilla/ErrorNames.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Logging.h"
#include "nsError.h"
#include "nsPrintfCString.h"
#include "nsString.h"  // Required before 'mozilla/ErrorNames.h'!?

namespace mozilla {

namespace dom {
class Promise;
}

class ErrorResult;

class MediaResult {
 public:
  MediaResult() : mCode(NS_OK) {}
  MOZ_IMPLICIT MediaResult(nsresult aResult) : mCode(aResult) {}
  MediaResult(nsresult aResult, const nsACString& aMessage,
              Maybe<int32_t> aPlatformErrorCode = Nothing())
      : mCode(aResult),
        mMessage(aMessage),
        mPlatformErrorCode(aPlatformErrorCode) {}
  MediaResult(nsresult aResult, const char* aMessage,
              Maybe<int32_t> aPlatformErrorCode = Nothing())
      : mCode(aResult),
        mMessage(aMessage),
        mPlatformErrorCode(aPlatformErrorCode) {}
  static MediaResult Logged(nsresult aResult, const char* aMessage,
                            const LogModule* aLogModule) {
    MOZ_LOG_FMT(aLogModule, LogLevel::Warning, "{}", aMessage);
    return MediaResult(aResult, aMessage);
  }
  static MediaResult Logged(nsresult aResult, const nsCString& aMessage,
                            const LogModule* aLogModule) {
    MOZ_LOG_FMT(aLogModule, LogLevel::Warning, "{}", aMessage.get());
    return MediaResult(aResult, aMessage);
  }
  MediaResult(const MediaResult& aOther) = default;
  MediaResult(MediaResult&& aOther) = default;
  MediaResult& operator=(const MediaResult& aOther) = default;
  MediaResult& operator=(MediaResult&& aOther) = default;

  nsresult Code() const { return mCode; }
  nsCString ErrorName() const {
    nsCString name;
    GetErrorName(mCode, name);
    return name;
  }

  const nsCString& Message() const { return mMessage; }

  bool operator==(nsresult aResult) const { return aResult == mCode; }
  bool operator!=(nsresult aResult) const { return aResult != mCode; }
  operator nsresult() const { return mCode; }

  nsCString Description() const {
    if (NS_SUCCEEDED(mCode)) {
      return nsCString();
    }
    return nsPrintfCString("%s (0x%08" PRIx32 ")%s%s", ErrorName().get(),
                           static_cast<uint32_t>(mCode),
                           mMessage.IsEmpty() ? "" : " - ", mMessage.get());
  }

  Maybe<int32_t> GetPlatformErrorCode() const { return mPlatformErrorCode; }

  void ThrowTo(ErrorResult& aRv) const;
  void RejectTo(dom::Promise* aPromise) const;

 private:
  nsresult mCode;
  nsCString mMessage;
  Maybe<int32_t> mPlatformErrorCode = Nothing();
};

#ifdef _MSC_VER
#  define RESULT_DETAIL(arg, ...) \
    nsPrintfCString("%s: " arg, __FUNCSIG__, ##__VA_ARGS__)
#else
#  define RESULT_DETAIL(arg, ...) \
    nsPrintfCString("%s: " arg, __PRETTY_FUNCTION__, ##__VA_ARGS__)
#endif

}  
#endif  // MediaResult_h_
