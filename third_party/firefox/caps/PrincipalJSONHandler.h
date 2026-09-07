/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_PrincipalJSONHandler_h
#define mozilla_PrincipalJSONHandler_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "js/JSON.h"       // JS::JSONParseHandler
#include "js/TypeDecls.h"  // JS::Latin1Char

#include "mozilla/AlreadyAddRefed.h"  // already_AddRefed
#include "mozilla/RefPtr.h"           // RefPtr
#include "mozilla/Variant.h"          // Variant

#include "nsDebug.h"          // NS_WARNING
#include "nsPrintfCString.h"  // nsPrintfCString

#include "BasePrincipal.h"
#include "ContentPrincipalJSONHandler.h"
#include "ExpandedPrincipalJSONHandler.h"
#include "NullPrincipalJSONHandler.h"
#include "SharedJSONHandler.h"

namespace mozilla {

class PrincipalJSONHandlerTypes {
 public:
  enum class State {
    Init,

    StartObject,

    SystemPrincipal_Key,
    SystemPrincipal_StartObject,
    SystemPrincipal_EndObject,

    NullPrincipal_Inner,
    ContentPrincipal_Inner,
    ExpandedPrincipal_Inner,

    EndObject,

    Error,
  };

  using InnerHandlerT =
      Maybe<Variant<NullPrincipalJSONHandler, ContentPrincipalJSONHandler,
                    ExpandedPrincipalJSONHandler>>;

  static constexpr bool CanContainExpandedPrincipal = true;
};

class PrincipalJSONHandler
    : public ContainerPrincipalJSONHandler<PrincipalJSONHandlerTypes> {
  using State = PrincipalJSONHandlerTypes::State;
  using InnerHandlerT = PrincipalJSONHandlerTypes::InnerHandlerT;

 public:
  PrincipalJSONHandler() = default;
  virtual ~PrincipalJSONHandler() = default;

  virtual void error(const char* msg, uint32_t line, uint32_t column) override {
    NS_WARNING(
        nsPrintfCString("JSON Error: %s at line %u column %u of the JSON data",
                        msg, line, column)
            .get());
  }

  already_AddRefed<BasePrincipal> Get() { return mPrincipal.forget(); }

 protected:
  virtual void SetErrorState() override { mState = State::Error; }
};

}  

#endif  // mozilla_PrincipalJSONHandler_h
