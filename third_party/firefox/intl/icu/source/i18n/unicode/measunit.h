// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2004-2016, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: April 26, 2004
* Since: ICU 3.0
**********************************************************************
*/
#ifndef __MEASUREUNIT_H__
#define __MEASUREUNIT_H__

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#if !UCONFIG_NO_FORMATTING

#include <utility>
#include "unicode/unistr.h"
#include "unicode/localpointer.h"


U_NAMESPACE_BEGIN

class StringEnumeration;
class MeasureUnitImpl;

namespace number::impl {
class LongNameHandler;
} 

enum UMeasureUnitComplexity {
    UMEASURE_UNIT_SINGLE,

    UMEASURE_UNIT_COMPOUND,

    UMEASURE_UNIT_MIXED
};


typedef enum UMeasurePrefix {
    UMEASURE_PREFIX_ONE = 30 + 0,

    UMEASURE_PREFIX_YOTTA = UMEASURE_PREFIX_ONE + 24,

    UMEASURE_PREFIX_RONNA = UMEASURE_PREFIX_ONE + 27,

    UMEASURE_PREFIX_QUETTA = UMEASURE_PREFIX_ONE + 30,

#ifndef U_HIDE_INTERNAL_API
#ifndef U_HIDE_DRAFT_API
    UMEASURE_PREFIX_INTERNAL_MAX_SI = UMEASURE_PREFIX_QUETTA,
#else  /* U_HIDE_DRAFT_API */
    UMEASURE_PREFIX_INTERNAL_MAX_SI = UMEASURE_PREFIX_YOTTA,
#endif  /* U_HIDE_DRAFT_API */

#endif  /* U_HIDE_INTERNAL_API */

    UMEASURE_PREFIX_ZETTA = UMEASURE_PREFIX_ONE + 21,

    UMEASURE_PREFIX_EXA = UMEASURE_PREFIX_ONE + 18,

    UMEASURE_PREFIX_PETA = UMEASURE_PREFIX_ONE + 15,

    UMEASURE_PREFIX_TERA = UMEASURE_PREFIX_ONE + 12,

    UMEASURE_PREFIX_GIGA = UMEASURE_PREFIX_ONE + 9,

    UMEASURE_PREFIX_MEGA = UMEASURE_PREFIX_ONE + 6,

    UMEASURE_PREFIX_KILO = UMEASURE_PREFIX_ONE + 3,

    UMEASURE_PREFIX_HECTO = UMEASURE_PREFIX_ONE + 2,

    UMEASURE_PREFIX_DEKA = UMEASURE_PREFIX_ONE + 1,

    UMEASURE_PREFIX_DECI = UMEASURE_PREFIX_ONE + -1,

    UMEASURE_PREFIX_CENTI = UMEASURE_PREFIX_ONE + -2,

    UMEASURE_PREFIX_MILLI = UMEASURE_PREFIX_ONE + -3,

    UMEASURE_PREFIX_MICRO = UMEASURE_PREFIX_ONE + -6,

    UMEASURE_PREFIX_NANO = UMEASURE_PREFIX_ONE + -9,

    UMEASURE_PREFIX_PICO = UMEASURE_PREFIX_ONE + -12,

    UMEASURE_PREFIX_FEMTO = UMEASURE_PREFIX_ONE + -15,

    UMEASURE_PREFIX_ATTO = UMEASURE_PREFIX_ONE + -18,

    UMEASURE_PREFIX_ZEPTO = UMEASURE_PREFIX_ONE + -21,

    UMEASURE_PREFIX_YOCTO = UMEASURE_PREFIX_ONE + -24,

    UMEASURE_PREFIX_RONTO = UMEASURE_PREFIX_ONE + -27,

    UMEASURE_PREFIX_QUECTO = UMEASURE_PREFIX_ONE + -30,

#ifndef U_HIDE_INTERNAL_API
#ifndef U_HIDE_DRAFT_API
    UMEASURE_PREFIX_INTERNAL_MIN_SI = UMEASURE_PREFIX_QUECTO,
#else  /* U_HIDE_DRAFT_API */
    UMEASURE_PREFIX_INTERNAL_MIN_SI = UMEASURE_PREFIX_YOCTO,
#endif  /* U_HIDE_DRAFT_API */

#endif  // U_HIDE_INTERNAL_API

    UMEASURE_PREFIX_INTERNAL_ONE_BIN = -60,

    UMEASURE_PREFIX_KIBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 1,

#ifndef U_HIDE_INTERNAL_API
    UMEASURE_PREFIX_INTERNAL_MIN_BIN = UMEASURE_PREFIX_KIBI,
#endif  // U_HIDE_INTERNAL_API

    UMEASURE_PREFIX_MEBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 2,

    UMEASURE_PREFIX_GIBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 3,

    UMEASURE_PREFIX_TEBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 4,

    UMEASURE_PREFIX_PEBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 5,

    UMEASURE_PREFIX_EXBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 6,

    UMEASURE_PREFIX_ZEBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 7,

    UMEASURE_PREFIX_YOBI = UMEASURE_PREFIX_INTERNAL_ONE_BIN + 8,

#ifndef U_HIDE_INTERNAL_API
    UMEASURE_PREFIX_INTERNAL_MAX_BIN = UMEASURE_PREFIX_YOBI,
#endif  // U_HIDE_INTERNAL_API
} UMeasurePrefix;

