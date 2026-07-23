// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
* Copyright (C) 1997-2015, International Business Machines Corporation and others.
* All Rights Reserved.
*******************************************************************************
*/

#ifndef RBNF_H
#define RBNF_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API


#if UCONFIG_NO_FORMATTING
#define U_HAVE_RBNF 0
#else
#define U_HAVE_RBNF 1

#include "unicode/dcfmtsym.h"
#include "unicode/fmtable.h"
#include "unicode/locid.h"
#include "unicode/numfmt.h"
#include "unicode/unistr.h"
#include "unicode/strenum.h"
#include "unicode/brkiter.h"
#include "unicode/upluralrules.h"

U_NAMESPACE_BEGIN

class NFRule;
class NFRuleSet;
class LocalizationInfo;
class PluralFormat;
class RuleBasedCollator;

enum URBNFRuleSetTag {
    URBNF_SPELLOUT,
    URBNF_ORDINAL,
#ifndef U_HIDE_DEPRECATED_API
    URBNF_DURATION,
#endif // U_HIDE_DERECATED_API
    URBNF_NUMBERING_SYSTEM = 3,
#ifndef U_HIDE_DEPRECATED_API
    URBNF_COUNT
#endif  // U_HIDE_DEPRECATED_API
};

class U_I18N_API_CLASS RuleBasedNumberFormat : public NumberFormat {
public:


    U_I18N_API RuleBasedNumberFormat(const UnicodeString& rules,
                                     UParseError& perror,
                                     UErrorCode& status);

    U_I18N_API RuleBasedNumberFormat(const UnicodeString& rules,
                                     const UnicodeString& localizations,
                                     UParseError& perror,
                                     UErrorCode& status);

    U_I18N_API RuleBasedNumberFormat(const UnicodeString& rules,
                                     const Locale& locale,
                                     UParseError& perror,
                                     UErrorCode& status);

    U_I18N_API RuleBasedNumberFormat(const UnicodeString& rules,
                                     const UnicodeString& localizations,
                                     const Locale& locale,
                                     UParseError& perror,
                                     UErrorCode& status);

  U_I18N_API RuleBasedNumberFormat(URBNFRuleSetTag tag, const Locale& locale, UErrorCode& status);


  U_I18N_API RuleBasedNumberFormat(const RuleBasedNumberFormat& rhs);

  U_I18N_API RuleBasedNumberFormat& operator=(const RuleBasedNumberFormat& rhs);

  U_I18N_API virtual ~RuleBasedNumberFormat();

  U_I18N_API virtual RuleBasedNumberFormat* clone() const override;

  U_I18N_API virtual bool operator==(const Format& other) const override;


  U_I18N_API virtual UnicodeString getRules() const;

  U_I18N_API virtual int32_t getNumberOfRuleSetNames() const;

  U_I18N_API virtual UnicodeString getRuleSetName(int32_t index) const;

  U_I18N_API virtual int32_t getNumberOfRuleSetDisplayNameLocales() const;

  U_I18N_API virtual Locale getRuleSetDisplayNameLocale(int32_t index, UErrorCode& status) const;

    U_I18N_API virtual UnicodeString getRuleSetDisplayName(int32_t index,
                                                           const Locale& locale = Locale::getDefault());

    U_I18N_API virtual UnicodeString getRuleSetDisplayName(const UnicodeString& ruleSetName,
                                                           const Locale& locale = Locale::getDefault());


  using NumberFormat::format;

  U_I18N_API virtual UnicodeString& format(int32_t number,
                                           UnicodeString& toAppendTo,
                                           FieldPosition& pos) const override;

  U_I18N_API virtual UnicodeString& format(int64_t number,
                                           UnicodeString& toAppendTo,
                                           FieldPosition& pos) const override;
  U_I18N_API virtual UnicodeString& format(double number,
                                           UnicodeString& toAppendTo,
                                           FieldPosition& pos) const override;

  U_I18N_API virtual UnicodeString& format(int32_t number,
                                           const UnicodeString& ruleSetName,
                                           UnicodeString& toAppendTo,
                                           FieldPosition& pos,
                                           UErrorCode& status) const;
  U_I18N_API virtual UnicodeString& format(int64_t number,
                                           const UnicodeString& ruleSetName,
                                           UnicodeString& toAppendTo,
                                           FieldPosition& pos,
                                           UErrorCode& status) const;
  U_I18N_API virtual UnicodeString& format(double number,
                                           const UnicodeString& ruleSetName,
                                           UnicodeString& toAppendTo,
                                           FieldPosition& pos,
                                           UErrorCode& status) const;

protected:
    virtual UnicodeString& format(const number::impl::DecimalQuantity &number,
                                  UnicodeString& appendTo,
                                  FieldPosition& pos,
                                  UErrorCode& status) const override;
public:

