/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_DIRECTORYLOCKCATEGORY_H_
#define DOM_QUOTA_DIRECTORYLOCKCATEGORY_H_

#include "mozilla/EnumSet.h"

namespace mozilla::dom::quota {

enum class DirectoryLockCategory : uint8_t {
  None = 0,
  UninitStorage,
  UninitOrigins,
  UninitClients,
};


constexpr EnumSet<DirectoryLockCategory> kUninitStorageOnlyCategory = {
    DirectoryLockCategory::UninitStorage};

constexpr EnumSet<DirectoryLockCategory> kUninitOriginsAndBroaderCategories = {
    DirectoryLockCategory::UninitOrigins, DirectoryLockCategory::UninitStorage};

constexpr EnumSet<DirectoryLockCategory> kUninitClientsAndBroaderCategories = {
    DirectoryLockCategory::UninitClients, DirectoryLockCategory::UninitOrigins,
    DirectoryLockCategory::UninitStorage};

}  

#endif  // DOM_QUOTA_DIRECTORYLOCKCATEGORY_H_