U_CAPI int32_t U_EXPORT2 umeas_getPrefixBase(UMeasurePrefix unitPrefix);

U_CAPI int32_t U_EXPORT2 umeas_getPrefixPower(UMeasurePrefix unitPrefix);

class U_I18N_API MeasureUnit: public UObject {
 public:

    MeasureUnit();

    MeasureUnit(const MeasureUnit &other);

    MeasureUnit(MeasureUnit &&other) noexcept;

    static MeasureUnit forIdentifier(StringPiece identifier, UErrorCode& status);

    MeasureUnit &operator=(const MeasureUnit &other);

    MeasureUnit &operator=(MeasureUnit &&other) noexcept;

    virtual MeasureUnit* clone() const;

    virtual ~MeasureUnit();

    virtual bool operator==(const UObject& other) const;

    bool operator!=(const UObject& other) const {
        return !(*this == other);
    }

    const char *getType() const;

    const char *getSubtype() const;

    const char* getIdentifier() const;

    UMeasureUnitComplexity getComplexity(UErrorCode& status) const;

    MeasureUnit withPrefix(UMeasurePrefix prefix, UErrorCode& status) const;

    UMeasurePrefix getPrefix(UErrorCode& status) const;

#ifndef U_HIDE_DRAFT_API

    MeasureUnit withConstantDenominator(uint64_t denominator, UErrorCode &status) const;

    uint64_t getConstantDenominator(UErrorCode &status) const;

#endif /* U_HIDE_DRAFT_API */

    MeasureUnit withDimensionality(int32_t dimensionality, UErrorCode& status) const;

    int32_t getDimensionality(UErrorCode& status) const;

    MeasureUnit reciprocal(UErrorCode& status) const;

    MeasureUnit product(const MeasureUnit& other, UErrorCode& status) const;

    inline std::pair<LocalArray<MeasureUnit>, int32_t> splitToSingleUnits(UErrorCode& status) const;

    static int32_t getAvailable(
            MeasureUnit *destArray,
            int32_t destCapacity,
            UErrorCode &errorCode);

    static int32_t getAvailable(
            const char *type,
            MeasureUnit *destArray,
            int32_t destCapacity,
            UErrorCode &errorCode);

    static StringEnumeration* getAvailableTypes(UErrorCode &errorCode);

    static UClassID U_EXPORT2 getStaticClassID();

    virtual UClassID getDynamicClassID() const override;

#ifndef U_HIDE_INTERNAL_API
    int32_t getOffset() const;
#endif /* U_HIDE_INTERNAL_API */


    static MeasureUnit *createGForce(UErrorCode &status);

    static MeasureUnit getGForce();

    static MeasureUnit *createMeterPerSecondSquared(UErrorCode &status);

    static MeasureUnit getMeterPerSecondSquared();

    static MeasureUnit *createArcMinute(UErrorCode &status);

    static MeasureUnit getArcMinute();

    static MeasureUnit *createArcSecond(UErrorCode &status);

    static MeasureUnit getArcSecond();

    static MeasureUnit *createDegree(UErrorCode &status);

