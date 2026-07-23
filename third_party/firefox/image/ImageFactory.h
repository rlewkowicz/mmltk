/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_ImageFactory_h
#define mozilla_image_ImageFactory_h

#include "nsCOMPtr.h"
#include "nsProxyRelease.h"
#include "nsStringFwd.h"

class nsIRequest;
class nsIURI;

namespace mozilla {
namespace image {

class Image;
class MultipartImage;
class ProgressTracker;

class ImageFactory {
 public:
  static void Initialize();

  static already_AddRefed<Image> CreateImage(nsIRequest* aRequest,
                                             ProgressTracker* aProgressTracker,
                                             const nsCString& aMimeType,
                                             nsIURI* aURI, bool aIsMultiPart,
                                             uint64_t aInnerWindowId);
  static already_AddRefed<Image> CreateAnonymousImage(
      const nsCString& aMimeType, uint32_t aSizeHint = 0);

  static already_AddRefed<MultipartImage> CreateMultipartImage(
      Image* aFirstPart, ProgressTracker* aProgressTracker);

 private:
  static already_AddRefed<Image> CreateRasterImage(
      nsIRequest* aRequest, ProgressTracker* aProgressTracker,
      const nsCString& aMimeType, nsIURI* aURI, uint32_t aImageFlags,
      uint64_t aInnerWindowId);

  static already_AddRefed<Image> CreateVectorImage(
      nsIRequest* aRequest, ProgressTracker* aProgressTracker,
      const nsCString& aMimeType, nsIURI* aURI, uint32_t aImageFlags,
      uint64_t aInnerWindowId);

  virtual ~ImageFactory() = 0;
};

}  
}  

#endif  // mozilla_image_ImageFactory_h
