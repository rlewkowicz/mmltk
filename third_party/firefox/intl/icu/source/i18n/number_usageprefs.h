// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING
#ifndef __NUMBER_USAGEPREFS_H__
#define __NUMBER_USAGEPREFS_H__

#include "cmemory.h"
#include "number_types.h"
#include "unicode/listformatter.h"
#include "unicode/localpointer.h"
#include "unicode/locid.h"
#include "unicode/measunit.h"
#include "unicode/stringpiece.h"
#include "unicode/uobject.h"
#include "units_converter.h"
#include "units_router.h"

U_NAMESPACE_BEGIN

using ::icu::units::ComplexUnitsConverter;
using ::icu::units::UnitsRouter;

namespace number::impl {

class UsagePrefsHandler : public MicroPropsGenerator, public UMemory {
  public:
    UsagePrefsHandler(const Locale &locale, const MeasureUnit &inputUnit, const StringPiece usage,
                      const MicroPropsGenerator *parent, UErrorCode &status);

    void processQuantity(DecimalQuantity &quantity, MicroProps &micros,
                         UErrorCode &status) const override;

    const MaybeStackVector<MeasureUnit> *getOutputUnits() const {
        return fUnitsRouter.getOutputUnits();
    }

  private:
    UnitsRouter fUnitsRouter;
    const MicroPropsGenerator *fParent;
};

class UnitConversionHandler : public MicroPropsGenerator, public UMemory {
  public:
    UnitConversionHandler(const MeasureUnit &targetUnit, const MicroPropsGenerator *parent,
                          UErrorCode &status);

    void processQuantity(DecimalQuantity &quantity, MicroProps &micros,
                         UErrorCode &status) const override;
  private:
    MeasureUnit fOutputUnit;
    LocalPointer<ComplexUnitsConverter> fUnitConverter;
    const MicroPropsGenerator *fParent;
};

} 

U_NAMESPACE_END

#endif // __NUMBER_USAGEPREFS_H__
#endif /* #if !UCONFIG_NO_FORMATTING */
