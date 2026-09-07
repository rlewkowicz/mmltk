// License & terms of use: http://www.unicode.org/copyright.html
/*
**************************************************************************
* Copyright (C) 1999-2012, International Business Machines Corporation and
* others. All Rights Reserved.
**************************************************************************
*   Date        Name        Description
*   11/17/99    aliu        Creation.  Ported from java.  Modified to
*                           match current UnicodeString API.  Forced
*                           to use name "handleReplaceBetween" because
*                           of existing methods in UnicodeString.
**************************************************************************
*/

#ifndef REP_H
#define REP_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#include "unicode/uobject.h"

 
U_NAMESPACE_BEGIN

class UnicodeString;

class U_COMMON_API Replaceable : public UObject {

public:
    virtual ~Replaceable();

    inline int32_t length() const;

    inline char16_t charAt(int32_t offset) const;

    inline UChar32 char32At(int32_t offset) const;

    virtual void extractBetween(int32_t start,
                                int32_t limit,
                                UnicodeString& target) const = 0;

    virtual void handleReplaceBetween(int32_t start,
                                      int32_t limit,
                                      const UnicodeString& text) = 0;

    virtual void copy(int32_t start, int32_t limit, int32_t dest) = 0;

    virtual UBool hasMetaData() const;

    virtual Replaceable *clone() const;

protected:

    inline Replaceable();


    virtual int32_t getLength() const = 0;

    virtual char16_t getCharAt(int32_t offset) const = 0;

    virtual UChar32 getChar32At(int32_t offset) const = 0;
};

inline Replaceable::Replaceable() {}

inline int32_t
Replaceable::length() const {
    return getLength();
}

inline char16_t
Replaceable::charAt(int32_t offset) const {
    return getCharAt(offset);
}

inline UChar32
Replaceable::char32At(int32_t offset) const {
    return getChar32At(offset);
}


U_NAMESPACE_END

#endif /* U_SHOW_CPLUSPLUS_API */

#endif
