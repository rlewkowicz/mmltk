/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_GeolocationIPCUtils_h
#define mozilla_dom_GeolocationIPCUtils_h

#include "ipc/EnumSerializer.h"

namespace mozilla::dom::geolocation {

enum class SystemGeolocationPermissionBehavior {
  NoPrompt,
  SystemWillPromptUser,
  GeckoWillPromptUser,
  Last = GeckoWillPromptUser,
};

enum class GeolocationPermissionStatus {
  Canceled,
  Granted,
  Error,
  Last = Error
};

}  

namespace IPC {

template <>
struct ParamTraits<
    mozilla::dom::geolocation::SystemGeolocationPermissionBehavior>
    : public ContiguousEnumSerializerInclusive<
          mozilla::dom::geolocation::SystemGeolocationPermissionBehavior,
          mozilla::dom::geolocation::SystemGeolocationPermissionBehavior::
              NoPrompt,
          mozilla::dom::geolocation::SystemGeolocationPermissionBehavior::
              Last> {};

template <>
struct ParamTraits<mozilla::dom::geolocation::GeolocationPermissionStatus>
    : public ContiguousEnumSerializerInclusive<
          mozilla::dom::geolocation::GeolocationPermissionStatus,
          mozilla::dom::geolocation::GeolocationPermissionStatus::Canceled,
          mozilla::dom::geolocation::GeolocationPermissionStatus::Last> {};

}  

#endif /* mozilla_dom_GeolocationIPCUtils_h */
