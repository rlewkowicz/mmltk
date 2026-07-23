// License & terms of use: http://www.unicode.org/copyright.html
/*
* Copyright (C) 2001-2005, International Business Machines Corporation and others. All Rights Reserved.
**********************************************************************
*   Date        Name        Description
*   07/18/01    aliu        Creation.
**********************************************************************
*/
#ifndef UNIMATCH_H
#define UNIMATCH_H

#include "unicode/utypes.h"


#if U_SHOW_CPLUSPLUS_API

U_NAMESPACE_BEGIN

class Replaceable;
class UnicodeString;
class UnicodeSet;

enum UMatchDegree {
    U_MISMATCH,
    
    U_PARTIAL_MATCH,
    
    U_MATCH
};

class U_COMMON_API UnicodeMatcher  {

public:
    virtual ~UnicodeMatcher();

    virtual UMatchDegree matches(const Replaceable& text,
                                 int32_t& offset,
                                 int32_t limit,
                                 UBool incremental) = 0;

    virtual UnicodeString& toPattern(UnicodeString& result,
                                     UBool escapeUnprintable = false) const = 0;

    virtual UBool matchesIndexValue(uint8_t v) const = 0;

    virtual void addMatchSetTo(UnicodeSet& toUnionTo) const = 0;
};

U_NAMESPACE_END

#endif /* U_SHOW_CPLUSPLUS_API */

#endif
