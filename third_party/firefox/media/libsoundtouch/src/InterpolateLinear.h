/// Author        : Copyright (c) Olli Parviainen
// License :
//  Copyright (c) Olli Parviainen
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//  Lesser General Public License for more details.
//  License along with this library; if not, write to the Free Software

#ifndef _InterpolateLinear_H_
#define _InterpolateLinear_H_

#include "RateTransposer.h"
#include "STTypes.h"

namespace soundtouch
{

class InterpolateLinearInteger : public TransposerBase
{
protected:
    int iFract;
    int iRate;

    virtual int transposeMono(SAMPLETYPE *dest,
                       const SAMPLETYPE *src,
                       int &srcSamples) override;
    virtual int transposeStereo(SAMPLETYPE *dest,
                         const SAMPLETYPE *src,
                         int &srcSamples) override;
    virtual int transposeMulti(SAMPLETYPE *dest, const SAMPLETYPE *src, int &srcSamples) override;
public:
    InterpolateLinearInteger();

    virtual void setRate(double newRate) override;

    virtual void resetRegisters() override;

    virtual int getLatency() const override
    {
        return 0;
    }
};


class InterpolateLinearFloat : public TransposerBase
{
protected:
    double fract;

    virtual int transposeMono(SAMPLETYPE *dest,
                       const SAMPLETYPE *src,
                       int &srcSamples);
    virtual int transposeStereo(SAMPLETYPE *dest,
                         const SAMPLETYPE *src,
                         int &srcSamples);
    virtual int transposeMulti(SAMPLETYPE *dest, const SAMPLETYPE *src, int &srcSamples);

public:
    InterpolateLinearFloat();

    virtual void resetRegisters();

    int getLatency() const
    {
        return 0;
    }
};

}

#endif
