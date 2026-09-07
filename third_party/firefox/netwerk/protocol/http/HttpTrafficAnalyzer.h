/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_netwerk_protocol_http_HttpTrafficAnalyzer_h
#define mozilla_netwerk_protocol_http_HttpTrafficAnalyzer_h

#include <stdint.h>
#include "nsTArrayForwardDeclare.h"

#define FOR_EACH_HTTP_TRAFFIC_CATEGORY(DEFINE_CATEGORY)                         \
                                   \
  DEFINE_CATEGORY(N1Sys, 0)                                                     \
                \
  DEFINE_CATEGORY(N1, 1)                                                        \
                                                     \
  DEFINE_CATEGORY(N3Oth, 2)                                                     \
                                                          \
  DEFINE_CATEGORY(N3BasicLead, 3)                                               \
                                                          \
  DEFINE_CATEGORY(N3BasicBg, 4)                                                 \
                                                          \
  DEFINE_CATEGORY(N3BasicOth, 5)                                                \
               \
  DEFINE_CATEGORY(N3ContentLead, 6)                                             \
               \
  DEFINE_CATEGORY(N3ContentBg, 7)                                               \
               \
  DEFINE_CATEGORY(N3ContentOth, 8)                                              \
        \
  DEFINE_CATEGORY(N3FpLead, 9)                                                  \
        \
  DEFINE_CATEGORY(N3FpBg, 10)                                                   \
        \
  DEFINE_CATEGORY(N3FpOth, 11)                                                  \
                         \
  DEFINE_CATEGORY(P1Sys, 12)                                                    \
                                                                    \
  DEFINE_CATEGORY(P1, 13)                                                       \
                              \
  DEFINE_CATEGORY(P3Oth, 14)                                                    \
                                      \
  DEFINE_CATEGORY(P3BasicLead, 15)                                              \
                                      \
  DEFINE_CATEGORY(P3BasicBg, 16)                                                \
                                      \
  DEFINE_CATEGORY(P3BasicOth, 17)                                               \
                                                                       \
  DEFINE_CATEGORY(P3ContentLead, 18)                                            \
                                                                       \
  DEFINE_CATEGORY(P3ContentBg, 19)                                              \
                                                                       \
  DEFINE_CATEGORY(P3ContentOth, 20)                                             \
                                                               \
  DEFINE_CATEGORY(P3FpLead, 21)                                                 \
                                                               \
  DEFINE_CATEGORY(P3FpBg, 22)                                                   \
                                                               \
  DEFINE_CATEGORY(P3FpOth, 23)

namespace mozilla {
namespace net {

#define DEFINE_CATEGORY(_name, _idx) e##_name = _idx##u,
enum HttpTrafficCategory : uint8_t {
  FOR_EACH_HTTP_TRAFFIC_CATEGORY(DEFINE_CATEGORY) eInvalid,
};
#undef DEFINE_CATEGORY

class HttpTrafficAnalyzer final {
 public:
  enum ClassOfService : uint8_t {
    eLeader = 0,
    eBackground = 1,
    eOther = 255,
  };

  enum TrackingClassification : uint8_t {
    eNone = 0,
    eBasic = 1,
    eContent = 2,
    eFingerprinting = 3,
  };

  static HttpTrafficCategory CreateTrafficCategory(
      bool aIsPrivateMode, bool aIsSystemPrincipal, bool aIsThirdParty,
      ClassOfService aClassOfService, TrackingClassification aClassification);

  void IncrementHttpTransaction(HttpTrafficCategory aCategory);
  void IncrementHttpConnection(HttpTrafficCategory aCategory);
  void IncrementHttpConnection(nsTArray<HttpTrafficCategory>&& aCategories);
};

}  
}  

#endif  // mozilla_netwerk_protocol_http_HttpTrafficAnalyzer_h
