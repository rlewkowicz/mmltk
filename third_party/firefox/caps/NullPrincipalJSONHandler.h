/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_NullPrincipalJSONHandler_h
#define mozilla_NullPrincipalJSONHandler_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "js/TypeDecls.h"  // JS::Latin1Char

#include "mozilla/Assertions.h"  // MOZ_ASSERT_UNREACHABLE
#include "mozilla/RefPtr.h"      // RefPtr

#include "nsCOMPtr.h"  // nsCOMPtr
#include "nsDebug.h"   // NS_WARNING
#include "nsIURI.h"    // nsIURI

#include "NullPrincipal.h"
#include "OriginAttributes.h"
#include "SharedJSONHandler.h"

namespace mozilla {

class NullPrincipalJSONHandler : public PrincipalJSONHandlerShared {
  enum class State {
    Init,

    StartObject,

    SpecKey,

    SuffixKey,

    AfterPropertyValue,

    EndObject,

    Error,
  };

 public:
  NullPrincipalJSONHandler() = default;
  virtual ~NullPrincipalJSONHandler() = default;

  virtual bool startObject() override;

  using PrincipalJSONHandlerShared::propertyName;
  virtual bool propertyName(const JS::Latin1Char* name, size_t length) override;

  virtual bool endObject() override;

  virtual bool startArray() override {
    NS_WARNING("Unexpected array value");
    mState = State::Error;
    return false;
  }
  virtual bool endArray() override {
    NS_WARNING("Unexpected array value");
    mState = State::Error;
    return false;
  }

  using PrincipalJSONHandlerShared::stringValue;
  virtual bool stringValue(const JS::Latin1Char* str, size_t length) override;

  bool HasAccepted() const { return mState == State::EndObject; }

 protected:
  virtual void SetErrorState() override { mState = State::Error; }

 private:
  State mState = State::Init;

  nsCOMPtr<nsIURI> mUri;
  OriginAttributes mAttrs;
};

}  

#endif  // mozilla_NullPrincipalJSONHandler_h
