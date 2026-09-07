/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ExpandedPrincipalJSONHandler_h
#define mozilla_ExpandedPrincipalJSONHandler_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "js/TypeDecls.h"  // JS::Latin1Char

#include "mozilla/Maybe.h"   // Maybe
#include "mozilla/RefPtr.h"  // RefPtr

#include "nsCOMPtr.h"      // nsCOMPtr
#include "nsDebug.h"       // NS_WARNING
#include "nsIPrincipal.h"  // nsIPrincipal
#include "nsTArray.h"      // nsTArray

#include "OriginAttributes.h"
#include "ExpandedPrincipal.h"
#include "SubsumedPrincipalJSONHandler.h"
#include "SharedJSONHandler.h"

namespace mozilla {

class ExpandedPrincipalJSONHandler : public PrincipalJSONHandlerShared {
  enum class State {
    Init,

    StartObject,

    SpecsKey,

    SuffixKey,

    StartArray,

    SubsumedPrincipal,

    AfterPropertyValue,

    EndObject,

    Error,
  };

 public:
  ExpandedPrincipalJSONHandler() = default;
  virtual ~ExpandedPrincipalJSONHandler() = default;

  virtual bool startObject() override;

  using PrincipalJSONHandlerShared::propertyName;
  virtual bool propertyName(const JS::Latin1Char* name, size_t length) override;

  virtual bool endObject() override;

  virtual bool startArray() override;
  virtual bool endArray() override;

  using PrincipalJSONHandlerShared::stringValue;
  virtual bool stringValue(const JS::Latin1Char* str, size_t length) override;

  bool HasAccepted() const { return mState == State::EndObject; }

 protected:
  virtual void SetErrorState() override { mState = State::Error; }

 private:
  bool ProcessSubsumedResult(bool aResult);

 private:
  State mState = State::Init;

  nsTArray<nsCOMPtr<nsIPrincipal>> mAllowList;
  OriginAttributes mAttrs;
  Maybe<SubsumedPrincipalJSONHandler> mSubsumedHandler;
};

}  

#endif  // mozilla_ExpandedPrincipalJSONHandler_h
