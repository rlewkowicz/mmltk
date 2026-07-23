// License & terms of use: http://www.unicode.org/copyright.html
/*
 ********************************************************************************
 *   Copyright (C) 1997-2006, International Business Machines
 *   Corporation and others.  All Rights Reserved.
 ********************************************************************************
 *
 * File FIELDPOS.H
 *
 * Modification History:
 *
 *   Date        Name        Description
 *   02/25/97    aliu        Converted from java.
 *   03/17/97    clhuang     Updated per Format implementation.
 *    07/17/98    stephen        Added default/copy ctors, and operators =, ==, !=
 ********************************************************************************
 */

 
#ifndef FIELDPOS_H
#define FIELDPOS_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API


#if !UCONFIG_NO_FORMATTING

#include "unicode/uobject.h"

U_NAMESPACE_BEGIN

class U_I18N_API FieldPosition : public UObject {
public:
    enum { DONT_CARE = -1 };

    FieldPosition() 
        : UObject(), fField(DONT_CARE), fBeginIndex(0), fEndIndex(0) {}

    FieldPosition(int32_t field) 
        : UObject(), fField(field), fBeginIndex(0), fEndIndex(0) {}

    FieldPosition(const FieldPosition& copy) 
        : UObject(copy), fField(copy.fField), fBeginIndex(copy.fBeginIndex), fEndIndex(copy.fEndIndex) {}

    virtual ~FieldPosition();

    FieldPosition&      operator=(const FieldPosition& copy);

    bool               operator==(const FieldPosition& that) const;

    bool               operator!=(const FieldPosition& that) const;

    FieldPosition *clone() const;

    int32_t getField() const { return fField; }

    int32_t getBeginIndex() const { return fBeginIndex; }

    int32_t getEndIndex() const { return fEndIndex; }

    void setField(int32_t f) { fField = f; }

    void setBeginIndex(int32_t bi) { fBeginIndex = bi; }

    void setEndIndex(int32_t ei) { fEndIndex = ei; }
    
    virtual UClassID getDynamicClassID() const override;

    static UClassID U_EXPORT2 getStaticClassID();

private:
    int32_t fField;

    int32_t fBeginIndex;

    int32_t fEndIndex;
};

inline FieldPosition&
FieldPosition::operator=(const FieldPosition& copy)
{
    fField         = copy.fField;
    fEndIndex     = copy.fEndIndex;
    fBeginIndex = copy.fBeginIndex;
    return *this;
}

inline bool
FieldPosition::operator==(const FieldPosition& copy) const
{
    return (fField == copy.fField &&
        fEndIndex == copy.fEndIndex &&
        fBeginIndex == copy.fBeginIndex);
}

inline bool
FieldPosition::operator!=(const FieldPosition& copy) const
{
    return !operator==(copy);
}

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _FIELDPOS
