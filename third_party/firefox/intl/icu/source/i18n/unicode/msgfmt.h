// License & terms of use: http://www.unicode.org/copyright.html
/*
* Copyright (C) 2007-2013, International Business Machines Corporation and
* others. All Rights Reserved.
********************************************************************************
*
* File MSGFMT.H
*
* Modification History:
*
*   Date        Name        Description
*   02/19/97    aliu        Converted from java.
*   03/20/97    helena      Finished first cut of implementation.
*   07/22/98    stephen     Removed operator!= (defined in Format)
*   08/19/2002  srl         Removing Javaisms
*******************************************************************************/

#ifndef MSGFMT_H
#define MSGFMT_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API


#if !UCONFIG_NO_FORMATTING

#include "unicode/format.h"
#include "unicode/locid.h"
#include "unicode/messagepattern.h"
#include "unicode/parseerr.h"
#include "unicode/plurfmt.h"
#include "unicode/plurrule.h"

U_CDECL_BEGIN
struct UHashtable;
typedef struct UHashtable UHashtable; 
U_CDECL_END

U_NAMESPACE_BEGIN

class AppendableWrapper;
class DateFormat;
class NumberFormat;

class U_I18N_API_CLASS MessageFormat : public Format {
public:
#ifndef U_HIDE_OBSOLETE_API
    enum EFormatNumber {
        kMaxFormat = 10
    };
#endif  /* U_HIDE_OBSOLETE_API */

    U_I18N_API MessageFormat(const UnicodeString& pattern, UErrorCode& status);

    U_I18N_API MessageFormat(const UnicodeString& pattern, const Locale& newLocale, UErrorCode& status);
    U_I18N_API MessageFormat(const UnicodeString& pattern,
                             const Locale& newLocale,
                             UParseError& parseError,
                             UErrorCode& status);
    U_I18N_API MessageFormat(const MessageFormat&);

    U_I18N_API const MessageFormat& operator=(const MessageFormat&);

    U_I18N_API virtual ~MessageFormat();

    U_I18N_API virtual MessageFormat* clone() const override;

    U_I18N_API virtual bool operator==(const Format& other) const override;

    U_I18N_API virtual void setLocale(const Locale& theLocale);

    U_I18N_API virtual const Locale& getLocale() const;

    U_I18N_API virtual void applyPattern(const UnicodeString& pattern, UErrorCode& status);
    U_I18N_API virtual void applyPattern(const UnicodeString& pattern,
                                         UParseError& parseError,
                                         UErrorCode& status);

    U_I18N_API virtual void applyPattern(const UnicodeString& pattern,
                                         UMessagePatternApostropheMode aposMode,
                                         UParseError* parseError,
                                         UErrorCode& status);

    U_I18N_API UMessagePatternApostropheMode getApostropheMode() const {
        return msgPattern.getApostropheMode();
    }

    U_I18N_API virtual UnicodeString& toPattern(UnicodeString& appendTo) const;

    U_I18N_API virtual void adoptFormats(Format** formatsToAdopt, int32_t count);

    U_I18N_API virtual void setFormats(const Format** newFormats, int32_t cnt);

    U_I18N_API virtual void adoptFormat(int32_t formatNumber, Format* formatToAdopt);

    U_I18N_API virtual void setFormat(int32_t formatNumber, const Format& format);

    U_I18N_API virtual StringEnumeration* getFormatNames(UErrorCode& status);

    U_I18N_API virtual Format* getFormat(const UnicodeString& formatName, UErrorCode& status);

    U_I18N_API virtual void setFormat(const UnicodeString& formatName,
                                      const Format& format,
                                      UErrorCode& status);

    U_I18N_API virtual void adoptFormat(const UnicodeString& formatName,
                                        Format* formatToAdopt,
                                        UErrorCode& status);

    U_I18N_API virtual const Format** getFormats(int32_t& count) const;

    using Format::format;

    U_I18N_API UnicodeString& format(const Formattable* source,
                                     int32_t count,
                                     UnicodeString& appendTo,
                                     FieldPosition& ignore,
                                     UErrorCode& status) const;

    U_I18N_API static UnicodeString& format(const UnicodeString& pattern,
                                            const Formattable* arguments,
                                            int32_t count,
                                            UnicodeString& appendTo,
                                            UErrorCode& status);

    U_I18N_API virtual UnicodeString& format(const Formattable& obj,
                                             UnicodeString& appendTo,
                                             FieldPosition& pos,
                                             UErrorCode& status) const override;

    U_I18N_API UnicodeString& format(const UnicodeString* argumentNames,
                                     const Formattable* arguments,
                                     int32_t count,
                                     UnicodeString& appendTo,
                                     UErrorCode& status) const;
    U_I18N_API virtual Formattable* parse(const UnicodeString& source,
                                          ParsePosition& pos,
                                          int32_t& count) const;

    U_I18N_API virtual Formattable* parse(const UnicodeString& source,
                                          int32_t& count,
                                          UErrorCode& status) const;

    U_I18N_API virtual void parseObject(const UnicodeString& source,
                                        Formattable& result,
                                        ParsePosition& pos) const override;

    U_I18N_API static UnicodeString autoQuoteApostrophe(const UnicodeString& pattern, UErrorCode& status);

