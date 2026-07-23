// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING
#ifndef __NUMBER_PATTERNMODIFIER_H__
#define __NUMBER_PATTERNMODIFIER_H__

#include "standardplural.h"
#include "unicode/numberformatter.h"
#include "number_patternstring.h"
#include "number_types.h"
#include "number_modifiers.h"
#include "number_utils.h"
#include "number_currencysymbols.h"

U_NAMESPACE_BEGIN

namespace number::impl {

class MutablePatternModifier;

class U_I18N_API_CLASS ImmutablePatternModifier : public MicroPropsGenerator, public UMemory {
  public:
    ~ImmutablePatternModifier() override = default;

    void processQuantity(DecimalQuantity&, MicroProps& micros, UErrorCode& status) const override;

    U_I18N_API void applyToMicros(MicroProps& micros,
                                  const DecimalQuantity& quantity,
                                  UErrorCode& status) const;

    const Modifier* getModifier(Signum signum, StandardPlural::Form plural) const;

    void addToChain(const MicroPropsGenerator* parent);

  private:
    ImmutablePatternModifier(AdoptingModifierStore* pm, const PluralRules* rules);

    const LocalPointer<AdoptingModifierStore> pm;
    const PluralRules* rules;
    const MicroPropsGenerator* parent;

    friend class MutablePatternModifier;
};

class U_I18N_API_CLASS MutablePatternModifier
        : public MicroPropsGenerator,
          public Modifier,
          public SymbolProvider,
          public UMemory {
  public:

    ~MutablePatternModifier() override = default;

    U_I18N_API explicit MutablePatternModifier(bool isStrong);

    U_I18N_API void setPatternInfo(const AffixPatternProvider *patternInfo, Field field);

    U_I18N_API void setPatternAttributes(UNumberSignDisplay signDisplay, bool perMille,
                                         bool approximately);

    U_I18N_API void setSymbols(const DecimalFormatSymbols* symbols, const CurrencyUnit& currency,
                               UNumberUnitWidth unitWidth, const PluralRules* rules, UErrorCode& status);

    U_I18N_API void setNumberProperties(Signum signum, StandardPlural::Form plural);

    bool needsPlurals() const;

    U_I18N_API AdoptingSignumModifierStore createImmutableForPlural(StandardPlural::Form plural,
                                                                    UErrorCode& status);

    U_I18N_API ImmutablePatternModifier *createImmutable(UErrorCode &status);

    U_I18N_API MicroPropsGenerator &addToChain(const MicroPropsGenerator *parent);

    U_I18N_API void processQuantity(DecimalQuantity &, MicroProps &micros,
                                    UErrorCode &status) const override;

    U_I18N_API int32_t apply(FormattedStringBuilder &output, int32_t leftIndex, int32_t rightIndex,
                             UErrorCode &status) const override;

    int32_t getPrefixLength() const override;

    int32_t getCodePointCount() const override;

    bool isStrong() const override;

    bool containsField(Field field) const override;

    void getParameters(Parameters& output) const override;

    bool strictEquals(const Modifier& other) const override;

    UnicodeString getSymbol(AffixPatternType type) const override;

    UnicodeString getCurrencySymbolForUnitWidth(UErrorCode& status) const;

    UnicodeString toUnicodeString() const;

  private:
    const bool fStrong;

    const AffixPatternProvider *fPatternInfo;
    Field fField;
    UNumberSignDisplay fSignDisplay;
    bool fPerMilleReplacesPercent;
    bool fApproximately;

    const DecimalFormatSymbols *fSymbols;
    UNumberUnitWidth fUnitWidth;
    CurrencySymbols fCurrencySymbols;
    const PluralRules *fRules;

    Signum fSignum;
    StandardPlural::Form fPlural;

    const MicroPropsGenerator *fParent;

    UnicodeString currentAffix;

    ConstantMultiFieldModifier *createConstantModifier(UErrorCode &status);

    int32_t insertPrefix(FormattedStringBuilder &sb, int position, UErrorCode &status);

    int32_t insertSuffix(FormattedStringBuilder &sb, int position, UErrorCode &status);

    void prepareAffix(bool isPrefix);
};

} 

U_NAMESPACE_END

#endif //__NUMBER_PATTERNMODIFIER_H__

#endif /* #if !UCONFIG_NO_FORMATTING */
