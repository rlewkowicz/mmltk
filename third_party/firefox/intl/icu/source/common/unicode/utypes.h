// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 1996-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*
*  FILE NAME : UTYPES.H (formerly ptypes.h)
*
*   Date        Name        Description
*   12/11/96    helena      Creation.
*   02/27/97    aliu        Added typedefs for UClassID, int8, int16, int32,
*                           uint8, uint16, and uint32.
*   04/01/97    aliu        Added XP_CPLUSPLUS and modified to work under C as
*                            well as C++.
*                           Modified to use memcpy() for uprv_arrayCopy() fns.
*   04/14/97    aliu        Added TPlatformUtilities.
*   05/07/97    aliu        Added import/export specifiers (replacing the old
*                           broken EXT_CLASS).  Added version number for our
*                           code.  Cleaned up header.
*    6/20/97    helena      Java class name change.
*   08/11/98    stephen     UErrorCode changed from typedef to enum
*   08/12/98    erm         Changed T_ANALYTIC_PACKAGE_VERSION to 3
*   08/14/98    stephen     Added uprv_arrayCopy() for int8_t, int16_t, int32_t
*   12/09/98    jfitz       Added BUFFER_OVERFLOW_ERROR (bug 1100066)
*   04/20/99    stephen     Cleaned up & reworked for autoconf.
*                           Renamed to utypes.h.
*   05/05/99    stephen     Changed to use <inttypes.h>
*   12/07/99    helena      Moved copyright notice string from ucnv_bld.h here.
*******************************************************************************
*/

#if !defined(UTYPES_H)
#define UTYPES_H


#include "unicode/umachine.h"
#include "unicode/uversion.h"
#include "unicode/uconfig.h"
#include <float.h>

#if !U_NO_DEFAULT_INCLUDE_UTF_HEADERS
#   include "unicode/utf.h"
#endif

/*!
 * \file
 * \brief Basic definitions for ICU, for both C and C++ APIs
 *
 * This file defines basic types, constants, and enumerations directly or
 * indirectly by including other header files, especially utf.h for the
 * basic character and string definitions and umachine.h for consistent
 * integer and other types.
 */


#if defined(__cplusplus)
#if !defined(U_SHOW_CPLUSPLUS_API)
#       define U_SHOW_CPLUSPLUS_API 1
#endif
#if !defined(U_SHOW_CPLUSPLUS_HEADER_API)
#       define U_SHOW_CPLUSPLUS_HEADER_API 1
#endif
#else
#   undef U_SHOW_CPLUSPLUS_API
#   define U_SHOW_CPLUSPLUS_API 0
#   undef U_SHOW_CPLUSPLUS_HEADER_API
#   define U_SHOW_CPLUSPLUS_HEADER_API 0
#endif

#if !U_DEFAULT_SHOW_DRAFT && !defined(U_SHOW_DRAFT_API)
#define U_HIDE_DRAFT_API 1
#endif
#if !U_DEFAULT_SHOW_DRAFT && !defined(U_SHOW_INTERNAL_API)
#define U_HIDE_INTERNAL_API 1
#endif




#if U_CHARSET_FAMILY
#if U_IS_BIG_ENDIAN
#     define U_ICUDATA_TYPE_LETTER "e"
#     define U_ICUDATA_TYPE_LITLETTER e
#else
#     error "Don't know what to do with little endian EBCDIC!"
#     define U_ICUDATA_TYPE_LETTER "x"
#     define U_ICUDATA_TYPE_LITLETTER x
#endif
#else
#if U_IS_BIG_ENDIAN
#     define U_ICUDATA_TYPE_LETTER "b"
#     define U_ICUDATA_TYPE_LITLETTER b
#else
#     define U_ICUDATA_TYPE_LETTER "l"
#     define U_ICUDATA_TYPE_LITLETTER l
#endif
#endif