    static MeasureUnit getDegree();

    static MeasureUnit *createRadian(UErrorCode &status);

    static MeasureUnit getRadian();

    static MeasureUnit *createRevolutionAngle(UErrorCode &status);

    static MeasureUnit getRevolutionAngle();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createSteradian(UErrorCode &status);

    static MeasureUnit getSteradian();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createAcre(UErrorCode &status);

    static MeasureUnit getAcre();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createBuJp(UErrorCode &status);

    static MeasureUnit getBuJp();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createCho(UErrorCode &status);

    static MeasureUnit getCho();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createDunam(UErrorCode &status);

    static MeasureUnit getDunam();

    static MeasureUnit *createHectare(UErrorCode &status);

    static MeasureUnit getHectare();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createSeJp(UErrorCode &status);

    static MeasureUnit getSeJp();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createSquareCentimeter(UErrorCode &status);

    static MeasureUnit getSquareCentimeter();

    static MeasureUnit *createSquareFoot(UErrorCode &status);

    static MeasureUnit getSquareFoot();

    static MeasureUnit *createSquareInch(UErrorCode &status);

    static MeasureUnit getSquareInch();

    static MeasureUnit *createSquareKilometer(UErrorCode &status);

    static MeasureUnit getSquareKilometer();

    static MeasureUnit *createSquareMeter(UErrorCode &status);

    static MeasureUnit getSquareMeter();

    static MeasureUnit *createSquareMile(UErrorCode &status);

    static MeasureUnit getSquareMile();

    static MeasureUnit *createSquareYard(UErrorCode &status);

    static MeasureUnit getSquareYard();

    static MeasureUnit *createItem(UErrorCode &status);

    static MeasureUnit getItem();

    static MeasureUnit *createKarat(UErrorCode &status);

    static MeasureUnit getKarat();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createKatal(UErrorCode &status);

    static MeasureUnit getKatal();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createMilligramOfglucosePerDeciliter(UErrorCode &status);

    static MeasureUnit getMilligramOfglucosePerDeciliter();

#ifndef U_HIDE_DEPRECATED_API
    static MeasureUnit *createMilligramPerDeciliter(UErrorCode &status);

    static MeasureUnit getMilligramPerDeciliter();
#endif  /* U_HIDE_DEPRECATED_API */

    static MeasureUnit *createMillimolePerLiter(UErrorCode &status);

    static MeasureUnit getMillimolePerLiter();

    static MeasureUnit *createMole(UErrorCode &status);

    static MeasureUnit getMole();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createOfglucose(UErrorCode &status);

    static MeasureUnit getOfglucose();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createPart(UErrorCode &status);

    static MeasureUnit getPart();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createPartPer1E6(UErrorCode &status);

    static MeasureUnit getPartPer1E6();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createPartPerMillion(UErrorCode &status);

    static MeasureUnit getPartPerMillion();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createPartPer1E9(UErrorCode &status);

    static MeasureUnit getPartPer1E9();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createPercent(UErrorCode &status);

    static MeasureUnit getPercent();

    static MeasureUnit *createPermille(UErrorCode &status);

    static MeasureUnit getPermille();

    static MeasureUnit *createPermyriad(UErrorCode &status);

    static MeasureUnit getPermyriad();

    static MeasureUnit *createLiterPer100Kilometers(UErrorCode &status);

    static MeasureUnit getLiterPer100Kilometers();

    static MeasureUnit *createLiterPerKilometer(UErrorCode &status);

    static MeasureUnit getLiterPerKilometer();

    static MeasureUnit *createMilePerGallon(UErrorCode &status);

    static MeasureUnit getMilePerGallon();

    static MeasureUnit *createMilePerGallonImperial(UErrorCode &status);

    static MeasureUnit getMilePerGallonImperial();

    static MeasureUnit *createBit(UErrorCode &status);

    static MeasureUnit getBit();

    static MeasureUnit *createByte(UErrorCode &status);

    static MeasureUnit getByte();

    static MeasureUnit *createGigabit(UErrorCode &status);

    static MeasureUnit getGigabit();

    static MeasureUnit *createGigabyte(UErrorCode &status);

    static MeasureUnit getGigabyte();

    static MeasureUnit *createKilobit(UErrorCode &status);

