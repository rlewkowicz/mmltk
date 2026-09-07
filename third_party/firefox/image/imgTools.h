/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_imgITools_h
#define mozilla_image_imgITools_h

#include "imgITools.h"

#define NS_IMGTOOLS_CID                       \
  { \
   0x3d8fa16d,                                \
   0xc9e1,                                    \
   0x4b50,                                    \
   {0xbd, 0xef, 0x2c, 0x7a, 0xe2, 0x49, 0x96, 0x7a}}

namespace mozilla {
namespace image {

class imgTools final : public imgITools {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_IMGITOOLS

  imgTools();

 private:
  virtual ~imgTools();
};

}  
}  

#endif  // mozilla_image_imgITools_h
