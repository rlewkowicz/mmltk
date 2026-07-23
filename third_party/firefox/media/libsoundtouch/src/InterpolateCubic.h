/// Author        : Copyright (c) Olli Parviainen
// License :
//  Copyright (c) Olli Parviainen
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//  Lesser General Public License for more details.
//  License along with this library; if not, write to the Free Software

#ifndef _InterpolateCubic_H_
#define _InterpolateCubic_H_

#include "RateTransposer.h"
#include "STTypes.h"

namespace soundtouch
{

class InterpolateCubic : public TransposerBase
{
protected:
    virtual int transposeMono(SAMPLETYPE *dest,
                        const SAMPLETYPE *src,
                        int &srcSamples) override;
    virtual int transposeStereo(SAMPLETYPE *dest,
                        const SAMPLETYPE *src,
                        int &srcSamples) override;
    virtual int transposeMulti(SAMPLETYPE *dest,
                        const SAMPLETYPE *src,
                        int &srcSamples) override;

    double fract;

public:
    InterpolateCubic();

    virtual void resetRegisters() override;

    virtual int getLatency() const override
    {
        return 1;
    }
};

}

#endif