    static MeasureUnit getKilobit();

    static MeasureUnit *createKilobyte(UErrorCode &status);

    static MeasureUnit getKilobyte();

    static MeasureUnit *createMegabit(UErrorCode &status);

    static MeasureUnit getMegabit();

    static MeasureUnit *createMegabyte(UErrorCode &status);

    static MeasureUnit getMegabyte();

    static MeasureUnit *createPetabyte(UErrorCode &status);

    static MeasureUnit getPetabyte();

    static MeasureUnit *createTerabit(UErrorCode &status);

    static MeasureUnit getTerabit();

    static MeasureUnit *createTerabyte(UErrorCode &status);

    static MeasureUnit getTerabyte();

    static MeasureUnit *createCentury(UErrorCode &status);

    static MeasureUnit getCentury();

    static MeasureUnit *createDay(UErrorCode &status);

    static MeasureUnit getDay();

    static MeasureUnit *createDayPerson(UErrorCode &status);

    static MeasureUnit getDayPerson();

    static MeasureUnit *createDecade(UErrorCode &status);

    static MeasureUnit getDecade();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createFortnight(UErrorCode &status);

    static MeasureUnit getFortnight();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createHour(UErrorCode &status);

    static MeasureUnit getHour();

    static MeasureUnit *createMicrosecond(UErrorCode &status);

    static MeasureUnit getMicrosecond();

    static MeasureUnit *createMillisecond(UErrorCode &status);

    static MeasureUnit getMillisecond();

    static MeasureUnit *createMinute(UErrorCode &status);

    static MeasureUnit getMinute();

    static MeasureUnit *createMonth(UErrorCode &status);

    static MeasureUnit getMonth();

    static MeasureUnit *createMonthPerson(UErrorCode &status);

    static MeasureUnit getMonthPerson();

    static MeasureUnit *createNanosecond(UErrorCode &status);

    static MeasureUnit getNanosecond();

    static MeasureUnit *createNight(UErrorCode &status);

    static MeasureUnit getNight();

    static MeasureUnit *createQuarter(UErrorCode &status);

    static MeasureUnit getQuarter();

    static MeasureUnit *createSecond(UErrorCode &status);

    static MeasureUnit getSecond();

    static MeasureUnit *createWeek(UErrorCode &status);

    static MeasureUnit getWeek();

    static MeasureUnit *createWeekPerson(UErrorCode &status);

    static MeasureUnit getWeekPerson();

    static MeasureUnit *createYear(UErrorCode &status);

    static MeasureUnit getYear();

    static MeasureUnit *createYearPerson(UErrorCode &status);

    static MeasureUnit getYearPerson();

    static MeasureUnit *createAmpere(UErrorCode &status);

    static MeasureUnit getAmpere();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createCoulomb(UErrorCode &status);

    static MeasureUnit getCoulomb();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createFarad(UErrorCode &status);

    static MeasureUnit getFarad();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createHenry(UErrorCode &status);

    static MeasureUnit getHenry();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createMilliampere(UErrorCode &status);

    static MeasureUnit getMilliampere();

    static MeasureUnit *createOhm(UErrorCode &status);

    static MeasureUnit getOhm();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createSiemens(UErrorCode &status);

    static MeasureUnit getSiemens();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createVolt(UErrorCode &status);

    static MeasureUnit getVolt();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createBecquerel(UErrorCode &status);

    static MeasureUnit getBecquerel();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createBritishThermalUnit(UErrorCode &status);

    static MeasureUnit getBritishThermalUnit();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createBritishThermalUnitIt(UErrorCode &status);

    static MeasureUnit getBritishThermalUnitIt();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createCalorie(UErrorCode &status);

    static MeasureUnit getCalorie();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createCalorieIt(UErrorCode &status);

    static MeasureUnit getCalorieIt();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createElectronvolt(UErrorCode &status);

    static MeasureUnit getElectronvolt();

    static MeasureUnit *createFoodcalorie(UErrorCode &status);

    static MeasureUnit getFoodcalorie();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createGray(UErrorCode &status);

    static MeasureUnit getGray();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createJoule(UErrorCode &status);

    static MeasureUnit getJoule();

    static MeasureUnit *createKilocalorie(UErrorCode &status);

