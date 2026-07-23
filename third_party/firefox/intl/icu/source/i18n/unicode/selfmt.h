// License & terms of use: http://www.unicode.org/copyright.html
/********************************************************************
 * COPYRIGHT:
 * Copyright (c) 1997-2011, International Business Machines Corporation and
 * others. All Rights Reserved.
 * Copyright (C) 2010 , Yahoo! Inc.
 ********************************************************************
 *
 * File SELFMT.H
 *
 * Modification History:
 *
 *   Date        Name        Description
 *   11/11/09    kirtig      Finished first cut of implementation.
 ********************************************************************/

#ifndef SELFMT
#define SELFMT

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#include "unicode/messagepattern.h"
#include "unicode/numfmt.h"


#if !UCONFIG_NO_FORMATTING

U_NAMESPACE_BEGIN

class MessageFormat;


class U_I18N_API SelectFormat : public Format {
public:

    SelectFormat(const UnicodeString& pattern, UErrorCode& status);

    SelectFormat(const SelectFormat& other);

    virtual ~SelectFormat();

    void applyPattern(const UnicodeString& pattern, UErrorCode& status);


    using Format::format;

    UnicodeString& format(const UnicodeString& keyword,
                            UnicodeString& appendTo,
                            FieldPosition& pos,
                            UErrorCode& status) const;

    SelectFormat& operator=(const SelectFormat& other);

    virtual bool operator==(const Format& other) const override;

    virtual bool operator!=(const Format& other) const;

    virtual SelectFormat* clone() const override;

    UnicodeString& format(const Formattable& obj,
                         UnicodeString& appendTo,
                         FieldPosition& pos,
                         UErrorCode& status) const override;

    UnicodeString& toPattern(UnicodeString& appendTo);

    virtual void parseObject(const UnicodeString& source,
                            Formattable& result,
                            ParsePosition& parse_pos) const override;

    static UClassID U_EXPORT2 getStaticClassID();

    virtual UClassID getDynamicClassID() const override;

private:
    friend class MessageFormat;

    SelectFormat() = delete;   

    static int32_t findSubMessage(const MessagePattern& pattern, int32_t partIndex,
                                  const UnicodeString& keyword, UErrorCode& ec);

    MessagePattern msgPattern;
};

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _SELFMT
