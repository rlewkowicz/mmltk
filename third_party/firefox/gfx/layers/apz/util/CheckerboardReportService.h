/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CheckerboardReportService_h
#define mozilla_dom_CheckerboardReportService_h

#include <string>

#include "js/TypeDecls.h"            // for JSContext, JSObject
#include "mozilla/StaticPtr.h"       // for StaticRefPtr
#include "nsCOMPtr.h"                // for nsCOMPtr
#include "nsISupports.h"             // for NS_INLINE_DECL_REFCOUNTING
#include "nsTArrayForwardDeclare.h"  // for nsTArray
#include "nsWrapperCache.h"          // for nsWrapperCache

namespace mozilla {

namespace dom {
struct CheckerboardReport;
}

namespace layers {

class CheckerboardEventStorage {
  NS_INLINE_DECL_REFCOUNTING(CheckerboardEventStorage)

 public:
  static already_AddRefed<CheckerboardEventStorage> GetInstance();

  void GetReports(nsTArray<dom::CheckerboardReport>& aOutReports);

  static void Report(uint32_t aSeverity, const std::string& aLog);

 private:
  CheckerboardEventStorage() = default;
  virtual ~CheckerboardEventStorage() = default;

  static StaticRefPtr<CheckerboardEventStorage> sInstance;

  void ReportCheckerboard(uint32_t aSeverity, const std::string& aLog);

 private:
  struct CheckerboardReport {
    uint32_t mSeverity;  
    int64_t mTimestamp;  
    std::string mLog;

    CheckerboardReport() : mSeverity(0), mTimestamp(0) {}

    CheckerboardReport(uint32_t aSeverity, int64_t aTimestamp,
                       const std::string& aLog)
        : mSeverity(aSeverity), mTimestamp(aTimestamp), mLog(aLog) {}
  };

  static const int SEVERITY_MAX_INDEX = 5;
  static const int RECENT_MAX_INDEX = 10;
  CheckerboardReport mCheckerboardReports[RECENT_MAX_INDEX];
};

}  

namespace dom {

class GlobalObject;

class CheckerboardReportService : public nsWrapperCache {
 public:
  static bool IsEnabled(JSContext* aCtx, JSObject* aGlobal);


  static already_AddRefed<CheckerboardReportService> Constructor(
      const dom::GlobalObject& aGlobal);

  explicit CheckerboardReportService(nsISupports* aSupports);

  JSObject* WrapObject(JSContext* aCtx,
                       JS::Handle<JSObject*> aGivenProto) override;

  nsISupports* GetParentObject();

  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(CheckerboardReportService)
  NS_DECL_CYCLE_COLLECTION_NATIVE_WRAPPERCACHE_CLASS(CheckerboardReportService)

 public:
  void GetReports(nsTArray<dom::CheckerboardReport>& aOutReports);
  bool IsRecordingEnabled() const;
  void SetRecordingEnabled(bool aEnabled);
  void FlushActiveReports();

 private:
  virtual ~CheckerboardReportService() = default;

  nsCOMPtr<nsISupports> mParent;
};

}  
}  

#endif /* mozilla_layers_CheckerboardReportService_h */