#define U_ICUDATA_NAME    "icudt" U_ICU_VERSION_SHORT U_ICUDATA_TYPE_LETTER
#if !defined(U_HIDE_INTERNAL_API)
#define U_USRDATA_NAME    "usrdt" U_ICU_VERSION_SHORT U_ICUDATA_TYPE_LETTER  /**< @internal */
#define U_USE_USRDATA     0  /**< @internal */
#endif

#define U_ICUDATA_ENTRY_POINT  U_DEF2_ICUDATA_ENTRY_POINT(U_ICU_VERSION_MAJOR_NUM,U_LIB_SUFFIX_C_NAME)

#if !defined(U_HIDE_INTERNAL_API)
#define U_DEF2_ICUDATA_ENTRY_POINT(major,suff) U_DEF_ICUDATA_ENTRY_POINT(major,suff)

#if !defined(U_DEF_ICUDATA_ENTRY_POINT)
#if !defined(U_LIB_SUFFIX_C_NAME)
#define U_DEF_ICUDATA_ENTRY_POINT(major, suff) icudt##major##_dat
#else
#define U_DEF_ICUDATA_ENTRY_POINT(major, suff) icudt##suff ## major##_dat
#endif
#endif
#endif

#if !defined(NULL)
#if defined(__cplusplus)
#define NULL    nullptr
#else
#define NULL    ((void *)0)
#endif
#endif


typedef double UDate;

#define U_MILLIS_PER_SECOND        (1000)
#define U_MILLIS_PER_MINUTE       (60000)
#define U_MILLIS_PER_HOUR       (3600000)
#define U_MILLIS_PER_DAY       (86400000)

#define U_DATE_MAX DBL_MAX

#define U_DATE_MIN -U_DATE_MAX











#if defined(U_IN_DOXYGEN)
#define U_COMBINED_IMPLEMENTATION 1
#endif

#if defined(U_COMBINED_IMPLEMENTATION)
#define U_DATA_API     U_EXPORT
#define U_COMMON_API   U_EXPORT
#define U_I18N_API     U_EXPORT
#define U_LAYOUT_API   U_EXPORT
#define U_LAYOUTEX_API U_EXPORT
#define U_IO_API       U_EXPORT
#define U_TOOLUTIL_API U_EXPORT
#elif defined(U_STATIC_IMPLEMENTATION)
#define U_DATA_API
#define U_COMMON_API
#define U_I18N_API
#define U_LAYOUT_API
#define U_LAYOUTEX_API
#define U_IO_API
#define U_TOOLUTIL_API
#elif defined(U_COMMON_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_EXPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_I18N_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_EXPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_LAYOUT_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_EXPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_LAYOUTEX_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_EXPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_IO_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_EXPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_TOOLUTIL_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_EXPORT
#else
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#endif

#if !defined(U_FORCE_HIDE_DRAFT_API)








#define U_DATA_API_CLASS     U_DATA_API
#define U_COMMON_API_CLASS   U_COMMON_API
#define U_I18N_API_CLASS     U_I18N_API
#define U_LAYOUT_API_CLASS   U_LAYOUT_API
#define U_LAYOUTEX_API_CLASS U_LAYOUTEX_API
#define U_IO_API_CLASS       U_IO_API
#define U_TOOLUTIL_API_CLASS U_TOOLUTIL_API

#endif

#if defined(__cplusplus)
#define U_STANDARD_CPP_NAMESPACE        ::
#else
#define U_STANDARD_CPP_NAMESPACE
#endif


