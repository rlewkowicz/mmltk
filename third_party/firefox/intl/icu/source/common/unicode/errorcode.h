// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2009-2011, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  errorcode.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2009mar10
*   created by: Markus W. Scherer
*/

#ifndef __ERRORCODE_H__
#define __ERRORCODE_H__


#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#include "unicode/uobject.h"

U_NAMESPACE_BEGIN

class U_COMMON_API ErrorCode: public UMemory {
public:
    ErrorCode() : errorCode(U_ZERO_ERROR) {}
    virtual ~ErrorCode();
    operator UErrorCode & () { return errorCode; }
    operator UErrorCode * () { return &errorCode; }
    UBool isSuccess() const { return U_SUCCESS(errorCode); }
    UBool isFailure() const { return U_FAILURE(errorCode); }
    UErrorCode get() const { return errorCode; }
    void set(UErrorCode value) { errorCode=value; }
    UErrorCode reset();
    void assertSuccess() const;
    const char* errorName() const;

protected:
    UErrorCode errorCode;
    virtual void handleFailure() const {}
};

U_NAMESPACE_END

#endif /* U_SHOW_CPLUSPLUS_API */

#endif  // __ERRORCODE_H__
