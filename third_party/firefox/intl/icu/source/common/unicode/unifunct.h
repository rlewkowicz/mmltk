// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (c) 2002-2005, International Business Machines Corporation
*   and others.  All Rights Reserved.
**********************************************************************
*   Date        Name        Description
*   01/14/2002  aliu        Creation.
**********************************************************************
*/
#ifndef UNIFUNCT_H
#define UNIFUNCT_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#include "unicode/uobject.h"

 
U_NAMESPACE_BEGIN

class UnicodeMatcher;
class UnicodeReplacer;
class TransliterationRuleData;

class U_COMMON_API UnicodeFunctor : public UObject {

public:

    virtual ~UnicodeFunctor();

    virtual UnicodeFunctor* clone() const = 0;

    virtual UnicodeMatcher* toMatcher() const;

    virtual UnicodeReplacer* toReplacer() const;

    static UClassID U_EXPORT2 getStaticClassID();

    virtual UClassID getDynamicClassID() const override = 0;

    virtual void setData(const TransliterationRuleData*) = 0;

protected:


};


U_NAMESPACE_END

#endif /* U_SHOW_CPLUSPLUS_API */

#endif
