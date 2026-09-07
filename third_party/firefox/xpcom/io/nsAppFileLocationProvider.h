/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsAppFileLocationProvider_h
#define nsAppFileLocationProvider_h

#include "nsIDirectoryService.h"
#include "nsCOMPtr.h"

class nsIFile;


class nsAppFileLocationProvider final : public nsIDirectoryServiceProvider {
 public:
  nsAppFileLocationProvider();

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIDIRECTORYSERVICEPROVIDER

 private:
  ~nsAppFileLocationProvider() = default;

 protected:
  nsresult CloneMozBinDirectory(nsIFile** aLocalFile);
  nsresult GetProductDirectory(nsIFile** aLocalFile, bool aLocal = false);
  nsresult GetDefaultUserProfileRoot(nsIFile** aLocalFile, bool aLocal = false);

  nsCOMPtr<nsIFile> mMozBinDirectory;
};

#endif
