/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_image_ShutdownTracker_h
#define mozilla_image_ShutdownTracker_h

namespace mozilla {
namespace image {

struct ShutdownTracker {
  static void Initialize();

  static bool ShutdownHasStarted();

 private:
  virtual ~ShutdownTracker() = 0;  
};

}  
}  

#endif  // mozilla_image_ShutdownTracker_h