  using NumberFormat::parse;

  U_I18N_API virtual void parse(const UnicodeString& text,
                                Formattable& result,
                                ParsePosition& parsePosition) const override;

#if !UCONFIG_NO_COLLATION

  U_I18N_API virtual void setLenient(UBool enabled) override;

  U_I18N_API virtual inline UBool isLenient() const override;

#endif

  U_I18N_API virtual void setDefaultRuleSet(const UnicodeString& ruleSetName, UErrorCode& status);

  U_I18N_API virtual UnicodeString getDefaultRuleSetName() const;

  U_I18N_API virtual void setContext(UDisplayContext value, UErrorCode& status) override;

    U_I18N_API virtual ERoundingMode getRoundingMode() const override;

    U_I18N_API virtual void setRoundingMode(ERoundingMode roundingMode) override;

public:
    U_I18N_API static UClassID getStaticClassID();

    U_I18N_API virtual UClassID getDynamicClassID() const override;

    U_I18N_API virtual void adoptDecimalFormatSymbols(DecimalFormatSymbols* symbolsToAdopt);

    U_I18N_API virtual void setDecimalFormatSymbols(const DecimalFormatSymbols& symbols);

private:
    RuleBasedNumberFormat() = delete; 

    RuleBasedNumberFormat(const UnicodeString& description, LocalizationInfo* localizations,
              const Locale& locale, UParseError& perror, UErrorCode& status);

    void init(const UnicodeString& rules, LocalizationInfo* localizations, UParseError& perror, UErrorCode& status);
    void initCapitalizationContextInfo(const Locale& thelocale);
    void dispose();
    void stripWhitespace(UnicodeString& src);
    void initDefaultRuleSet();
    NFRuleSet* findRuleSet(const UnicodeString& name, UErrorCode& status) const;

    friend class NFSubstitution;
    friend class NFRule;
    friend class NFRuleSet;
    friend class FractionalPartSubstitution;

    inline NFRuleSet * getDefaultRuleSet() const;
    const RuleBasedCollator * getCollator() const;
    DecimalFormatSymbols * initializeDecimalFormatSymbols(UErrorCode &status);
    const DecimalFormatSymbols * getDecimalFormatSymbols() const;
    NFRule * initializeDefaultInfinityRule(UErrorCode &status);
    const NFRule * getDefaultInfinityRule() const;
    NFRule * initializeDefaultNaNRule(UErrorCode &status);
    const NFRule * getDefaultNaNRule() const;
    PluralFormat *createPluralFormat(UPluralType pluralType, const UnicodeString &pattern, UErrorCode& status) const;
    UnicodeString& adjustForCapitalizationContext(int32_t startPos, UnicodeString& currentResult, UErrorCode& status) const;
    UnicodeString& format(int64_t number, NFRuleSet *ruleSet, UnicodeString& toAppendTo, UErrorCode& status) const;
    void format(double number, NFRuleSet& rs, UnicodeString& toAppendTo, UErrorCode& status) const;

private:
    NFRuleSet **fRuleSets;
    UnicodeString* ruleSetDescriptions;
    int32_t numRuleSets;
    NFRuleSet *defaultRuleSet;
    Locale locale;
    RuleBasedCollator* collator;
    DecimalFormatSymbols* decimalFormatSymbols;
    NFRule *defaultInfinityRule;
    NFRule *defaultNaNRule;
    ERoundingMode fRoundingMode;
    UBool lenient;
    UnicodeString* lenientParseRules;
    LocalizationInfo* localizations;
    UnicodeString originalDescription;
    UBool capitalizationInfoSet;
    UBool capitalizationForUIListMenu;
    UBool capitalizationForStandAlone;
    BreakIterator* capitalizationBrkIter;
};


#if !UCONFIG_NO_COLLATION

inline UBool
RuleBasedNumberFormat::isLenient() const {
    return lenient;
}

#endif

inline NFRuleSet*
RuleBasedNumberFormat::getDefaultRuleSet() const {
    return defaultRuleSet;
}

U_NAMESPACE_END

#endif

#endif /* U_SHOW_CPLUSPLUS_API */

#endif
