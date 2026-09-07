/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_FS_PARENT_FILESYSTEMPARENTTYPES_H_
#define DOM_FS_PARENT_FILESYSTEMPARENTTYPES_H_

#include "nsStringFwd.h"
#include "nsTString.h"

namespace mozilla::dom::fs {

struct FileId {
  explicit FileId(const nsCString& aValue) : mValue(aValue) {}

  explicit FileId(nsCString&& aValue) : mValue(std::move(aValue)) {}

  constexpr bool IsEmpty() const { return mValue.IsEmpty(); }

  constexpr const nsCString& Value() const { return mValue; }

  nsCString mValue;
};

inline bool operator==(const FileId& aLhs, const FileId& aRhs) {
  return aLhs.mValue == aRhs.mValue;
}

inline bool operator!=(const FileId& aLhs, const FileId& aRhs) {
  return aLhs.mValue != aRhs.mValue;
}

enum class FileMode { EXCLUSIVE, SHARED_FROM_EMPTY, SHARED_FROM_COPY };

}  

#endif  // DOM_FS_PARENT_FILESYSTEMPARENTTYPES_H_