    static MeasureUnit getKilocalorie();

    static MeasureUnit *createKilojoule(UErrorCode &status);

    static MeasureUnit getKilojoule();

    static MeasureUnit *createKilowattHour(UErrorCode &status);

    static MeasureUnit getKilowattHour();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createSievert(UErrorCode &status);

    static MeasureUnit getSievert();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createThermUs(UErrorCode &status);

    static MeasureUnit getThermUs();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createKilogramForce(UErrorCode &status);

    static MeasureUnit getKilogramForce();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createKilowattHourPer100Kilometer(UErrorCode &status);

    static MeasureUnit getKilowattHourPer100Kilometer();

    static MeasureUnit *createNewton(UErrorCode &status);

    static MeasureUnit getNewton();

    static MeasureUnit *createPoundForce(UErrorCode &status);

    static MeasureUnit getPoundForce();

    static MeasureUnit *createGigahertz(UErrorCode &status);

    static MeasureUnit getGigahertz();

    static MeasureUnit *createHertz(UErrorCode &status);

    static MeasureUnit getHertz();

    static MeasureUnit *createKilohertz(UErrorCode &status);

    static MeasureUnit getKilohertz();

    static MeasureUnit *createMegahertz(UErrorCode &status);

    static MeasureUnit getMegahertz();

    static MeasureUnit *createDot(UErrorCode &status);

    static MeasureUnit getDot();

    static MeasureUnit *createDotPerCentimeter(UErrorCode &status);

    static MeasureUnit getDotPerCentimeter();

    static MeasureUnit *createDotPerInch(UErrorCode &status);

    static MeasureUnit getDotPerInch();

    static MeasureUnit *createEm(UErrorCode &status);

    static MeasureUnit getEm();

    static MeasureUnit *createMegapixel(UErrorCode &status);

    static MeasureUnit getMegapixel();

    static MeasureUnit *createPixel(UErrorCode &status);

    static MeasureUnit getPixel();

    static MeasureUnit *createPixelPerCentimeter(UErrorCode &status);

    static MeasureUnit getPixelPerCentimeter();

    static MeasureUnit *createPixelPerInch(UErrorCode &status);

    static MeasureUnit getPixelPerInch();

    static MeasureUnit *createAstronomicalUnit(UErrorCode &status);

    static MeasureUnit getAstronomicalUnit();

    static MeasureUnit *createCentimeter(UErrorCode &status);

    static MeasureUnit getCentimeter();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createChain(UErrorCode &status);

    static MeasureUnit getChain();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createDecimeter(UErrorCode &status);

    static MeasureUnit getDecimeter();

    static MeasureUnit *createEarthRadius(UErrorCode &status);

    static MeasureUnit getEarthRadius();

    static MeasureUnit *createFathom(UErrorCode &status);

    static MeasureUnit getFathom();

    static MeasureUnit *createFoot(UErrorCode &status);

    static MeasureUnit getFoot();

    static MeasureUnit *createFurlong(UErrorCode &status);

    static MeasureUnit getFurlong();

    static MeasureUnit *createInch(UErrorCode &status);

    static MeasureUnit getInch();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createJoJp(UErrorCode &status);

    static MeasureUnit getJoJp();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createKen(UErrorCode &status);

    static MeasureUnit getKen();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createKilometer(UErrorCode &status);

    static MeasureUnit getKilometer();

    static MeasureUnit *createLightYear(UErrorCode &status);

    static MeasureUnit getLightYear();

    static MeasureUnit *createMeter(UErrorCode &status);

    static MeasureUnit getMeter();

    static MeasureUnit *createMicrometer(UErrorCode &status);

    static MeasureUnit getMicrometer();

    static MeasureUnit *createMile(UErrorCode &status);

    static MeasureUnit getMile();

    static MeasureUnit *createMileScandinavian(UErrorCode &status);

    static MeasureUnit getMileScandinavian();

    static MeasureUnit *createMillimeter(UErrorCode &status);

    static MeasureUnit getMillimeter();

    static MeasureUnit *createNanometer(UErrorCode &status);

    static MeasureUnit getNanometer();

    static MeasureUnit *createNauticalMile(UErrorCode &status);

    static MeasureUnit getNauticalMile();

