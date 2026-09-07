/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef intl_components_NumberingSystem_h_
#define intl_components_NumberingSystem_h_

#include "mozilla/intl/ICUError.h"
#include "mozilla/Result.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtr.h"

struct UNumberingSystem;

namespace mozilla::intl {

class NumberingSystem final {
 public:
  explicit NumberingSystem(UNumberingSystem* aNumberingSystem)
      : mNumberingSystem(aNumberingSystem) {
    MOZ_ASSERT(aNumberingSystem);
  };

  NumberingSystem(const NumberingSystem&) = delete;
  NumberingSystem& operator=(const NumberingSystem&) = delete;

  ~NumberingSystem();

  static Result<UniquePtr<NumberingSystem>, ICUError> TryCreate(
      const char* aLocale);

  Result<Span<const char>, ICUError> GetName();

 private:
  UNumberingSystem* mNumberingSystem = nullptr;
};

}  

#endif
