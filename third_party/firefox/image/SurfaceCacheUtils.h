/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_SurfaceCacheUtils_h
#define mozilla_image_SurfaceCacheUtils_h


namespace mozilla {
namespace image {

class SurfaceCacheUtils {
 public:
  static void DiscardAll();

 private:
  virtual ~SurfaceCacheUtils() = 0;  
};

}  
}  

#endif  // mozilla_image_SurfaceCacheUtils_h
