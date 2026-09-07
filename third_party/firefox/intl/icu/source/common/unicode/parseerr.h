// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 1999-2005, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*   Date        Name        Description
*   03/14/00    aliu        Creation.
*   06/27/00    aliu        Change from C++ class to C struct
**********************************************************************
*/
#ifndef PARSEERR_H
#define PARSEERR_H

#include "unicode/utypes.h"


enum { U_PARSE_CONTEXT_LEN = 16 };

typedef struct UParseError {

    int32_t        line;

    int32_t        offset;

    UChar          preContext[U_PARSE_CONTEXT_LEN];

    UChar          postContext[U_PARSE_CONTEXT_LEN];

} UParseError;

#endif
