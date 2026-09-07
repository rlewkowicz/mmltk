/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ChromeObjectWrapper_h_
#define ChromeObjectWrapper_h_

#include "FilteringWrapper.h"

namespace xpc {

struct OpaqueWithSilentFailing;

#define ChromeObjectWrapperBase \
  FilteringWrapper<js::CrossCompartmentSecurityWrapper, OpaqueWithSilentFailing>

class ChromeObjectWrapper : public ChromeObjectWrapperBase {
 public:
  constexpr ChromeObjectWrapper() : ChromeObjectWrapperBase(0) {}

  virtual bool defineProperty(JSContext* cx, JS::Handle<JSObject*> wrapper,
                              JS::Handle<jsid> id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool set(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                   JS::HandleValue v, JS::HandleValue receiver,
                   JS::ObjectOpResult& result) const override;

  static const ChromeObjectWrapper singleton;
};

} 

#endif /* ChromeObjectWrapper_h_ */
