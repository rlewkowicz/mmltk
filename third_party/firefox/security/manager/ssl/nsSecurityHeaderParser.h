/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsSecurityHeaderParser_h
#define nsSecurityHeaderParser_h

#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "nsCOMPtr.h"
#include "nsString.h"

class nsSecurityHeaderDirective
    : public mozilla::LinkedListElement<nsSecurityHeaderDirective> {
 public:
  nsCString mName;
  mozilla::Maybe<nsCString> mValue;
};


class nsSecurityHeaderParser {
 public:
  explicit nsSecurityHeaderParser(const nsCString& aHeader);
  ~nsSecurityHeaderParser();

  nsresult Parse();
  mozilla::LinkedList<nsSecurityHeaderDirective>* GetDirectives();

 private:
  bool Accept(char aChr);
  bool Accept(bool (*aClassifier)(signed char));
  void Expect(char aChr);
  void Advance();
  void Header();          
  void Directive();       
  void DirectiveName();   
  void DirectiveValue();  
  void Token();           
  void QuotedString();    
  void QuotedText();      
  void QuotedPair();      

  void LWSMultiple();  
  void LWSCRLF();      
  void LWS();          

  mozilla::LinkedList<nsSecurityHeaderDirective> mDirectives;
  const char* mCursor;
  nsSecurityHeaderDirective* mDirective;

  nsCString mOutput;
  bool mError;
};

#endif  // nsSecurityHeaderParser_h
