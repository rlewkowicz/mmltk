/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsParserMsgUtils_h
#define nsParserMsgUtils_h

#include "nsString.h"

#define XMLPARSER_PROPERTIES \
  "chrome://global/locale/layout/xmlparser.properties"

#define XMLPARSER_PROPERTIES_en_US \
  "resource://gre/res/locale/layout/xmlparser.properties"

class nsParserMsgUtils {
 public:
  static nsresult GetLocalizedStringByName(const char* aPropFileName,
                                           const char* aKey, nsString& aVal);
  static nsresult GetLocalizedStringByID(const char* aPropFileName,
                                         uint32_t aID, nsString& aVal);

  nsParserMsgUtils() = delete;  
  ~nsParserMsgUtils() =
      delete;  
};

#endif
