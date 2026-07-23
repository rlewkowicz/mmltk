// License & terms of use: http://www.unicode.org/copyright.html
/*
********************************************************************************
*   Copyright (C) 1997-2013, International Business Machines
*   Corporation and others.  All Rights Reserved.
********************************************************************************
*
* File CHOICFMT.H
*
* Modification History:
*
*   Date        Name        Description
*   02/19/97    aliu        Converted from java.
*   03/20/97    helena      Finished first cut of implementation and got rid
*                           of nextDouble/previousDouble and replaced with
*                           boolean array.
*   4/10/97     aliu        Clean up.  Modified to work on AIX.
*   8/6/97      nos         Removed overloaded constructor, member var 'buffer'.
*   07/22/98    stephen     Removed operator!= (implemented in Format)
********************************************************************************
*/

#if !defined(CHOICFMT_H)
#define CHOICFMT_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API


#if !UCONFIG_NO_FORMATTING

#include "unicode/fieldpos.h"
#include "unicode/format.h"
#include "unicode/messagepattern.h"
#include "unicode/numfmt.h"
#include "unicode/unistr.h"

#if !defined(U_HIDE_DEPRECATED_API)

U_NAMESPACE_BEGIN

class MessageFormat;

class U_I18N_API ChoiceFormat: public NumberFormat {
public:
    ChoiceFormat(const UnicodeString& pattern,
                 UErrorCode& status);


    ChoiceFormat(const double* limits,
                 const UnicodeString* formats,
                 int32_t count );

    ChoiceFormat(const double* limits,
                 const UBool* closures,
                 const UnicodeString* formats,
                 int32_t count);

    ChoiceFormat(const ChoiceFormat& that);

    const ChoiceFormat& operator=(const ChoiceFormat& that);

    virtual ~ChoiceFormat();

    virtual ChoiceFormat* clone() const override;

    virtual bool operator==(const Format& other) const override;

    virtual void applyPattern(const UnicodeString& pattern,
                              UErrorCode& status);

    virtual void applyPattern(const UnicodeString& pattern,
                             UParseError& parseError,
                             UErrorCode& status);
    virtual UnicodeString& toPattern(UnicodeString &pattern) const;

    virtual void setChoices(const double* limitsToCopy,
                            const UnicodeString* formatsToCopy,
                            int32_t count );

    virtual void setChoices(const double* limits,
                            const UBool* closures,
                            const UnicodeString* formats,
                            int32_t count);

    virtual const double* getLimits(int32_t& count) const;

    virtual const UBool* getClosures(int32_t& count) const;

    virtual const UnicodeString* getFormats(int32_t& count) const;


    using NumberFormat::format;

    virtual UnicodeString& format(double number,
                                  UnicodeString& appendTo,
                                  FieldPosition& pos) const override;
    virtual UnicodeString& format(int32_t number,
                                  UnicodeString& appendTo,
                                  FieldPosition& pos) const override;

    virtual UnicodeString& format(int64_t number,
                                  UnicodeString& appendTo,
                                  FieldPosition& pos) const override;

    virtual UnicodeString& format(const Formattable* objs,
                                  int32_t cnt,
                                  UnicodeString& appendTo,
                                  FieldPosition& pos,
                                  UErrorCode& success) const;

   using NumberFormat::parse;

    virtual void parse(const UnicodeString& text,
                       Formattable& result,
                       ParsePosition& parsePosition) const override;

    virtual UClassID getDynamicClassID() const override;

    static UClassID U_EXPORT2 getStaticClassID();

private:
    static UnicodeString& dtos(double value, UnicodeString& string);

    ChoiceFormat() = delete; 

    ChoiceFormat(const UnicodeString& newPattern,
                 UParseError& parseError,
                 UErrorCode& status);

    friend class MessageFormat;

    virtual void setChoices(const double* limits,
                            const UBool* closures,
                            const UnicodeString* formats,
                            int32_t count,
                            UErrorCode &errorCode);

    static int32_t findSubMessage(const MessagePattern &pattern, int32_t partIndex, double number);

    static double parseArgument(
            const MessagePattern &pattern, int32_t partIndex,
            const UnicodeString &source, ParsePosition &pos);

    static int32_t matchStringUntilLimitPart(
            const MessagePattern &pattern, int32_t partIndex, int32_t limitPartIndex,
            const UnicodeString &source, int32_t sourceOffset);

    UErrorCode constructorErrorCode;

    MessagePattern msgPattern;

};


U_NAMESPACE_END

#endif
#endif

#endif

#endif
