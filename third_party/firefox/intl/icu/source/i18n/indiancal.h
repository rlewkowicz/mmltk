// License & terms of use: http://www.unicode.org/copyright.html
/*
 *****************************************************************************
 * Copyright (C) 2003-2008, International Business Machines Corporation
 * and others. All Rights Reserved.
 *****************************************************************************
 *
 * File INDIANCAL.H
 *****************************************************************************
 */

#ifndef INDIANCAL_H
#define INDIANCAL_H

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/calendar.h"

U_NAMESPACE_BEGIN



class U_I18N_API IndianCalendar : public Calendar {
public:
  enum EEras {
      CHAITRA,

      VAISAKHA,

      JYAISTHA,

      ASADHA,

      SRAVANA,

      BHADRA,

      ASVINA,

      KARTIKA,

      AGRAHAYANA,

      PAUSA,

      MAGHA,

      PHALGUNA
    };


  IndianCalendar(const Locale& aLocale, UErrorCode &success);

  IndianCalendar(const IndianCalendar& other);

  virtual ~IndianCalendar();

    



  virtual IndianCalendar* clone() const override;

 private:
 protected:
  virtual int32_t handleGetLimit(UCalendarDateFields field, ELimitType limitType) const override;
  
  virtual int32_t handleGetMonthLength(int32_t extendedYear, int32_t month, UErrorCode& status) const override;
  
  virtual int32_t handleGetYearLength(int32_t extendedYear, UErrorCode& status) const override;


  virtual int64_t handleComputeMonthStart(int32_t eyear, int32_t month, UBool useMonth, UErrorCode& status) const override;


  virtual int32_t handleGetExtendedYear(UErrorCode& status) override;

  virtual void handleComputeFields(int32_t julianDay, UErrorCode &status) override;

 public: 
  virtual UClassID getDynamicClassID() const override;

  static UClassID U_EXPORT2 getStaticClassID();

  virtual const char * getType() const override;

private:
  IndianCalendar() = delete; 

protected:
  int32_t getRelatedYearDifference() const override;

  DECLARE_OVERRIDE_SYSTEM_DEFAULT_CENTURY
};

U_NAMESPACE_END

#endif
#endif



