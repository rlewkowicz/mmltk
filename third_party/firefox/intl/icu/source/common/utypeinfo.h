// License & terms of use: http://www.unicode.org/copyright.html
/*
******************************************************************************
*
*   Copyright (C) 2012-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
******************************************************************************
*/

#ifndef __UTYPEINFO_H__
#define __UTYPEINFO_H__

#if defined(_MSC_VER) && _HAS_EXCEPTIONS == 0
#include <exception>
using std::exception;
#endif
#if defined(__GLIBCXX__)
namespace std { class type_info; } 
#endif
#include <typeinfo>  // for 'typeid' to work

#endif