    static MeasureUnit *createParsec(UErrorCode &status);

    static MeasureUnit getParsec();

    static MeasureUnit *createPicometer(UErrorCode &status);

    static MeasureUnit getPicometer();

    static MeasureUnit *createPoint(UErrorCode &status);

    static MeasureUnit getPoint();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createRiJp(UErrorCode &status);

    static MeasureUnit getRiJp();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createRin(UErrorCode &status);

    static MeasureUnit getRin();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createRod(UErrorCode &status);

    static MeasureUnit getRod();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createShakuCloth(UErrorCode &status);

    static MeasureUnit getShakuCloth();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createShakuLength(UErrorCode &status);

    static MeasureUnit getShakuLength();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createSolarRadius(UErrorCode &status);

    static MeasureUnit getSolarRadius();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createSun(UErrorCode &status);

    static MeasureUnit getSun();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createYard(UErrorCode &status);

    static MeasureUnit getYard();

    static MeasureUnit *createCandela(UErrorCode &status);

    static MeasureUnit getCandela();

    static MeasureUnit *createLumen(UErrorCode &status);

    static MeasureUnit getLumen();

    static MeasureUnit *createLux(UErrorCode &status);

    static MeasureUnit getLux();

    static MeasureUnit *createSolarLuminosity(UErrorCode &status);

    static MeasureUnit getSolarLuminosity();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createTesla(UErrorCode &status);

    static MeasureUnit getTesla();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createWeber(UErrorCode &status);

    static MeasureUnit getWeber();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createCarat(UErrorCode &status);

    static MeasureUnit getCarat();

    static MeasureUnit *createDalton(UErrorCode &status);

    static MeasureUnit getDalton();

    static MeasureUnit *createEarthMass(UErrorCode &status);

    static MeasureUnit getEarthMass();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createFun(UErrorCode &status);

    static MeasureUnit getFun();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createGrain(UErrorCode &status);

    static MeasureUnit getGrain();

    static MeasureUnit *createGram(UErrorCode &status);

    static MeasureUnit getGram();

    static MeasureUnit *createKilogram(UErrorCode &status);

    static MeasureUnit getKilogram();

    static MeasureUnit *createMicrogram(UErrorCode &status);

    static MeasureUnit getMicrogram();

    static MeasureUnit *createMilligram(UErrorCode &status);

    static MeasureUnit getMilligram();

    static MeasureUnit *createOunce(UErrorCode &status);

    static MeasureUnit getOunce();

    static MeasureUnit *createOunceTroy(UErrorCode &status);

    static MeasureUnit getOunceTroy();

    static MeasureUnit *createPound(UErrorCode &status);

    static MeasureUnit getPound();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createSlug(UErrorCode &status);

    static MeasureUnit getSlug();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createSolarMass(UErrorCode &status);

    static MeasureUnit getSolarMass();

    static MeasureUnit *createStone(UErrorCode &status);

    static MeasureUnit getStone();

    static MeasureUnit *createTon(UErrorCode &status);

    static MeasureUnit getTon();

    static MeasureUnit *createTonne(UErrorCode &status);

    static MeasureUnit getTonne();

#ifndef U_HIDE_DEPRECATED_API
    static MeasureUnit *createMetricTon(UErrorCode &status);

    static MeasureUnit getMetricTon();
#endif  /* U_HIDE_DEPRECATED_API */

    static MeasureUnit *createGigawatt(UErrorCode &status);

    static MeasureUnit getGigawatt();

    static MeasureUnit *createHorsepower(UErrorCode &status);

    static MeasureUnit getHorsepower();

    static MeasureUnit *createKilowatt(UErrorCode &status);

    static MeasureUnit getKilowatt();

    static MeasureUnit *createMegawatt(UErrorCode &status);

    static MeasureUnit getMegawatt();

    static MeasureUnit *createMilliwatt(UErrorCode &status);

    static MeasureUnit getMilliwatt();

    static MeasureUnit *createWatt(UErrorCode &status);

    static MeasureUnit getWatt();

    static MeasureUnit *createAtmosphere(UErrorCode &status);

    static MeasureUnit getAtmosphere();

    static MeasureUnit *createBar(UErrorCode &status);

    static MeasureUnit getBar();