typedef enum UErrorCode {

    U_USING_FALLBACK_WARNING  = -128,   

    U_ERROR_WARNING_START     = -128,   

    U_USING_DEFAULT_WARNING   = -127,   

    U_SAFECLONE_ALLOCATED_WARNING = -126, 

    U_STATE_OLD_WARNING       = -125,   

    U_STRING_NOT_TERMINATED_WARNING = -124,

    U_SORT_KEY_TOO_SHORT_WARNING = -123, 

    U_AMBIGUOUS_ALIAS_WARNING = -122,   

    U_DIFFERENT_UCA_VERSION = -121,     
    
    U_PLUGIN_CHANGED_LEVEL_WARNING = -120, 


#if !defined(U_HIDE_DEPRECATED_API)
    U_ERROR_WARNING_LIMIT,
#endif

    U_ZERO_ERROR              =  0,     

    U_ILLEGAL_ARGUMENT_ERROR  =  1,     
    U_MISSING_RESOURCE_ERROR  =  2,     
    U_INVALID_FORMAT_ERROR    =  3,     
    U_FILE_ACCESS_ERROR       =  4,     
    U_INTERNAL_PROGRAM_ERROR  =  5,     
    U_MESSAGE_PARSE_ERROR     =  6,     
    U_MEMORY_ALLOCATION_ERROR =  7,     
    U_INDEX_OUTOFBOUNDS_ERROR =  8,     
    U_PARSE_ERROR             =  9,     
    U_INVALID_CHAR_FOUND      = 10,     
    U_TRUNCATED_CHAR_FOUND    = 11,     
    U_ILLEGAL_CHAR_FOUND      = 12,     
    U_INVALID_TABLE_FORMAT    = 13,     
    U_INVALID_TABLE_FILE      = 14,     
    U_BUFFER_OVERFLOW_ERROR   = 15,     
    U_UNSUPPORTED_ERROR       = 16,     
    U_RESOURCE_TYPE_MISMATCH  = 17,     
    U_ILLEGAL_ESCAPE_SEQUENCE = 18,     
    U_UNSUPPORTED_ESCAPE_SEQUENCE = 19, 
    U_NO_SPACE_AVAILABLE      = 20,     
    U_CE_NOT_FOUND_ERROR      = 21,     
    U_PRIMARY_TOO_LONG_ERROR  = 22,     
    U_STATE_TOO_OLD_ERROR     = 23,     
    U_TOO_MANY_ALIASES_ERROR  = 24,     
    U_ENUM_OUT_OF_SYNC_ERROR  = 25,     
    U_INVARIANT_CONVERSION_ERROR = 26,  
    U_INVALID_STATE_ERROR     = 27,     
    U_COLLATOR_VERSION_MISMATCH = 28,   
    U_USELESS_COLLATOR_ERROR  = 29,     
    U_NO_WRITE_PERMISSION     = 30,     
    U_INPUT_TOO_LONG_ERROR = 31,

#if !defined(U_HIDE_DEPRECATED_API)
    U_STANDARD_ERROR_LIMIT = 32,
#endif

    U_BAD_VARIABLE_DEFINITION=0x10000,
    U_PARSE_ERROR_START = 0x10000,    
    U_MALFORMED_RULE,                 
    U_MALFORMED_SET,                  
    U_MALFORMED_SYMBOL_REFERENCE,     
    U_MALFORMED_UNICODE_ESCAPE,       
    U_MALFORMED_VARIABLE_DEFINITION,  
    U_MALFORMED_VARIABLE_REFERENCE,   
    U_MISMATCHED_SEGMENT_DELIMITERS,  
    U_MISPLACED_ANCHOR_START,         
    U_MISPLACED_CURSOR_OFFSET,        
    U_MISPLACED_QUANTIFIER,           
    U_MISSING_OPERATOR,               
    U_MISSING_SEGMENT_CLOSE,          
    U_MULTIPLE_ANTE_CONTEXTS,         
    U_MULTIPLE_CURSORS,               
    U_MULTIPLE_POST_CONTEXTS,         
    U_TRAILING_BACKSLASH,             
    U_UNDEFINED_SEGMENT_REFERENCE,    
    U_UNDEFINED_VARIABLE,             
    U_UNQUOTED_SPECIAL,               
    U_UNTERMINATED_QUOTE,             
    U_RULE_MASK_ERROR,                
    U_MISPLACED_COMPOUND_FILTER,      
    U_MULTIPLE_COMPOUND_FILTERS,      
    U_INVALID_RBT_SYNTAX,             
    U_INVALID_PROPERTY_PATTERN,       
    U_MALFORMED_PRAGMA,               
    U_UNCLOSED_SEGMENT,               
    U_ILLEGAL_CHAR_IN_SEGMENT,        
    U_VARIABLE_RANGE_EXHAUSTED,       
    U_VARIABLE_RANGE_OVERLAP,         
    U_ILLEGAL_CHARACTER,              
    U_INTERNAL_TRANSLITERATOR_ERROR,  
    U_INVALID_ID,                     
    U_INVALID_FUNCTION,               
#if !defined(U_HIDE_DEPRECATED_API)
    U_PARSE_ERROR_LIMIT,
#endif

    U_UNEXPECTED_TOKEN=0x10100,       
    U_FMT_PARSE_ERROR_START=0x10100,  
    U_MULTIPLE_DECIMAL_SEPARATORS,    
    U_MULTIPLE_DECIMAL_SEPERATORS = U_MULTIPLE_DECIMAL_SEPARATORS, 
    U_MULTIPLE_EXPONENTIAL_SYMBOLS,   
    U_MALFORMED_EXPONENTIAL_PATTERN,  
    U_MULTIPLE_PERCENT_SYMBOLS,       
    U_MULTIPLE_PERMILL_SYMBOLS,       
    U_MULTIPLE_PAD_SPECIFIERS,        
    U_PATTERN_SYNTAX_ERROR,           
    U_ILLEGAL_PAD_POSITION,           
    U_UNMATCHED_BRACES,               
    U_UNSUPPORTED_PROPERTY,           
    U_UNSUPPORTED_ATTRIBUTE,          
    U_ARGUMENT_TYPE_MISMATCH,         
    U_DUPLICATE_KEYWORD,              
    U_UNDEFINED_KEYWORD,              
    U_DEFAULT_KEYWORD_MISSING,        
    U_DECIMAL_NUMBER_SYNTAX_ERROR,    
    U_FORMAT_INEXACT_ERROR,           
    U_NUMBER_ARG_OUTOFBOUNDS_ERROR,   
    U_NUMBER_SKELETON_SYNTAX_ERROR,   

    U_MF_UNRESOLVED_VARIABLE_ERROR,    
    U_MF_SYNTAX_ERROR,                 
    U_MF_UNKNOWN_FUNCTION_ERROR,       
    U_MF_VARIANT_KEY_MISMATCH_ERROR,   
    U_MF_FORMATTING_ERROR,             
    U_MF_NONEXHAUSTIVE_PATTERN_ERROR,  
    U_MF_DUPLICATE_OPTION_NAME_ERROR,  
    U_MF_SELECTOR_ERROR,               
    U_MF_MISSING_SELECTOR_ANNOTATION_ERROR,  
    U_MF_DUPLICATE_DECLARATION_ERROR, 
    U_MF_OPERAND_MISMATCH_ERROR,     
    U_MF_DUPLICATE_VARIANT_ERROR, 
    U_MF_BAD_OPTION,             
#if !defined(U_HIDE_DEPRECATED_API)
    U_FMT_PARSE_ERROR_LIMIT = 0x10121,
#endif

    U_BRK_INTERNAL_ERROR=0x10200,          
    U_BRK_ERROR_START=0x10200,             
    U_BRK_HEX_DIGITS_EXPECTED,             
    U_BRK_SEMICOLON_EXPECTED,              
    U_BRK_RULE_SYNTAX,                     
    U_BRK_UNCLOSED_SET,                    
    U_BRK_ASSIGN_ERROR,                    
    U_BRK_VARIABLE_REDFINITION,            
    U_BRK_MISMATCHED_PAREN,                
    U_BRK_NEW_LINE_IN_QUOTED_STRING,       
    U_BRK_UNDEFINED_VARIABLE,              
    U_BRK_INIT_ERROR,                      
    U_BRK_RULE_EMPTY_SET,                  
    U_BRK_UNRECOGNIZED_OPTION,             
    U_BRK_MALFORMED_RULE_TAG,              
#if !defined(U_HIDE_DEPRECATED_API)
    U_BRK_ERROR_LIMIT,
#endif

    U_REGEX_INTERNAL_ERROR=0x10300,       
    U_REGEX_ERROR_START=0x10300,          
    U_REGEX_RULE_SYNTAX,                  
    U_REGEX_INVALID_STATE,                
    U_REGEX_BAD_ESCAPE_SEQUENCE,          
    U_REGEX_PROPERTY_SYNTAX,              
    U_REGEX_UNIMPLEMENTED,                
    U_REGEX_MISMATCHED_PAREN,             
    U_REGEX_NUMBER_TOO_BIG,               
    U_REGEX_BAD_INTERVAL,                 
    U_REGEX_MAX_LT_MIN,                   
    U_REGEX_INVALID_BACK_REF,             
    U_REGEX_INVALID_FLAG,                 
    U_REGEX_LOOK_BEHIND_LIMIT,            
    U_REGEX_SET_CONTAINS_STRING,          
#if !defined(U_HIDE_DEPRECATED_API)
    U_REGEX_OCTAL_TOO_BIG,                
#endif
    U_REGEX_MISSING_CLOSE_BRACKET=U_REGEX_SET_CONTAINS_STRING+2, 
    U_REGEX_INVALID_RANGE,                
    U_REGEX_STACK_OVERFLOW,               
    U_REGEX_TIME_OUT,                     
    U_REGEX_STOPPED_BY_CALLER,            
    U_REGEX_PATTERN_TOO_BIG,              
    U_REGEX_INVALID_CAPTURE_GROUP_NAME,   
#if !defined(U_HIDE_DEPRECATED_API)
    U_REGEX_ERROR_LIMIT=U_REGEX_STOPPED_BY_CALLER+3,
#endif

    U_IDNA_PROHIBITED_ERROR=0x10400,
    U_IDNA_ERROR_START=0x10400,
    U_IDNA_UNASSIGNED_ERROR,
    U_IDNA_CHECK_BIDI_ERROR,
    U_IDNA_STD3_ASCII_RULES_ERROR,
    U_IDNA_ACE_PREFIX_ERROR,
    U_IDNA_VERIFICATION_ERROR,
    U_IDNA_LABEL_TOO_LONG_ERROR,
    U_IDNA_ZERO_LENGTH_LABEL_ERROR,
    U_IDNA_DOMAIN_NAME_TOO_LONG_ERROR,
#if !defined(U_HIDE_DEPRECATED_API)
    U_IDNA_ERROR_LIMIT,
#endif
    U_STRINGPREP_PROHIBITED_ERROR = U_IDNA_PROHIBITED_ERROR,
    U_STRINGPREP_UNASSIGNED_ERROR = U_IDNA_UNASSIGNED_ERROR,
    U_STRINGPREP_CHECK_BIDI_ERROR = U_IDNA_CHECK_BIDI_ERROR,
    
    U_PLUGIN_ERROR_START=0x10500,         
    U_PLUGIN_TOO_HIGH=0x10500,            
    U_PLUGIN_DIDNT_SET_LEVEL,             
#if !defined(U_HIDE_DEPRECATED_API)
    U_PLUGIN_ERROR_LIMIT,
#endif

#if !defined(U_HIDE_DEPRECATED_API)
    U_ERROR_LIMIT=U_PLUGIN_ERROR_LIMIT
#endif
} UErrorCode;


#if defined(__cplusplus)
    static
    inline UBool U_SUCCESS(UErrorCode code) { return code <= U_ZERO_ERROR; }
    static
    inline UBool U_FAILURE(UErrorCode code) { return code > U_ZERO_ERROR; }
#else
#   define U_SUCCESS(x) ((x)<=U_ZERO_ERROR)
#   define U_FAILURE(x) ((x)>U_ZERO_ERROR)
#endif

U_CAPI const char * U_EXPORT2
u_errorName(UErrorCode code);


#endif
