// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
* Copyright (C) 2007-2014, International Business Machines Corporation and
* others. All Rights Reserved.
*******************************************************************************
*

* File PLURFMT.H
********************************************************************************
*/

#ifndef PLURFMT
#define PLURFMT

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API


#if !UCONFIG_NO_FORMATTING

#include "unicode/messagepattern.h"
#include "unicode/numfmt.h"
#include "unicode/plurrule.h"

U_NAMESPACE_BEGIN

class Hashtable;
class NFRule;


class U_I18N_API_CLASS PluralFormat : public Format {
public:

    U_I18N_API PluralFormat(UErrorCode& status);

    U_I18N_API PluralFormat(const Locale& locale, UErrorCode& status);

    U_I18N_API PluralFormat(const PluralRules& rules, UErrorCode& status);

    U_I18N_API PluralFormat(const Locale& locale, const PluralRules& rules, UErrorCode& status);

    U_I18N_API PluralFormat(const Locale& locale, UPluralType type, UErrorCode& status);

    U_I18N_API PluralFormat(const UnicodeString& pattern, UErrorCode& status);

    U_I18N_API PluralFormat(const Locale& locale, const UnicodeString& pattern, UErrorCode& status);

    U_I18N_API PluralFormat(const PluralRules& rules, const UnicodeString& pattern, UErrorCode& status);

    U_I18N_API PluralFormat(const Locale& locale,
                            const PluralRules& rules,
                            const UnicodeString& pattern,
                            UErrorCode& status);

    U_I18N_API PluralFormat(const Locale& locale,
                            UPluralType type,
                            const UnicodeString& pattern,
                            UErrorCode& status);

    U_I18N_API PluralFormat(const PluralFormat& other);

    U_I18N_API virtual ~PluralFormat();

    U_I18N_API void applyPattern(const UnicodeString& pattern, UErrorCode& status);

    using Format::format;

    U_I18N_API UnicodeString format(int32_t number, UErrorCode& status) const;

    U_I18N_API UnicodeString format(double number, UErrorCode& status) const;

    U_I18N_API UnicodeString& format(int32_t number,
                                     UnicodeString& appendTo,
                                     FieldPosition& pos,
                                     UErrorCode& status) const;

    U_I18N_API UnicodeString& format(double number,
                                     UnicodeString& appendTo,
                                     FieldPosition& pos,
                                     UErrorCode& status) const;

#ifndef U_HIDE_DEPRECATED_API 
    U_I18N_API void setLocale(const Locale& locale, UErrorCode& status);
#endif  /* U_HIDE_DEPRECATED_API */

    U_I18N_API void setNumberFormat(const NumberFormat* format, UErrorCode& status);

    U_I18N_API PluralFormat& operator=(const PluralFormat& other);

    U_I18N_API virtual bool operator==(const Format& other) const override;

    U_I18N_API virtual bool operator!=(const Format& other) const;

    U_I18N_API virtual PluralFormat* clone() const override;

    U_I18N_API UnicodeString& format(const Formattable& obj,
                                     UnicodeString& appendTo,
                                     FieldPosition& pos,
                                     UErrorCode& status) const override;

    U_I18N_API UnicodeString& toPattern(UnicodeString& appendTo);

    U_I18N_API virtual void parseObject(const UnicodeString& source,
                                        Formattable& result,
                                        ParsePosition& parse_pos) const override;

    U_I18N_API static UClassID getStaticClassID();

    U_I18N_API virtual UClassID getDynamicClassID() const override;

private:
    class PluralSelector : public UMemory {
      public:
        virtual ~PluralSelector();
        virtual UnicodeString select(void *context, double number, UErrorCode& ec) const = 0;
    };

    class PluralSelectorAdapter : public PluralSelector {
      public:
        PluralSelectorAdapter() : pluralRules(nullptr) {
        }

        virtual ~PluralSelectorAdapter();

        virtual UnicodeString select(void *context, double number, UErrorCode& ) const override;

        void reset();

        PluralRules* pluralRules;
    };

    Locale  locale;
    MessagePattern msgPattern;
    NumberFormat*  numberFormat;
    double offset;
    PluralSelectorAdapter pluralRulesWrapper;

    PluralFormat() = delete;   
    void init(const PluralRules* rules, UPluralType type, UErrorCode& status);
    void copyObjects(const PluralFormat& other);

    UnicodeString& format(const Formattable& numberObject, double number,
                          UnicodeString& appendTo,
                          FieldPosition& pos,
                          UErrorCode& status) const;

    static int32_t findSubMessage(
         const MessagePattern& pattern, int32_t partIndex,
         const PluralSelector& selector, void *context, double number, UErrorCode& ec);

    void parseType(const UnicodeString& source, const NFRule *rbnfLenientScanner,
        Formattable& result, FieldPosition& pos) const;

    friend class MessageFormat;
    friend class NFRule;
};

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _PLURFMT
