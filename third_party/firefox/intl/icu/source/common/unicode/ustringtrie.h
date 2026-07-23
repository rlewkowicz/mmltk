// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*   Copyright (C) 2010-2012, International Business Machines
*   Corporation and others.  All Rights Reserved.
*******************************************************************************
*   file name:  udicttrie.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2010dec17
*   created by: Markus W. Scherer
*/

#ifndef __USTRINGTRIE_H__
#define __USTRINGTRIE_H__


#include "unicode/utypes.h"


enum UStringTrieResult {
    USTRINGTRIE_NO_MATCH,
    USTRINGTRIE_NO_VALUE,
    USTRINGTRIE_FINAL_VALUE,
    USTRINGTRIE_INTERMEDIATE_VALUE
};

#define USTRINGTRIE_MATCHES(result) ((result)!=USTRINGTRIE_NO_MATCH)

#define USTRINGTRIE_HAS_VALUE(result) ((result)>=USTRINGTRIE_FINAL_VALUE)

#define USTRINGTRIE_HAS_NEXT(result) ((result)&1)

#endif  /* __USTRINGTRIE_H__ */
