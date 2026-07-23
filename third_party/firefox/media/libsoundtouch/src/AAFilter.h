/// Author        : Copyright (c) Olli Parviainen
// License :
//  Copyright (c) Olli Parviainen
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//  Lesser General Public License for more details.
//  License along with this library; if not, write to the Free Software

#ifndef AAFilter_H
#define AAFilter_H

#include "STTypes.h"
#include "FIFOSampleBuffer.h"

namespace soundtouch
{

class AAFilter
{
protected:
    class FIRFilter *pFIR;

    double cutoffFreq;

    uint length;

    void calculateCoeffs();
public:
    AAFilter(uint length);

    ~AAFilter();

    void setCutoffFreq(double newCutoffFreq);

    void setLength(uint newLength);

    uint getLength() const;

    uint evaluate(SAMPLETYPE *dest,
                  const SAMPLETYPE *src,
                  uint numSamples,
                  uint numChannels) const;

    uint evaluate(FIFOSampleBuffer &dest,
                  FIFOSampleBuffer &src) const;

};

}

#endif