    static MeasureUnit *createGasolineEnergyDensity(UErrorCode &status);

    static MeasureUnit getGasolineEnergyDensity();

    static MeasureUnit *createHectopascal(UErrorCode &status);

    static MeasureUnit getHectopascal();

    static MeasureUnit *createInchHg(UErrorCode &status);

    static MeasureUnit getInchHg();

    static MeasureUnit *createKilopascal(UErrorCode &status);

    static MeasureUnit getKilopascal();

    static MeasureUnit *createMegapascal(UErrorCode &status);

    static MeasureUnit getMegapascal();

    static MeasureUnit *createMillibar(UErrorCode &status);

    static MeasureUnit getMillibar();

    static MeasureUnit *createMillimeterOfMercury(UErrorCode &status);

    static MeasureUnit getMillimeterOfMercury();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createOfhg(UErrorCode &status);

    static MeasureUnit getOfhg();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createPascal(UErrorCode &status);

    static MeasureUnit getPascal();

    static MeasureUnit *createPoundPerSquareInch(UErrorCode &status);

    static MeasureUnit getPoundPerSquareInch();

    static MeasureUnit *createBeaufort(UErrorCode &status);

    static MeasureUnit getBeaufort();

    static MeasureUnit *createKilometerPerHour(UErrorCode &status);

    static MeasureUnit getKilometerPerHour();

    static MeasureUnit *createKnot(UErrorCode &status);

    static MeasureUnit getKnot();

    static MeasureUnit *createLightSpeed(UErrorCode &status);

    static MeasureUnit getLightSpeed();

    static MeasureUnit *createMeterPerSecond(UErrorCode &status);

    static MeasureUnit getMeterPerSecond();

    static MeasureUnit *createMilePerHour(UErrorCode &status);

    static MeasureUnit getMilePerHour();

    static MeasureUnit *createCelsius(UErrorCode &status);

    static MeasureUnit getCelsius();

    static MeasureUnit *createFahrenheit(UErrorCode &status);

    static MeasureUnit getFahrenheit();

    static MeasureUnit *createGenericTemperature(UErrorCode &status);

    static MeasureUnit getGenericTemperature();

    static MeasureUnit *createKelvin(UErrorCode &status);

    static MeasureUnit getKelvin();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createRankine(UErrorCode &status);

    static MeasureUnit getRankine();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createNewtonMeter(UErrorCode &status);

    static MeasureUnit getNewtonMeter();

    static MeasureUnit *createPoundFoot(UErrorCode &status);

    static MeasureUnit getPoundFoot();

    static MeasureUnit *createAcreFoot(UErrorCode &status);

    static MeasureUnit getAcreFoot();

    static MeasureUnit *createBarrel(UErrorCode &status);

    static MeasureUnit getBarrel();

    static MeasureUnit *createBushel(UErrorCode &status);

    static MeasureUnit getBushel();

    static MeasureUnit *createCentiliter(UErrorCode &status);

    static MeasureUnit getCentiliter();

    static MeasureUnit *createCubicCentimeter(UErrorCode &status);

    static MeasureUnit getCubicCentimeter();

    static MeasureUnit *createCubicFoot(UErrorCode &status);

    static MeasureUnit getCubicFoot();

    static MeasureUnit *createCubicInch(UErrorCode &status);

    static MeasureUnit getCubicInch();

    static MeasureUnit *createCubicKilometer(UErrorCode &status);

    static MeasureUnit getCubicKilometer();

    static MeasureUnit *createCubicMeter(UErrorCode &status);

    static MeasureUnit getCubicMeter();

    static MeasureUnit *createCubicMile(UErrorCode &status);

    static MeasureUnit getCubicMile();

    static MeasureUnit *createCubicYard(UErrorCode &status);

    static MeasureUnit getCubicYard();

    static MeasureUnit *createCup(UErrorCode &status);

    static MeasureUnit getCup();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createCupImperial(UErrorCode &status);

    static MeasureUnit getCupImperial();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createCupJp(UErrorCode &status);

    static MeasureUnit getCupJp();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createCupMetric(UErrorCode &status);

    static MeasureUnit getCupMetric();

    static MeasureUnit *createDeciliter(UErrorCode &status);

    static MeasureUnit getDeciliter();

