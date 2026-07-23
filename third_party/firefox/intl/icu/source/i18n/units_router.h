// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING
#ifndef __UNITS_ROUTER_H__
#define __UNITS_ROUTER_H__

#include <limits>

#include "cmemory.h"
#include "measunit_impl.h"
#include "unicode/locid.h"
#include "unicode/measunit.h"
#include "unicode/stringpiece.h"
#include "unicode/uobject.h"
#include "units_complexconverter.h"
#include "units_data.h"

U_NAMESPACE_BEGIN

class Measure;
namespace number {
class Precision;
}

namespace units {

struct RouteResult : UMemory {
    MaybeStackVector<Measure> measures;

    MeasureUnitImpl outputUnit;

    RouteResult(MaybeStackVector<Measure> measures, MeasureUnitImpl outputUnit)
        : measures(std::move(measures)), outputUnit(std::move(outputUnit)) {}
};

struct ConverterPreference : UMemory {
    ComplexUnitsConverter converter;
    double limit;
    UnicodeString precision;

    MeasureUnitImpl targetUnit;

    ConverterPreference(const MeasureUnitImpl &source, const MeasureUnitImpl &complexTarget,
                        UnicodeString precision, const ConversionRates &ratesInfo, UErrorCode &status)
        : ConverterPreference(source, complexTarget, std::numeric_limits<double>::lowest(), precision,
                              ratesInfo, status) {}

    ConverterPreference(const MeasureUnitImpl &source, const MeasureUnitImpl &complexTarget,
                        double limit, UnicodeString precision, const ConversionRates &ratesInfo,
                        UErrorCode &status)
        : converter(source, complexTarget, ratesInfo, status), limit(limit),
          precision(std::move(precision)), targetUnit(complexTarget.copy(status)) {}
};

class U_I18N_API_CLASS UnitsRouter {
  public:
    U_I18N_API UnitsRouter(StringPiece inputUnitIdentifier, const Locale &locale, StringPiece usage,
                           UErrorCode &status);
    U_I18N_API UnitsRouter(const MeasureUnit &inputUnit, const Locale &locale, StringPiece usage,
                           UErrorCode &status);

    U_I18N_API RouteResult route(double quantity, icu::number::impl::RoundingImpl *rounder,
                                 UErrorCode &status) const;

    const MaybeStackVector<MeasureUnit> *getOutputUnits() const;

  private:
    MaybeStackVector<MeasureUnit> outputUnits_;

    MaybeStackVector<ConverterPreference> converterPreferences_;

    static number::Precision parseSkeletonToPrecision(icu::UnicodeString precisionSkeleton,
                                                      UErrorCode &status);

    void init(const MeasureUnit &inputUnit, const Locale &locale, StringPiece usage, UErrorCode &status);
};

} 
U_NAMESPACE_END

#endif //__UNITS_ROUTER_H__

#endif /* #if !UCONFIG_NO_FORMATTING */
