/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef NSSERIALIZATIONHELPER_H_
#define NSSERIALIZATIONHELPER_H_

#include "nsStringFwd.h"
#include "nsISerializationHelper.h"

class nsISerializable;

nsresult NS_SerializeToString(nsISerializable* obj, nsACString& str);

nsresult NS_DeserializeObject(const nsACString& str, nsISupports** obj);

class nsSerializationHelper final : public nsISerializationHelper {
  ~nsSerializationHelper() = default;

  NS_DECL_ISUPPORTS
  NS_DECL_NSISERIALIZATIONHELPER
};

#endif
