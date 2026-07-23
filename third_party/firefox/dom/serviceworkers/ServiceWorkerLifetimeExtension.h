/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_serviceworkerlifetimeextension_h
#define mozilla_dom_serviceworkerlifetimeextension_h

#include "mozilla/TimeStamp.h"
#include "mozilla/Variant.h"

namespace mozilla::dom {

struct NoLifetimeExtension {};

struct PropagatedLifetimeExtension {
  TimeStamp mDeadline;
};

struct FullLifetimeExtension {};

struct ServiceWorkerLifetimeExtension
    : public Variant<NoLifetimeExtension, PropagatedLifetimeExtension,
                     FullLifetimeExtension> {
 public:
  explicit ServiceWorkerLifetimeExtension(NoLifetimeExtension aExt)
      : Variant(AsVariant(std::move(aExt))) {}
  explicit ServiceWorkerLifetimeExtension(PropagatedLifetimeExtension aExt)
      : Variant(AsVariant(std::move(aExt))) {}
  explicit ServiceWorkerLifetimeExtension(FullLifetimeExtension aExt)
      : Variant(AsVariant(std::move(aExt))) {}

  bool LifetimeExtendsIntoTheFuture(uint32_t aRequiredSecs = 5) const;
};

}  

#endif  // mozilla_dom_serviceworkerlifetimeextension_h
