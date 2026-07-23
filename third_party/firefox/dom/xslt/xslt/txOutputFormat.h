/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TRANSFRMX_OUTPUTFORMAT_H
#define TRANSFRMX_OUTPUTFORMAT_H

#include "nsString.h"
#include "txList.h"

enum txOutputMethod { eMethodNotSet, eXMLOutput, eHTMLOutput, eTextOutput };

enum txThreeState { eNotSet, eFalse, eTrue };

class txOutputFormat {
 public:
  txOutputFormat();
  ~txOutputFormat();

  void reset();

  void merge(txOutputFormat& aOutputFormat);

  void setFromDefaults();

  txOutputMethod mMethod;

  nsString mVersion;

  nsString mEncoding;

  txThreeState mOmitXMLDeclaration;

  txThreeState mStandalone;

  nsString mPublicId;

  nsString mSystemId;

  txList mCDATASectionElements;

  txThreeState mIndent;

  nsCString mMediaType;
};

#endif
