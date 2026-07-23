// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*   Copyright (C) 2011, International Business Machines
*   Corporation and others.  All Rights Reserved.
*******************************************************************************
*   file name:  messageimpl.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2011apr04
*   created by: Markus W. Scherer
*/

#ifndef __MESSAGEIMPL_H__
#define __MESSAGEIMPL_H__

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/messagepattern.h"

U_NAMESPACE_BEGIN

class U_COMMON_API MessageImpl {
public:
    static UBool jdkAposMode(const MessagePattern &msgPattern) {
        return msgPattern.getApostropheMode()==UMSGPAT_APOS_DOUBLE_REQUIRED;
    }

    static void appendReducedApostrophes(const UnicodeString &s, int32_t start, int32_t limit,
                                         UnicodeString &sb);

    static UnicodeString &appendSubMessageWithoutSkipSyntax(const MessagePattern &msgPattern,
                                                            int32_t msgStart,
                                                            UnicodeString &result);

private:
    MessageImpl() = delete;  
};

U_NAMESPACE_END

#endif  // !UCONFIG_NO_FORMATTING

#endif  // __MESSAGEIMPL_H__
