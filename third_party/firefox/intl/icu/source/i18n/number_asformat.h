// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING
#ifndef __NUMBER_ASFORMAT_H__
#define __NUMBER_ASFORMAT_H__

#include "unicode/numberformatter.h"
#include "number_types.h"
#include "number_decimalquantity.h"
#include "number_scientific.h"
#include "number_patternstring.h"
#include "number_modifiers.h"
#include "number_multiplier.h"
#include "number_roundingutils.h"
#include "decNumber.h"
#include "charstr.h"

U_NAMESPACE_BEGIN
namespace number::impl {

class U_I18N_API_CLASS LocalizedNumberFormatterAsFormat : public Format {
  public:
    U_I18N_API LocalizedNumberFormatterAsFormat(const LocalizedNumberFormatter& formatter,
                                                const Locale& locale);

    U_I18N_API ~LocalizedNumberFormatterAsFormat() override;

    U_I18N_API bool operator==(const Format& other) const override;

    U_I18N_API LocalizedNumberFormatterAsFormat* clone() const override;

    U_I18N_API UnicodeString& format(const Formattable& obj,
                                     UnicodeString& appendTo,
                                     FieldPosition& pos,
                                     UErrorCode& status) const override;

    U_I18N_API UnicodeString& format(const Formattable& obj,
                                     UnicodeString& appendTo,
                                     FieldPositionIterator* posIter,
                                     UErrorCode& status) const override;

    U_I18N_API void parseObject(const UnicodeString& source,
                                Formattable& result,
                                ParsePosition& parse_pos) const override;

    U_I18N_API const LocalizedNumberFormatter& getNumberFormatter() const;

    U_I18N_API UClassID getDynamicClassID() const override;
    U_I18N_API static UClassID getStaticClassID();

  private:
    LocalizedNumberFormatter fFormatter;

    Locale fLocale;
};

} 
U_NAMESPACE_END

#endif // __NUMBER_ASFORMAT_H__

#endif /* #if !UCONFIG_NO_FORMATTING */
