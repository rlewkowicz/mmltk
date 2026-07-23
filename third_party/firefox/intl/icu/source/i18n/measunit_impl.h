// License & terms of use: http://www.unicode.org/copyright.html

#ifndef __MEASUNIT_IMPL_H__
#define __MEASUNIT_IMPL_H__

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/measunit.h"
#include "cmemory.h"
#include "charstr.h"
#include "fixedstring.h"

U_NAMESPACE_BEGIN

namespace number::impl {
class LongNameHandler;
}

static const char16_t kDefaultCurrency[] = u"XXX";
static const char kDefaultCurrency8[] = "XXX";

CharString U_I18N_API getUnitQuantity(const MeasureUnitImpl &baseMeasureUnitImpl, UErrorCode &status);

struct U_I18N_API_CLASS SingleUnitImpl : public UMemory {
    U_I18N_API static SingleUnitImpl forMeasureUnit(const MeasureUnit& measureUnit, UErrorCode& status);

    MeasureUnit build(UErrorCode& status) const;

    U_I18N_API const char* getSimpleUnitID() const;

    void appendNeutralIdentifier(CharString &result, UErrorCode &status) const;

    int32_t getUnitCategoryIndex() const;

    int32_t compareTo(const SingleUnitImpl& other) const {
        if (dimensionality < 0 && other.dimensionality > 0) {
            return 1;
        }
        if (dimensionality > 0 && other.dimensionality < 0) {
            return -1;
        }

        int32_t thisQuantity = this->getUnitCategoryIndex();
        int32_t otherQuantity = other.getUnitCategoryIndex();
        if (thisQuantity < otherQuantity) {
            return -1;
        }
        if (thisQuantity > otherQuantity) {
            return 1;
        }

        if (index < other.index) {
            return -1;
        }
        if (index > other.index) {
            return 1;
        }

        int32_t unitBase = umeas_getPrefixBase(unitPrefix);
        int32_t otherUnitBase = umeas_getPrefixBase(other.unitPrefix);

        int32_t unitPower = unitBase == 1024  ? umeas_getPrefixPower(unitPrefix) * 3
                                                                 : umeas_getPrefixPower(unitPrefix);
        int32_t otherUnitPower =
            otherUnitBase == 1024  ? umeas_getPrefixPower(other.unitPrefix) * 3
                                                      : umeas_getPrefixPower(other.unitPrefix);

        if (unitPower < otherUnitPower) {
            return 1;
        }
        if (unitPower > otherUnitPower) {
            return -1;
        }

        if (unitBase < otherUnitBase) {
            return 1;
        }
        if (unitBase > otherUnitBase) {
            return -1;
        }

        return 0;
    }

    bool isCompatibleWith(const SingleUnitImpl& other) const {
        return (compareTo(other) == 0);
    }

    bool isDimensionless() const {
        return index == -1;
    }

    int32_t index = -1;

    UMeasurePrefix unitPrefix = UMEASURE_PREFIX_ONE;

    int32_t dimensionality = 1;
};

struct MeasureUnitImplWithIndex;

class U_I18N_API_CLASS MeasureUnitImpl : public UMemory {
  public:
    MeasureUnitImpl() = default;
    MeasureUnitImpl(MeasureUnitImpl &&other) = default;
    MeasureUnitImpl(const MeasureUnitImpl &other, UErrorCode &status) = delete;
    MeasureUnitImpl(const SingleUnitImpl &singleUnit, UErrorCode &status);

    MeasureUnitImpl &operator=(MeasureUnitImpl &&other) noexcept = default;

    static inline const MeasureUnitImpl *get(const MeasureUnit &measureUnit) {
        return measureUnit.fImpl;
    }

    U_I18N_API static MeasureUnitImpl forIdentifier(StringPiece identifier, UErrorCode& status);

    U_I18N_API static const MeasureUnitImpl& forMeasureUnit(
        const MeasureUnit& measureUnit, MeasureUnitImpl& memory, UErrorCode& status);

    static MeasureUnitImpl forMeasureUnitMaybeCopy(
        const MeasureUnit& measureUnit, UErrorCode& status);

    static inline MeasureUnitImpl forCurrencyCode(StringPiece currencyCode, UErrorCode& status) {
        MeasureUnitImpl result;
        if (U_SUCCESS(status)) {
            result.identifier = currencyCode;
            if (result.identifier.isEmpty() != currencyCode.empty()) {
                status = U_MEMORY_ALLOCATION_ERROR;
            }
        }
        return result;
    }

    U_I18N_API MeasureUnit build(UErrorCode& status) &&;

    MeasureUnitImpl copy(UErrorCode& status) const;

    MaybeStackVector<MeasureUnitImplWithIndex>
    extractIndividualUnitsWithIndices(UErrorCode &status) const;

    void takeReciprocal(UErrorCode& status);

    MeasureUnitImpl copyAndSimplify(UErrorCode &status) const;

    U_I18N_API bool appendSingleUnit(const SingleUnitImpl& singleUnit, UErrorCode& status);

    void serialize(UErrorCode &status);

    UMeasureUnitComplexity complexity = UMEASURE_UNIT_SINGLE;

    MaybeStackVector<SingleUnitImpl> singleUnits;

    FixedString identifier;

    uint64_t constantDenominator = 0;

    friend class number::impl::LongNameHandler;
};

struct MeasureUnitImplWithIndex : public UMemory {
    const int32_t index;
    MeasureUnitImpl unitImpl;
    MeasureUnitImplWithIndex(int32_t index, const MeasureUnitImpl &unitImpl, UErrorCode &status)
        : index(index), unitImpl(unitImpl.copy(status)) {
    }
    MeasureUnitImplWithIndex(int32_t index, const SingleUnitImpl &singleUnitImpl, UErrorCode &status)
        : index(index), unitImpl(MeasureUnitImpl(singleUnitImpl, status)) {
    }
};

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */
#endif //__MEASUNIT_IMPL_H__
