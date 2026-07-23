/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsHtml5OplessBuilder_h
#define nsHtml5OplessBuilder_h

#include "nsHtml5DocumentBuilder.h"
#include "nsTArray.h"

class nsParserBase;

class nsHtml5OplessBuilder : public nsHtml5DocumentBuilder {
 public:
  nsHtml5OplessBuilder();
  ~nsHtml5OplessBuilder();
  void Start();
  void Finish();
  void SetParser(nsParserBase* aParser);

 private:
  const size_t kRecyclableLength =
      ((1024 * sizeof(size_t)) - sizeof(nsTArrayHeader)) / sizeof(size_t);
};

#endif  // nsHtml5OplessBuilder_h
