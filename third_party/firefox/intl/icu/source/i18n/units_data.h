// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING
#ifndef __UNITS_DATA_H__
#define __UNITS_DATA_H__

#include <limits>

#include "charstr.h"
#include "cmemory.h"
#include "fixedstring.h"
#include "unicode/stringpiece.h"
#include "unicode/uobject.h"

U_NAMESPACE_BEGIN
namespace units {

class ConversionRateInfo : public UMemory {
  public:
    ConversionRateInfo() {}
    ConversionRateInfo(StringPiece sourceUnit, StringPiece baseUnit, StringPiece factor,
                       StringPiece offset, UErrorCode &status)
        : sourceUnit(sourceUnit), baseUnit(baseUnit), factor(factor), offset(offset),
          specialMappingName(), systems() {
        if (this->sourceUnit.isEmpty() != sourceUnit.empty() ||
            this->baseUnit.isEmpty() != baseUnit.empty() ||
            this->factor.isEmpty() != factor.empty() ||
            this->offset.isEmpty() != offset.empty()) {
            status = U_MEMORY_ALLOCATION_ERROR;
        }
    }
    FixedString sourceUnit;
    FixedString baseUnit;
    FixedString factor;
    FixedString offset;
    FixedString specialMappingName; 
    FixedString systems;
};

void U_I18N_API getAllConversionRates(MaybeStackVector<ConversionRateInfo> &result, UErrorCode &status);

class ConversionRates {
  public:
    ConversionRates(UErrorCode &status) { getAllConversionRates(conversionInfo_, status); }

    const ConversionRateInfo *extractConversionInfo(StringPiece source, UErrorCode &status) const;

  private:
    MaybeStackVector<ConversionRateInfo> conversionInfo_;
};

struct UnitPreference : public UMemory {
    UnitPreference() : geq(1.0) {}
    FixedString unit;
    double geq;
    UnicodeString skeleton;

    UnitPreference(const UnitPreference &other) {
        this->unit = other.unit;
        this->geq = other.geq;
        this->skeleton = other.skeleton;
    }
};

class UnitPreferenceMetadata : public UMemory {
  public:
    UnitPreferenceMetadata() {}
    UnitPreferenceMetadata(StringPiece category, StringPiece usage, StringPiece region,
                           int32_t prefsOffset, int32_t prefsCount, UErrorCode &status);

    CharString category;
    CharString usage;
    CharString region;
    int32_t prefsOffset;
    int32_t prefsCount;

    int32_t compareTo(const UnitPreferenceMetadata &other) const;
    int32_t compareTo(const UnitPreferenceMetadata &other, bool *foundCategory, bool *foundUsage,
                      bool *foundRegion) const;
};

class U_I18N_API_CLASS UnitPreferences {
  public:
    U_I18N_API UnitPreferences(UErrorCode& status);

    U_I18N_API MaybeStackVector<UnitPreference> getPreferencesFor(StringPiece category,
                                                                  StringPiece usage,
                                                                  const Locale& locale,
                                                                  UErrorCode& status) const;

  protected:
    MaybeStackVector<UnitPreferenceMetadata> metadata_;
    MaybeStackVector<UnitPreference> unitPrefs_;
};

} 
U_NAMESPACE_END

#endif //__UNITS_DATA_H__

#endif /* #if !UCONFIG_NO_FORMATTING */