    U_I18N_API UBool usesNamedArguments() const;

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API int32_t getArgTypeCount() const;
#endif  /* U_HIDE_INTERNAL_API */

    U_I18N_API virtual UClassID getDynamicClassID() const override;

    U_I18N_API static UClassID getStaticClassID();

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API static UBool equalFormats(const void* left, const void* right);
#endif  /* U_HIDE_INTERNAL_API */

private:

    Locale              fLocale;
    MessagePattern      msgPattern;
    Format**            formatAliases; 
    int32_t             formatAliasesCapacity;

    MessageFormat() = delete; 

    class PluralSelectorProvider : public PluralFormat::PluralSelector {
    public:
        PluralSelectorProvider(const MessageFormat &mf, UPluralType type);
        virtual ~PluralSelectorProvider();
        virtual UnicodeString select(void *ctx, double number, UErrorCode& ec) const override;

        void reset();
    private:
        const MessageFormat &msgFormat;
        PluralRules* rules;
        UPluralType type;
    };

    Formattable::Type* argTypes;
    int32_t            argTypeCount;
    int32_t            argTypeCapacity;

    UBool hasArgTypeConflicts;

    UBool allocateArgTypes(int32_t capacity, UErrorCode& status);

    NumberFormat* defaultNumberFormat;
    DateFormat*   defaultDateFormat;

    UHashtable* cachedFormatters;
    UHashtable* customFormatArgStarts;

    PluralSelectorProvider pluralProvider;
    PluralSelectorProvider ordinalProvider;

    const NumberFormat* getDefaultNumberFormat(UErrorCode&) const;
    const DateFormat*   getDefaultDateFormat(UErrorCode&) const;

    static int32_t findKeyword( const UnicodeString& s,
                                const char16_t * const *list);

    UnicodeString& format(const Formattable* arguments,
                          const UnicodeString *argumentNames,
                          int32_t cnt,
                          UnicodeString& appendTo,
                          FieldPosition* pos,
                          UErrorCode& status) const;

    void format(int32_t msgStart,
                const void *plNumber,
                const Formattable* arguments,
                const UnicodeString *argumentNames,
                int32_t cnt,
                AppendableWrapper& appendTo,
                FieldPosition* pos,
                UErrorCode& success) const;

    UnicodeString getArgName(int32_t partIndex);

    void setArgStartFormat(int32_t argStart, Format* formatter, UErrorCode& status);

    void setCustomArgStartFormat(int32_t argStart, Format* formatter, UErrorCode& status);

    int32_t nextTopLevelArgStart(int32_t partIndex) const;

    UBool argNameMatches(int32_t partIndex, const UnicodeString& argName, int32_t argNumber);

    void cacheExplicitFormats(UErrorCode& status);

    Format* createAppropriateFormat(UnicodeString& type,
                                    UnicodeString& style,
                                    Formattable::Type& formattableType,
                                    UParseError& parseError,
                                    UErrorCode& ec);

    const Formattable* getArgFromListByName(const Formattable* arguments,
                                            const UnicodeString *argumentNames,
                                            int32_t cnt, UnicodeString& name) const;

    Formattable* parse(int32_t msgStart,
                       const UnicodeString& source,
                       ParsePosition& pos,
                       int32_t& count,
                       UErrorCode& ec) const;

    FieldPosition* updateMetaData(AppendableWrapper& dest, int32_t prevLength,
                                  FieldPosition* fp, const Formattable* argId) const;

    int32_t findOtherSubMessage(int32_t partIndex) const;

    int32_t findFirstPluralNumberArg(int32_t msgStart, const UnicodeString &argName) const;

    Format* getCachedFormatter(int32_t argumentNumber) const;

    UnicodeString getLiteralStringUntilNextArgument(int32_t from) const;

    void copyObjects(const MessageFormat& that, UErrorCode& ec);

    void formatComplexSubMessage(int32_t msgStart,
                                 const void *plNumber,
                                 const Formattable* arguments,
                                 const UnicodeString *argumentNames,
                                 int32_t cnt,
                                 AppendableWrapper& appendTo,
                                 UErrorCode& success) const;

    NumberFormat* createIntegerFormat(const Locale& locale, UErrorCode& status) const;

    const Formattable::Type* getArgTypeList(int32_t& listCount) const {
        listCount = argTypeCount;
        return argTypes;
    }

    void resetPattern();

    class DummyFormat : public Format {
    public:
        virtual bool operator==(const Format&) const override;
        virtual DummyFormat* clone() const override;
        virtual UnicodeString& format(const Formattable& obj,
                              UnicodeString& appendTo,
                              UErrorCode& status) const;
        virtual UnicodeString& format(const Formattable&,
                                      UnicodeString& appendTo,
                                      FieldPosition&,
                                      UErrorCode& status) const override;
        virtual UnicodeString& format(const Formattable& obj,
                                      UnicodeString& appendTo,
                                      FieldPositionIterator* posIter,
                                      UErrorCode& status) const override;
        virtual void parseObject(const UnicodeString&,
                                 Formattable&,
                                 ParsePosition&) const override;
    };

    friend class MessageFormatAdapter; 
};

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _MSGFMT