    static MeasureUnit *createDessertSpoon(UErrorCode &status);

    static MeasureUnit getDessertSpoon();

    static MeasureUnit *createDessertSpoonImperial(UErrorCode &status);

    static MeasureUnit getDessertSpoonImperial();

    static MeasureUnit *createDram(UErrorCode &status);

    static MeasureUnit getDram();

    static MeasureUnit *createDrop(UErrorCode &status);

    static MeasureUnit getDrop();

    static MeasureUnit *createFluidOunce(UErrorCode &status);

    static MeasureUnit getFluidOunce();

    static MeasureUnit *createFluidOunceImperial(UErrorCode &status);

    static MeasureUnit getFluidOunceImperial();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createFluidOunceMetric(UErrorCode &status);

    static MeasureUnit getFluidOunceMetric();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createGallon(UErrorCode &status);

    static MeasureUnit getGallon();

    static MeasureUnit *createGallonImperial(UErrorCode &status);

    static MeasureUnit getGallonImperial();

    static MeasureUnit *createHectoliter(UErrorCode &status);

    static MeasureUnit getHectoliter();

    static MeasureUnit *createJigger(UErrorCode &status);

    static MeasureUnit getJigger();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createKoku(UErrorCode &status);

    static MeasureUnit getKoku();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createKosaji(UErrorCode &status);

    static MeasureUnit getKosaji();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createLiter(UErrorCode &status);

    static MeasureUnit getLiter();

    static MeasureUnit *createMegaliter(UErrorCode &status);

    static MeasureUnit getMegaliter();

    static MeasureUnit *createMilliliter(UErrorCode &status);

    static MeasureUnit getMilliliter();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createOsaji(UErrorCode &status);

    static MeasureUnit getOsaji();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createPinch(UErrorCode &status);

    static MeasureUnit getPinch();

    static MeasureUnit *createPint(UErrorCode &status);

    static MeasureUnit getPint();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createPintImperial(UErrorCode &status);

    static MeasureUnit getPintImperial();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createPintMetric(UErrorCode &status);

    static MeasureUnit getPintMetric();

    static MeasureUnit *createQuart(UErrorCode &status);

    static MeasureUnit getQuart();

    static MeasureUnit *createQuartImperial(UErrorCode &status);

    static MeasureUnit getQuartImperial();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createSai(UErrorCode &status);

    static MeasureUnit getSai();
#endif /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createShaku(UErrorCode &status);

    static MeasureUnit getShaku();
#endif /* U_HIDE_DRAFT_API */

    static MeasureUnit *createTablespoon(UErrorCode &status);

    static MeasureUnit getTablespoon();

    static MeasureUnit *createTeaspoon(UErrorCode &status);

    static MeasureUnit getTeaspoon();

#ifndef U_HIDE_DRAFT_API
    static MeasureUnit *createToJp(UErrorCode &status);

    static MeasureUnit getToJp();
#endif /* U_HIDE_DRAFT_API */


 protected:

#ifndef U_HIDE_INTERNAL_API
    void initTime(const char *timeId);

    void initCurrency(StringPiece isoCurrency);

#endif  /* U_HIDE_INTERNAL_API */

private:

    MeasureUnitImpl* fImpl;

    int16_t fSubTypeId;
    int8_t fTypeId;

    MeasureUnit(int32_t typeId, int32_t subTypeId);
    MeasureUnit(MeasureUnitImpl&& impl);
    void setTo(int32_t typeId, int32_t subTypeId);
    static MeasureUnit *create(int typeId, int subTypeId, UErrorCode &status);

    static bool findBySubType(StringPiece subType, MeasureUnit* output);

    LocalArray<MeasureUnit> splitToSingleUnitsImpl(int32_t& outCount, UErrorCode& status) const;

    friend class MeasureUnitImpl;

    friend class number::impl::LongNameHandler;
};

inline std::pair<LocalArray<MeasureUnit>, int32_t>
MeasureUnit::splitToSingleUnits(UErrorCode& status) const {
    int32_t length;
    auto array = splitToSingleUnitsImpl(length, status);
    return std::make_pair(std::move(array), length);
}

U_NAMESPACE_END

#endif // !UNCONFIG_NO_FORMATTING

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // __MEASUREUNIT_H__
