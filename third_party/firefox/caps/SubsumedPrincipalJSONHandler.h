/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_SubsumedPrincipalJSONHandler_h
#define mozilla_SubsumedPrincipalJSONHandler_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "js/TypeDecls.h"  // JS::Latin1Char

#include "mozilla/Variant.h"  // Variant
#include "mozilla/RefPtr.h"   // RefPtr

#include "nsDebug.h"  // NS_WARNING

#include "BasePrincipal.h"
#include "ContentPrincipalJSONHandler.h"
#include "NullPrincipalJSONHandler.h"
#include "SharedJSONHandler.h"

namespace mozilla {

class SubsumedPrincipalJSONHandlerTypes {
 public:
  enum class State {
    Init,

    StartObject,

    SystemPrincipal_Key,
    SystemPrincipal_StartObject,
    SystemPrincipal_EndObject,

    NullPrincipal_Inner,
    ContentPrincipal_Inner,

    EndObject,

    Error,
  };

  using InnerHandlerT =
      Maybe<Variant<NullPrincipalJSONHandler, ContentPrincipalJSONHandler>>;

  static constexpr bool CanContainExpandedPrincipal = false;
};

class SubsumedPrincipalJSONHandler
    : public ContainerPrincipalJSONHandler<SubsumedPrincipalJSONHandlerTypes> {
  using State = SubsumedPrincipalJSONHandlerTypes::State;
  using InnerHandlerT = SubsumedPrincipalJSONHandlerTypes::InnerHandlerT;

 public:
  SubsumedPrincipalJSONHandler() = default;
  virtual ~SubsumedPrincipalJSONHandler() = default;

  bool HasAccepted() const { return mState == State::EndObject; }

 protected:
  virtual void SetErrorState() override { mState = State::Error; }
};

}  

#endif  // mozilla_SubsumedPrincipalJSONHandler_h
