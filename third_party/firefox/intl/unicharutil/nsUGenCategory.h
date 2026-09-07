/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsUGenCategory_h
#define nsUGenCategory_h

enum class nsUGenCategory {
  kUndefined = 0,
  kMark = 1,         
  kNumber = 2,       
  kSeparator = 3,    
  kOther = 4,        
  kLetter = 5,       
  kPunctuation = 6,  
  kSymbol = 7        
};

#endif  // nsUGenCategory_h
