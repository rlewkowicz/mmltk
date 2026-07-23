/**
 * @file avir.h
 *
 * @version 3.1
 *
 * @brief The "main" inclusion file with all required classes and functions.
 *
 * This is the "main" inclusion file for the "AVIR" image resizer. This
 * inclusion file contains implementation of the AVIR image resizing algorithm
 * in its entirety. Also includes several classes and functions that can be
 * useful elsewhere.
 *
 * AVIR Copyright (c) 2015-2025 Aleksey Vaneev
 *
 * @mainpage
 *
 * @section intro_sec Introduction
 *
 * Description is available at https://github.com/avaneev/avir
 *
 * AVIR is devoted to women. Your digital photos can look good at any size!
 *
 * Please credit the author of this library in your documentation in the
 * following way: "AVIR image resizing algorithm designed by Aleksey Vaneev".
 *
 * @section license License
 *
 * MIT License
 *
 * Copyright (c) 2015-2025 Aleksey Vaneev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef AVIR_CIMAGERESIZER_INCLUDED
#define AVIR_CIMAGERESIZER_INCLUDED

#include <cstring>
#include <cmath>
#include <unordered_map>
#include <vector>

#if __cplusplus >= 201103L

#include <cstdint>

#else  // __cplusplus >= 201103L

#include <stdint.h>

#endif  // __cplusplus >= 201103L

namespace avir {

using std ::memcpy;
using std ::memset;
using std ::floor;
using std ::ceil;
using std ::sin;
using std ::cos;
using std ::size_t;

namespace detail {

class CThreadLocalBufferCache {
   public:
    ~CThreadLocalBufferCache() {
        for (auto& entry : Buckets) {
            for (char* const block : entry.second) {
                delete[] block;
            }
        }
    }

    char* acquire(const size_t ByteCount) {
        if (ByteCount == 0) {
            return (nullptr);
        }

        auto it = Buckets.find(ByteCount);

        if (it != Buckets.end() && !it->second.empty()) {
            char* const block = it->second.back();
            it->second.pop_back();
            return (block);
        }

        return (new char[ByteCount]);
    }

    void release(char* const Block, const size_t ByteCount) {
        if (Block == nullptr || ByteCount == 0) {
            return;
        }

        auto& bucket = Buckets[ByteCount];

        if (bucket.size() < MaxBlocksPerBucket) {
            bucket.push_back(Block);
            return;
        }

        delete[] Block;
    }

   private:
    static constexpr size_t MaxBlocksPerBucket = 4;
    std ::unordered_map<size_t, std ::vector<char*>> Buckets;
};

inline CThreadLocalBufferCache& getThreadLocalBufferCache() {
    thread_local CThreadLocalBufferCache Cache;
    return (Cache);
}

}  

#if __cplusplus >= 201103L

using std ::uintptr_t;

#else  // __cplusplus >= 201103L

#if !defined(nullptr)
#define nullptr NULL
#define AVIR_NULLPTR
#endif  // !defined( nullptr )

#endif  // __cplusplus >= 201103L

#define AVIR_VERSION "3.1"  ///< The macro defines AVIR version string.

#define AVIR_PI 3.1415926535897932  ///< The macro equals to `pi` constant,

#define AVIR_PId2 1.5707963267948966  ///< The macro equals to `pi divided

#define AVIR_NOCTOR(ClassName)               \
   private:                                  \
    ClassName(const ClassName&) {}           \
    ClassName& operator=(const ClassName&) { \
        return (*this);                      \
    }


template <typename T>
inline T round(const T d) {
    return (d < (T)0 ? -(T)(int)((T)0.5 - d) : (T)(int)(d + (T)0.5));
}


template <typename T>
inline T clamp(const T& Value, const T minv, const T maxv) {
    return (Value < minv ? minv : (Value > maxv ? maxv : Value));
}


template <typename T>
inline T pow24_sRGB(const T x0) {
    const double x = (double)x0;
    const double x2 = x * x;
    const double x3 = x2 * x;
    const double x4 = x2 * x2;

    return ((T)(0.0985766365536824 + 0.839474952656502 * x2 + 0.363287814061725 * x3 -
                0.0125559718896615 / (0.12758338921578 + 0.290283465468235 * x) - 0.231757513261358 * x -
                0.0395365717969074 * x4));
}


template <typename T>
inline T pow24i_sRGB(const T x0) {
    const double x = (double)x0;
    const double sx = sqrt(x);
    const double ssx = sqrt(sx);
    const double sssx = sqrt(ssx);

    return ((T)(0.000213364515060263 + 0.0149409239419218 * x + 0.433973412731747 * sx +
                ssx * (0.659628181609715 * sssx - 0.0380957908841466 - 0.0706476137208521 * sx)));
}


template <typename T, typename Tin>
inline T convertSRGB2Lin(const Tin& s0, const T m) {
    const T s = (T)s0 * m;
    const T a = (T)0.055;

    if (s <= (T)0.04045) {
        return (s / (T)12.92);
    }

    return (pow24_sRGB((s + a) / ((T)1 + a)));
}


template <typename T>
inline T convertSRGB2Lin(const unsigned char& s0, const T) {
    static const float tbl[256] = {
        0.0f,         0.000303527f, 0.000607054f, 0.000910581f, 0.001214108f, 0.001517635f, 0.001821162f, 0.002124689f,
        0.002428216f, 0.002731743f, 0.00303527f,  0.003348383f, 0.003678029f, 0.004025973f, 0.004392482f, 0.004777817f,
        0.005182236f, 0.005605992f, 0.006049334f, 0.006512507f, 0.006995751f, 0.007499306f, 0.008023403f, 0.008568275f,
        0.009134147f, 0.009721244f, 0.01032979f,  0.01095999f,  0.01161207f,  0.01228625f,  0.01298271f,  0.01370169f,
        0.01444337f,  0.01520795f,  0.01599565f,  0.01680664f,  0.01764113f,  0.01849931f,  0.01938136f,  0.02028748f,
        0.02121784f,  0.02217263f,  0.02315203f,  0.02415622f,  0.02518537f,  0.02623966f,  0.02731927f,  0.02842436f,
        0.0295551f,   0.03071166f,  0.0318942f,   0.0331029f,   0.03433791f,  0.03559939f,  0.03688751f,  0.03820243f,
        0.03954429f,  0.04091326f,  0.04230949f,  0.04373313f,  0.04518433f,  0.04666325f,  0.04817003f,  0.04970482f,
        0.05126777f,  0.05285903f,  0.05447873f,  0.05612702f,  0.05780404f,  0.05950994f,  0.06124485f,  0.06300892f,
        0.06480227f,  0.06662506f,  0.0684774f,   0.07035945f,  0.07227132f,  0.07421317f,  0.07618511f,  0.07818728f,
        0.08021981f,  0.08228283f,  0.08437647f,  0.08650086f,  0.08865612f,  0.09084239f,  0.09305977f,  0.09530841f,
        0.09758843f,  0.09989995f,  0.1022431f,   0.104618f,    0.1070247f,   0.1094634f,   0.1119343f,   0.1144373f,
        0.1169728f,   0.1195406f,   0.1221411f,   0.1247742f,   0.1274402f,   0.1301391f,   0.132871f,    0.1356361f,
        0.1384344f,   0.1412662f,   0.1441314f,   0.1470303f,   0.1499628f,   0.1529292f,   0.1559296f,   0.158964f,
        0.1620326f,   0.1651354f,   0.1682726f,   0.1714443f,   0.1746506f,   0.1778916f,   0.1811674f,   0.1844781f,
        0.1878239f,   0.1912047f,   0.1946208f,   0.1980722f,   0.2015591f,   0.2050815f,   0.2086396f,   0.2122334f,
        0.215863f,    0.2195286f,   0.2232303f,   0.2269681f,   0.2307422f,   0.2345526f,   0.2383994f,   0.2422828f,
        0.2462029f,   0.2501597f,   0.2541534f,   0.258184f,    0.2622517f,   0.2663564f,   0.2704985f,   0.2746778f,
        0.2788946f,   0.2831489f,   0.2874409f,   0.2917705f,   0.296138f,    0.3005433f,   0.3049867f,   0.3094681f,
        0.3139877f,   0.3185456f,   0.3231419f,   0.3277766f,   0.3324498f,   0.3371618f,   0.3419124f,   0.3467019f,
        0.3515303f,   0.3563977f,   0.3613041f,   0.3662498f,   0.3712348f,   0.3762591f,   0.3813228f,   0.3864261f,
        0.391569f,    0.3967517f,   0.4019741f,   0.4072364f,   0.4125387f,   0.4178811f,   0.4232636f,   0.4286864f,
        0.4341495f,   0.4396529f,   0.4451969f,   0.4507815f,   0.4564067f,   0.4620726f,   0.4677794f,   0.4735271f,
        0.4793158f,   0.4851456f,   0.4910165f,   0.4969287f,   0.5028822f,   0.5088771f,   0.5149135f,   0.5209915f,
        0.5271111f,   0.5332725f,   0.5394757f,   0.5457208f,   0.5520078f,   0.558337f,    0.5647082f,   0.5711217f,
        0.5775775f,   0.5840756f,   0.5906162f,   0.5971993f,   0.6038251f,   0.6104935f,   0.6172047f,   0.6239587f,
        0.6307557f,   0.6375956f,   0.6444787f,   0.6514048f,   0.6583742f,   0.665387f,    0.6724431f,   0.6795426f,
        0.6866857f,   0.6938724f,   0.7011027f,   0.7083769f,   0.7156948f,   0.7230567f,   0.7304625f,   0.7379124f,
        0.7454064f,   0.7529446f,   0.7605271f,   0.7681539f,   0.7758252f,   0.7835409f,   0.7913012f,   0.7991061f,
        0.8069558f,   0.8148502f,   0.8227894f,   0.8307736f,   0.8388028f,   0.846877f,    0.8549964f,   0.8631609f,
        0.8713707f,   0.8796259f,   0.8879265f,   0.8962726f,   0.9046642f,   0.9131014f,   0.9215843f,   0.930113f,
        0.9386874f,   0.9473078f,   0.9559742f,   0.9646865f,   0.973445f,    0.9822496f,   0.9911004f,   0.9999975f};

    return (tbl[(size_t)s0]);
}


template <typename T>
inline T convertLin2SRGB(const T s) {
    const T a = (T)0.055;

    if (s <= (T)0.0031308) {
        return ((T)12.92 * s);
    }

    return (((T)1 + a) * pow24i_sRGB(s) - a);
}


template <typename T1, typename T2>
inline void copyArray(const T1* ip, T2* op, int l, const int ipinc = 1, const int opinc = 1) {
    while (l > 0) {
        *op = (T2)*ip;
        op += opinc;
        ip += ipinc;
        l--;
    }
}


template <typename T1, typename T2>
inline void addArray(const T1* ip, T2* op, int l, const int ipinc = 1, const int opinc = 1) {
    while (l > 0) {
        *op += *ip;
        op += opinc;
        ip += ipinc;
        l--;
    }
}


template <typename T1, typename T2>
inline void replicateArray(const T1* const ip, const int ipl, T2* op, int l, const int opinc) {
    if (ipl == 1) {
        while (l > 0) {
            op[0] = (T2)ip[0];
            op += opinc;
            l--;
        }
    } else if (ipl == 4) {
        while (l > 0) {
            op[0] = (T2)ip[0];
            op[1] = (T2)ip[1];
            op[2] = (T2)ip[2];
            op[3] = (T2)ip[3];
            op += opinc;
            l--;
        }
    } else if (ipl == 3) {
        while (l > 0) {
            op[0] = (T2)ip[0];
            op[1] = (T2)ip[1];
            op[2] = (T2)ip[2];
            op += opinc;
            l--;
        }
    } else if (ipl == 2) {
        while (l > 0) {
            op[0] = (T2)ip[0];
            op[1] = (T2)ip[1];
            op += opinc;
            l--;
        }
    } else {
        while (l > 0) {
            int i;

            for (i = 0; i < ipl; i++) {
                op[i] = (T2)ip[i];
            }

            op += opinc;
            l--;
        }
    }
}


template <typename T>
inline void calcFIRFilterResponse(const T* flt, int fltlen, const double th, double& re0, double& im0,
                                  const int fltlat = 0) {
    const double sincr = 2.0 * cos(th);
    double cvalue1;
    double svalue1;

    if (fltlat == 0) {
        cvalue1 = 1.0;
        svalue1 = 0.0;
    } else {
        cvalue1 = cos(-fltlat * th);
        svalue1 = sin(-fltlat * th);
    }

    double cvalue2 = cos(-(fltlat + 1) * th);
    double svalue2 = sin(-(fltlat + 1) * th);

    double re = 0.0;
    double im = 0.0;

    while (fltlen > 0) {
        re += cvalue1 * (double)flt[0];
        im += svalue1 * (double)flt[0];
        flt++;
        fltlen--;

        double tmp = cvalue1;
        cvalue1 = sincr * cvalue1 - cvalue2;
        cvalue2 = tmp;

        tmp = svalue1;
        svalue1 = sincr * svalue1 - svalue2;
        svalue2 = tmp;
    }

    re0 = re;
    im0 = im;
}


template <typename T>
inline void normalizeFIRFilter(T* const p, const int l, const double DCGain, const int pstep = 1) {
    double s = 0.0;
    T* pp = p;
    int i = l;

    while (i > 0) {
        s += *pp;
        pp += pstep;
        i--;
    }

    s = DCGain / s;
    pp = p;
    i = l;

    while (i > 0) {
        *pp = (T)(*pp * s);
        pp += pstep;
        i--;
    }
}

template <typename T, typename capint = int>
class CBuffer {
   public:
    CBuffer() : Data(nullptr), DataAligned(nullptr), Capacity(0), Alignment(0) {}


    CBuffer(const capint aCapacity, const int aAlignment = 0) {
        allocinit(aCapacity, aAlignment);
    }


    CBuffer(const CBuffer& Source) {
        allocinit(Source.Capacity, Source.Alignment);

        if (Capacity > 0) {
            memcpy(DataAligned, Source.DataAligned, (size_t)Capacity * sizeof(T));
        }
    }

    ~CBuffer() {
        freeData();
    }


    CBuffer& operator=(const CBuffer& Source) {
        alloc(Source.Capacity, Source.Alignment);

        if (Capacity > 0) {
            memcpy(DataAligned, Source.DataAligned, (size_t)Capacity * sizeof(T));
        }

        return (*this);
    }

    operator T*() const {
        return (DataAligned);
    }


    void alloc(const capint aCapacity, const int aAlignment = 0) {
        freeData();
        allocinit(aCapacity, aAlignment);
    }

    void free() {
        freeData();
        DataAligned = nullptr;
        Capacity = 0;
        Alignment = 0;
    }

    capint getCapacity() const {
        return (Capacity);
    }


    void forceCapacity(const capint NewCapacity) {
        Capacity = NewCapacity;
    }


    void increaseCapacity(const capint NewCapacity, const bool DoDataCopy = true) {
        if (NewCapacity < Capacity) {
            return;
        }

        if (DoDataCopy) {
            const capint PrevCapacity = Capacity;
            char* const PrevData = Data;
            T* const PrevDataAligned = DataAligned;
            const int PrevAlignment = Alignment;

            allocinit(NewCapacity, Alignment);

            if (PrevCapacity > 0) {
                memcpy(DataAligned, PrevDataAligned, (size_t)PrevCapacity * sizeof(T));
            }

            detail ::getThreadLocalBufferCache().release(PrevData, calcAllocBytes(PrevCapacity, PrevAlignment));
        } else {
            freeData();
            allocinit(NewCapacity, Alignment);
        }
    }


    void truncateCapacity(const capint NewCapacity) {
        if (NewCapacity >= Capacity) {
            return;
        }

        Capacity = NewCapacity;
    }


    void updateCapacity(const capint ReqCapacity) {
        if (ReqCapacity <= Capacity) {
            return;
        }

        capint NewCapacity = Capacity;

        while (NewCapacity < ReqCapacity) {
            NewCapacity += NewCapacity / 3 + 1;
        }

        increaseCapacity(NewCapacity);
    }

   private:
    char* Data;
    T* DataAligned;
    capint Capacity;
    int Alignment;

    static size_t calcAllocBytes(const capint aCapacity, const int aAlignment) {
        if (aCapacity <= 0) {
            return (0);
        }

        return ((size_t)aCapacity * sizeof(T) + (aAlignment > 0 ? (size_t)aAlignment : 0));
    }


    void allocinit(const capint aCapacity, const int aAlignment) {
        if (aCapacity <= 0) {
            Data = nullptr;
            DataAligned = nullptr;
            Capacity = 0;
            Alignment = aAlignment;
            return;
        }

        char* const RawData = detail ::getThreadLocalBufferCache().acquire(calcAllocBytes(aCapacity, aAlignment));

        if (aAlignment == 0) {
            Data = RawData;
            DataAligned = (T*)Data;
            Alignment = 0;
        } else {
            Data = RawData;

            DataAligned = (T*)(((uintptr_t)Data + (uintptr_t)aAlignment) & ~(uintptr_t)(aAlignment - 1));

            Alignment = aAlignment;
        }

        Capacity = aCapacity;
    }

    void freeData() {
        detail ::getThreadLocalBufferCache().release(Data, calcAllocBytes(Capacity, Alignment));
        Data = nullptr;
    }
};

template <class T>
class CStructArray {
   public:
    CStructArray() : ItemCount(0) {}


    CStructArray(const CStructArray& Source) : ItemCount(0), Items(Source.getItemCount()) {
        while (ItemCount < Source.getItemCount()) {
            Items[ItemCount] = new T(Source[ItemCount]);
            ItemCount++;
        }
    }

    ~CStructArray() {
        clear();
    }


    CStructArray& operator=(const CStructArray& Source) {
        clear();

        const int NewCount = Source.ItemCount;
        Items.updateCapacity(NewCount);

        while (ItemCount < NewCount) {
            Items[ItemCount] = new T(Source[ItemCount]);
            ItemCount++;
        }

        return (*this);
    }


    T& operator[](const int Index) {
        return (*Items[Index]);
    }


    const T& operator[](const int Index) const {
        return (*Items[Index]);
    }


    T& add() {
        if (ItemCount == Items.getCapacity()) {
            Items.increaseCapacity(ItemCount * 3 / 2 + 1);
        }

        Items[ItemCount] = new T();
        ItemCount++;

        return ((*this)[ItemCount - 1]);
    }


    void setItemCount(const int NewCount) {
        if (NewCount > ItemCount) {
            Items.increaseCapacity(NewCount);

            while (ItemCount < NewCount) {
                Items[ItemCount] = new T();
                ItemCount++;
            }
        } else {
            while (ItemCount > NewCount) {
                ItemCount--;
                delete Items[ItemCount];
            }
        }
    }

    void clear() {
        while (ItemCount > 0) {
            ItemCount--;
            delete Items[ItemCount];
        }
    }

    int getItemCount() const {
        return (ItemCount);
    }

   private:
    CBuffer<T*> Items;
    int ItemCount;
};

class CSineGen {
   public:

    CSineGen(const double si, const double ph) : svalue1(sin(ph)), svalue2(sin(ph - si)), sincr(2.0 * cos(si)) {}

    double generate() {
        const double res = svalue1;

        svalue1 = sincr * res - svalue2;
        svalue2 = res;

        return (res);
    }

   private:
    double svalue1;
    double svalue2;
    double sincr;
};

class CDSPWindowGenPeakedCosine {
   public:

    CDSPWindowGenPeakedCosine(const double aAlpha, const double aLen2)
        : Alpha(aAlpha), Len2(aLen2), Len2i(1.0 / aLen2), wn(0.0), w1(AVIR_PId2 / Len2, AVIR_PI * 0.5) {}

    double generate() {
        const double h = pow(wn * Len2i, Alpha);
        wn += 1.0;

        return (w1.generate() * (1.0 - h));
    }

   private:
    double Alpha;
    double Len2;
    double Len2i;
    double wn;
    CSineGen w1;
};

class CDSPFIREQ {
   public:

    void init(const double SampleRate, const double aFilterLength, const int aBandCount, const double MinFreq,
              const double MaxFreq, const bool IsLogBands, const double WFAlpha) {
        FilterLength = aFilterLength;
        BandCount = aBandCount;

        CenterFreqs.alloc(BandCount);

        z = (int)ceil(FilterLength * 0.5);
        zi = z + (z & 1);
        z2 = z * 2;

        CBuffer<double> oscbuf(z2);
        initOscBuf(oscbuf);

        CBuffer<double> winbuf(z);
        initWinBuf(winbuf, WFAlpha);

        UseFirstVirtBand = (MinFreq > 0.0);
        const int k = zi * (BandCount + (UseFirstVirtBand ? 1 : 0));
        Kernels1.alloc(k);
        Kernels2.alloc(k);

        double m;
        double mo;

        if (IsLogBands) {
            m = exp(log(MaxFreq / MinFreq) / (BandCount - 1));
            mo = 0.0;
        } else {
            m = 1.0;
            mo = (MaxFreq - MinFreq) / (BandCount - 1);
        }

        double f = MinFreq;
        double x1 = 0.0;
        double x2;
        int si;

        if (UseFirstVirtBand) {
            si = 0;
        } else {
            si = 1;
            CenterFreqs[0] = 0.0;
            f = f * m + mo;
        }

        double* kernbuf1 = &Kernels1[0];
        double* kernbuf2 = &Kernels2[0];
        int i;

        for (i = si; i < BandCount; i++) {
            x2 = f * 2.0 / SampleRate;
            CenterFreqs[i] = x2;

            fillBandKernel(x1, x2, kernbuf1, kernbuf2, oscbuf, winbuf);

            kernbuf1 += zi;
            kernbuf2 += zi;
            x1 = x2;
            f = f * m + mo;
        }

        if (x1 < 1.0) {
            UseLastVirtBand = true;
            fillBandKernel(x1, 1.0, kernbuf1, kernbuf2, oscbuf, winbuf);
        } else {
            UseLastVirtBand = false;
        }
    }

    int getFilterLength() const {
        return (z2 - 1);
    }

    int getFilterLatency() const {
        return (z - 1);
    }


    void buildFilter(const double* const BandGains, double* const Filter) {
        const double* kernbuf1 = &Kernels1[0];
        const double* kernbuf2 = &Kernels2[0];
        double x1 = 0.0;
        double y1 = BandGains[0];
        double x2;
        double y2;

        int i;
        int si;

        if (UseFirstVirtBand) {
            si = 1;
            x2 = CenterFreqs[0];
            y2 = y1;
        } else {
            si = 2;
            x2 = CenterFreqs[1];
            y2 = BandGains[1];
        }

        copyBandKernel(Filter, kernbuf1, kernbuf2, y1 - y2, x1 * y2 - x2 * y1);

        kernbuf1 += zi;
        kernbuf2 += zi;
        x1 = x2;
        y1 = y2;

        for (i = si; i < BandCount; i++) {
            x2 = CenterFreqs[i];
            y2 = BandGains[i];

            addBandKernel(Filter, kernbuf1, kernbuf2, y1 - y2, x1 * y2 - x2 * y1);

            kernbuf1 += zi;
            kernbuf2 += zi;
            x1 = x2;
            y1 = y2;
        }

        if (UseLastVirtBand) {
            addBandKernel(Filter, kernbuf1, kernbuf2, y1 - y2, x1 * y2 - y1);
        }

        for (i = 0; i < z - 1; i++) {
            Filter[z + i] = Filter[z - 2 - i];
        }
    }


    static int calcFilterLength(const double aFilterLength, int& Latency) {
        const int l = (int)ceil(aFilterLength * 0.5);
        Latency = l - 1;

        return (l * 2 - 1);
    }

   private:
    double FilterLength;
    int z;
    int zi;
    int z2;
    int BandCount;
    CBuffer<double> CenterFreqs;
    CBuffer<double> Kernels1;
    CBuffer<double> Kernels2;
    bool UseFirstVirtBand;
    bool UseLastVirtBand;


    void initOscBuf(double* oscbuf) const {
        int i = z;

        while (i > 0) {
            oscbuf[0] = 0.0;
            oscbuf[1] = 1.0;
            oscbuf += 2;
            i--;
        }
    }


    void initWinBuf(double* winbuf, const double Alpha) const {
        CDSPWindowGenPeakedCosine wf(Alpha, FilterLength * 0.5);
        int i;

        for (i = 1; i <= z; i++) {
            winbuf[z - i] = wf.generate();
        }
    }


    void fillBandKernel(const double x1, const double x2, double* kernbuf1, double* kernbuf2, double* oscbuf,
                        const double* const winbuf) {
        const double s2_incr = AVIR_PI * x2;
        const double s2_coeff = 2.0 * cos(s2_incr);

        double s2_value1 = sin(s2_incr * (-z + 1));
        double c2_value1 = sin(s2_incr * (-z + 1) + AVIR_PI * 0.5);
        oscbuf[0] = sin(s2_incr * -z);
        oscbuf[1] = sin(s2_incr * -z + AVIR_PI * 0.5);

        int ks;

        for (ks = 1; ks < z; ks++) {
            const int ks2 = ks * 2;
            const double s1_value1 = oscbuf[ks2];
            const double c1_value1 = oscbuf[ks2 + 1];
            oscbuf[ks2] = s2_value1;
            oscbuf[ks2 + 1] = c2_value1;

            const double x = AVIR_PI * (ks - z);
            const double v0 = winbuf[ks - 1] / ((x1 - x2) * x);

            kernbuf1[ks - 1] = (x2 * s2_value1 - x1 * s1_value1 + (c2_value1 - c1_value1) / x) * v0;

            kernbuf2[ks - 1] = (s2_value1 - s1_value1) * v0;

            s2_value1 = s2_coeff * s2_value1 - oscbuf[ks2 - 2];
            c2_value1 = s2_coeff * c2_value1 - oscbuf[ks2 - 1];
        }

        kernbuf1[z - 1] = (x2 * x2 - x1 * x1) / (x1 - x2) * 0.5;
        kernbuf2[z - 1] = -1.0;
    }


    void copyBandKernel(double* outbuf, const double* const kernbuf1, const double* const kernbuf2, const double c,
                        const double d) const {
        int ks;

        for (ks = 0; ks < z; ks++) {
            outbuf[ks] = c * kernbuf1[ks] + d * kernbuf2[ks];
        }
    }


    void addBandKernel(double* outbuf, const double* const kernbuf1, const double* const kernbuf2, const double c,
                       const double d) const {
        int ks;

        for (ks = 0; ks < z; ks++) {
            outbuf[ks] += c * kernbuf1[ks] + d * kernbuf2[ks];
        }
    }
};

class CDSPPeakedCosineLPF {
   public:
    int fl2;
    int FilterLen;


    CDSPPeakedCosineLPF(const double aLen2, const double aFreq2, const double aAlpha)
        : fl2((int)ceil(aLen2) - 1), FilterLen(fl2 + fl2 + 1), Len2(aLen2), Freq2(aFreq2), Alpha(aAlpha) {}


    template <typename T>
    void generateLPF(T* op, const double DCGain) {
        CDSPWindowGenPeakedCosine wf(Alpha, Len2);
        CSineGen f2(Freq2, 0.0);

        op += fl2;
        T* op2 = op;
        f2.generate();

        if (DCGain > 0.0) {
            int t = 1;

            *op = (T)(Freq2 * wf.generate());
            double s = *op;

            while (t <= fl2) {
                const T v = (T)(f2.generate() * wf.generate() / t);
                op++;
                op2--;
                *op = v;
                *op2 = v;
                s += v + v;
                t++;
            }

            t = FilterLen;
            s = DCGain / s;

            while (t > 0) {
                *op2 = (T)(*op2 * s);
                op2++;
                t--;
            }
        } else {
            int t = 1;

            *op = (T)(Freq2 * wf.generate());

            while (t <= fl2) {
                const T v = (T)(f2.generate() * wf.generate() / t);
                op++;
                op2--;
                *op = v;
                *op2 = v;
                t++;
            }
        }
    }

   private:
    double Len2;
    double Freq2;
    double Alpha;
};

class CFltBuffer : public CBuffer<double> {
   public:
    double Len2;
    double Freq;
    double Alpha;
    double DCGain;

    CFltBuffer() : CBuffer<double>(), Len2(0.0), Freq(0.0), Alpha(0.0), DCGain(0.0) {}


    bool operator==(const CFltBuffer& b2) const {
        return (Len2 == b2.Len2 && Freq == b2.Freq && Alpha == b2.Alpha && DCGain == b2.DCGain);
    }
};

template <typename fptype>
class CDSPFracFilterBankLin {
    AVIR_NOCTOR(CDSPFracFilterBankLin)

   public:
    CDSPFracFilterBankLin() : Order(-1) {}


    void copyInitParams(const CDSPFracFilterBankLin& s) {
        WFLen2 = s.WFLen2;
        WFFreq = s.WFFreq;
        WFAlpha = s.WFAlpha;
        FracCount = s.FracCount;
        Order = s.Order;
        Alignment = s.Alignment;
        SrcFilterLen = s.SrcFilterLen;
        FilterLen = s.FilterLen;
        FilterSize = s.FilterSize;
        IsSrcTableBuilt = false;
        ExtFilter = s.ExtFilter;
        TableFillFlags.alloc(s.TableFillFlags.getCapacity());
        int i;

        for (i = 0; i < TableFillFlags.getCapacity(); i++) {
            TableFillFlags[i] = (char)(s.TableFillFlags[i] << 2);
        }
    }


    bool operator==(const CDSPFracFilterBankLin& s) const {
        return (Order == s.Order && WFLen2 == s.WFLen2 && WFFreq == s.WFFreq && WFAlpha == s.WFAlpha &&
                FracCount == s.FracCount && ExtFilter == s.ExtFilter);
    }


    void init(const int ReqFracCount, const int ReqOrder, const double BaseLen, const double Cutoff,
              const double aWFAlpha, const CFltBuffer& aExtFilter, const int aAlignment = 0,
              const int FltLenAlign = 1) {
        double NewWFLen2 = 0.5 * BaseLen * ReqFracCount;
        double NewWFFreq = AVIR_PI * Cutoff / ReqFracCount;
        double NewWFAlpha = aWFAlpha;

        if (ReqOrder == Order && NewWFLen2 == WFLen2 && NewWFFreq == WFFreq && NewWFAlpha == WFAlpha &&
            ReqFracCount == FracCount && aExtFilter == ExtFilter) {
            IsInitRequired = false;
            return;
        }

        WFLen2 = NewWFLen2;
        WFFreq = NewWFFreq;
        WFAlpha = NewWFAlpha;
        FracCount = ReqFracCount;
        Order = ReqOrder;
        Alignment = aAlignment;
        ExtFilter = aExtFilter;

        CDSPPeakedCosineLPF p(WFLen2, WFFreq, WFAlpha);
        SrcFilterLen = (p.fl2 / ReqFracCount + 1) * 2;

        const int ElementSize = ReqOrder + 1;
        FilterLen = SrcFilterLen;

        if (ExtFilter.getCapacity() > 0) {
            FilterLen += ExtFilter.getCapacity() - 1;
        }

        FilterLen = (FilterLen + FltLenAlign - 1) & ~(FltLenAlign - 1);
        FilterSize = FilterLen * ElementSize;
        IsSrcTableBuilt = false;
        IsInitRequired = true;
    }

    int getFilterLen() const {
        return (FilterLen);
    }

    int getFracCount() const {
        return (FracCount);
    }

    int getOrder() const {
        return (Order);
    }


    const fptype* getFilter(const int i) {
        if (!IsSrcTableBuilt) {
            buildSrcTable();
        }

        fptype* const Res = &Table[i * FilterSize];

        if ((TableFillFlags[i] & 2) == 0) {
            createFilter(i);
            TableFillFlags[i] |= 2;

            if (Order > 0) {
                createFilter(i + 1);
                const fptype* const Res2 = Res + FilterSize;
                fptype* const op = Res + FilterLen;
                int j;

                for (j = 0; j < FilterLen; j++) {
                    op[j] = Res2[j] - Res[j];
                }
            }
        }

        return (Res);
    }


    const fptype* getFilterConst(const int i) const {
        return (&Table[i * FilterSize]);
    }

    void createAllFilters() {
        int i;

        for (i = 0; i < FracCount; i++) {
            getFilter(i);
        }
    }


    int calcInitComplexity(const CBuffer<char>& FracUseMap) const {
        const int FltInitCost = 65;
        const int FltUseCost = FilterLen * Order + SrcFilterLen * ExtFilter.getCapacity();
        const int ucb[2] = {0, FltUseCost};
        int ic;
        int i;

        if (IsInitRequired) {
            ic = FracCount * SrcFilterLen * FltInitCost;

            for (i = 0; i < FracCount; i++) {
                ic += ucb[(size_t)FracUseMap[i]];
            }
        } else {
            ic = 0;

            for (i = 0; i < FracCount; i++) {
                if (FracUseMap[i] != 0) {
                    ic += ucb[TableFillFlags[i] == 0 ? 1 : 0];
                }
            }
        }

        return (ic);
    }

   private:
    static const int InterpPoints = 2;
    double WFLen2;
    double WFFreq;
    double WFAlpha;
    int FracCount;
    int Order;
    int Alignment;
    int SrcFilterLen;
    int FilterLen;
    int FilterSize;
    bool IsInitRequired;
    CBuffer<fptype> Table;
    CBuffer<char> TableFillFlags;
    CFltBuffer ExtFilter;
    CBuffer<double> SrcTable;
    bool IsSrcTableBuilt;

    void buildSrcTable() {
        IsSrcTableBuilt = true;
        IsInitRequired = false;

        CDSPPeakedCosineLPF p(WFLen2, WFFreq, WFAlpha);

        const int BufLen = SrcFilterLen * FracCount + InterpPoints - 1;
        const int BufOffs = InterpPoints / 2 - 1;
        const int BufCenter = SrcFilterLen * FracCount / 2 + BufOffs;

        CBuffer<double> Buf(BufLen);
        memset(Buf, 0, (size_t)(BufCenter - p.fl2) * sizeof(double));
        int i = BufLen - BufCenter - p.fl2 - 1;
        memset(&Buf[BufLen - i], 0, (size_t)i * sizeof(double));

        p.generateLPF(&Buf[BufCenter - p.fl2], 0.0);

        SrcTable.alloc((FracCount + 1) * SrcFilterLen);
        TableFillFlags.alloc(FracCount + 1);
        int j;
        double* op0 = SrcTable;

        for (i = FracCount; i >= 0; i--) {
            TableFillFlags[i] = 0;
            double* ip = Buf + BufOffs + i;

            for (j = 0; j < SrcFilterLen; j++) {
                op0[0] = ip[0];
                op0++;
                ip += FracCount;
            }

            normalizeFIRFilter(op0 - SrcFilterLen, SrcFilterLen, 1.0);
        }

        Table.alloc((FracCount + 1) * FilterSize, Alignment);
    }


    void createFilter(const int n) {
        if (TableFillFlags[n] != 0) {
            return;
        }

        TableFillFlags[n] |= 1;
        const int ExtFilterLatency = ExtFilter.getCapacity() / 2;
        const int ResLatency = ExtFilterLatency + SrcFilterLen / 2;
        int ResLen = SrcFilterLen;

        if (ExtFilter.getCapacity() > 0) {
            ResLen += ExtFilter.getCapacity() - 1;
        }

        const int ResOffs = FilterLen / 2 - ResLatency;
        fptype* op = &Table[n * FilterSize];
        int i;

        for (i = 0; i < ResOffs; i++) {
            op[i] = 0;
        }

        for (i = ResOffs + ResLen; i < FilterLen; i++) {
            op[i] = 0;
        }

        op += ResOffs;
        const double* const srcflt = &SrcTable[n * SrcFilterLen];

        if (ExtFilter.getCapacity() == 0) {
            for (i = 0; i < ResLen; i++) {
                op[i] = (fptype)srcflt[i];
            }

            return;
        }

        const double* const extflt = &ExtFilter[0];
        int j;

        for (j = 0; j < ResLen; j++) {
            int k = 0;
            int l = j - ExtFilter.getCapacity() + 1;
            int r = l + ExtFilter.getCapacity();

            if (l < 0) {
                k -= l;
                l = 0;
            }

            if (r > SrcFilterLen) {
                r = SrcFilterLen;
            }

            const double* const extfltb = extflt + k;
            const double* const srcfltb = srcflt + l;
            double s = 0.0;
            l = r - l;

            for (i = 0; i < l; i++) {
                s += extfltb[i] * srcfltb[i];
            }

            op[j] = (fptype)s;
        }
    }
};

class CImageResizerThreadPool {
   public:
    CImageResizerThreadPool() {}

    virtual ~CImageResizerThreadPool() {}

    class CWorkload {
       public:
        virtual ~CWorkload() {}

        virtual void process() = 0;
    };


    virtual int getSuggestedWorkloadCount() const {
        return (1);
    }


    virtual void addWorkload(CWorkload* const Workload) {}

    virtual void startAllWorkloads() {}

    virtual void waitAllWorkloadsToFinish() {}

    virtual void removeAllWorkloads() {}
};

struct CImageResizerParams {
    double CorrFltAlpha;
    double CorrFltLen;
    double IntFltAlpha;
    double IntFltCutoff;
    double IntFltLen;
    double LPFltAlpha;
    double LPFltBaseLen;
    double LPFltCutoffMult;

    CImageResizerParams() : HBFltAlpha(1.94609), HBFltCutoff(0.46437), HBFltLen(24) {}

    double HBFltAlpha;
    double HBFltCutoff;
    double HBFltLen;
};

struct CImageResizerParamsDef : public CImageResizerParams {
    CImageResizerParamsDef() {
        CorrFltAlpha = 0.97946;
        CorrFltLen = 6.4262;
        IntFltAlpha = 6.41341;
        IntFltCutoff = 0.7372;
        IntFltLen = 18;
        LPFltAlpha = 4.76449;
        LPFltBaseLen = 7.55999999999998;
        LPFltCutoffMult = 0.79285;
    }
};

struct CImageResizerParamsULR : public CImageResizerParams {
    CImageResizerParamsULR() {
        CorrFltAlpha = 0.95521;
        CorrFltLen = 5.70774;
        IntFltAlpha = 1.00766;
        IntFltCutoff = 0.74202;
        IntFltLen = 18;
        LPFltAlpha = 1.6801;
        LPFltBaseLen = 6.62;
        LPFltCutoffMult = 0.67821;
    }
};

struct CImageResizerParamsLR : public CImageResizerParams {
    CImageResizerParamsLR() {
        CorrFltAlpha = 1;
        CorrFltLen = 5.865;
        IntFltAlpha = 1.79529;
        IntFltCutoff = 0.74325;
        IntFltLen = 18;
        LPFltAlpha = 1.87597;
        LPFltBaseLen = 6.89999999999999;
        LPFltCutoffMult = 0.69326;
    }
};

struct CImageResizerParamsLow : public CImageResizerParams {
    CImageResizerParamsLow() {
        CorrFltAlpha = 0.99739;
        CorrFltLen = 6.20326;
        IntFltAlpha = 4.6836;
        IntFltCutoff = 0.73879;
        IntFltLen = 18;
        LPFltAlpha = 7.86565;
        LPFltBaseLen = 6.91999999999999;
        LPFltCutoffMult = 0.78379;
    }
};

struct CImageResizerParamsHigh : public CImageResizerParams {
    CImageResizerParamsHigh() {
        CorrFltAlpha = 0.97433;
        CorrFltLen = 6.87893;
        IntFltAlpha = 7.74731;
        IntFltCutoff = 0.73844;
        IntFltLen = 18;
        LPFltAlpha = 4.8149;
        LPFltBaseLen = 8.07999999999996;
        LPFltCutoffMult = 0.79335;
    }
};

struct CImageResizerParamsUltra : public CImageResizerParams {
    CImageResizerParamsUltra() {
        CorrFltAlpha = 0.99705;
        CorrFltLen = 7.42695;
        IntFltAlpha = 1.71985;
        IntFltCutoff = 0.7571;
        IntFltLen = 18;
        LPFltAlpha = 6.71313;
        LPFltBaseLen = 8.27999999999996;
        LPFltCutoffMult = 0.78413;
    }
};

class CImageResizerVarsBase {
   public:
    int ElCount;
    int ElCountIO;
    int fppack;
    int fpalign;
    int elalign;
    int packmode;
    int BufLen[2];
    int BufOffs[2];
    double k;
    double o;
    int ResizeStep;
    bool IsResize2;
    double InGammaMult;
    double OutGammaMult;
};

class CImageResizerVars : public CImageResizerVarsBase {
   public:
    double ox;
    double oy;
    CImageResizerThreadPool* ThreadPool;
    bool UseSRGBGamma;
    int AlphaIndex;
    int BuildMode;
    int RndSeed;

    CImageResizerVars()
        : ox(0.0), oy(0.0), ThreadPool(nullptr), UseSRGBGamma(false), AlphaIndex(-1), BuildMode(-1), RndSeed(0) {}
};

template <typename fptype, typename fptypeatom>
class CImageResizerFilterStep {
    AVIR_NOCTOR(CImageResizerFilterStep)

   public:
    bool IsUpsample;
    int ResampleFactor;
    CBuffer<fptype> Flt;
    CFltBuffer FltOrig;
    double DCGain;
    int FltLatency;
    const CImageResizerVars* Vars;
    int InLen;
    int InBuf;
    int InPrefix;
    int InSuffix;
    int InElIncr;
    int OutLen;
    int OutBuf;
    int OutPrefix;
    int OutSuffix;
    int OutElIncr;
    CBuffer<fptype> PrefixDC;
    CBuffer<fptype> SuffixDC;
    int EdgePixelCount;
    static const int EdgePixelCountDef = 3;

    struct CResizePos {
        int SrcPosInt;
        int fti;
        const fptype* ftp;
        fptypeatom x;
        int SrcOffs;
        int fl;
    };

    class CRPosBuf : public CBuffer<CResizePos> {
       public:
        double k;
        double o;
        int FracCount;
    };

    class CRPosBufArray : public CStructArray<CRPosBuf> {
       public:
        using CStructArray<CRPosBuf>::add;
        using CStructArray<CRPosBuf>::getItemCount;


        CRPosBuf& getRPosBuf(const double k, const double o, const int FracCount) {
            int i;

            for (i = 0; i < getItemCount(); i++) {
                CRPosBuf& Buf = (*this)[i];

                if (Buf.k == k && Buf.o == o && Buf.FracCount == FracCount) {
                    return (Buf);
                }
            }

            CRPosBuf& NewBuf = add();
            NewBuf.k = k;
            NewBuf.o = o;
            NewBuf.FracCount = FracCount;

            return (NewBuf);
        }
    };

    CRPosBuf* RPosBuf;
    const CDSPFracFilterBankLin<fptype>* FltBank;
    CDSPFracFilterBankLin<fptype>* FltBankDyn;

    CImageResizerFilterStep() {}
};

template <typename fptype, typename fptypeatom>
class CImageResizerFilterStepINL : public CImageResizerFilterStep<fptype, fptypeatom> {
   public:
    using CImageResizerFilterStep<fptype, fptypeatom>::IsUpsample;
    using CImageResizerFilterStep<fptype, fptypeatom>::ResampleFactor;
    using CImageResizerFilterStep<fptype, fptypeatom>::Flt;
    using CImageResizerFilterStep<fptype, fptypeatom>::FltOrig;
    using CImageResizerFilterStep<fptype, fptypeatom>::FltLatency;
    using CImageResizerFilterStep<fptype, fptypeatom>::Vars;
    using CImageResizerFilterStep<fptype, fptypeatom>::InLen;
    using CImageResizerFilterStep<fptype, fptypeatom>::InPrefix;
    using CImageResizerFilterStep<fptype, fptypeatom>::InSuffix;
    using CImageResizerFilterStep<fptype, fptypeatom>::OutLen;
    using CImageResizerFilterStep<fptype, fptypeatom>::OutPrefix;
    using CImageResizerFilterStep<fptype, fptypeatom>::OutSuffix;
    using CImageResizerFilterStep<fptype, fptypeatom>::PrefixDC;
    using CImageResizerFilterStep<fptype, fptypeatom>::SuffixDC;
    using CImageResizerFilterStep<fptype, fptypeatom>::RPosBuf;
    using CImageResizerFilterStep<fptype, fptypeatom>::FltBank;
    using CImageResizerFilterStep<fptype, fptypeatom>::EdgePixelCount;


    template <typename Tin>
    void packScanline(const Tin* ip, fptype* const op0, const int l0) const {
        const int ElCount = Vars->ElCount;
        const int ElCountIO = Vars->ElCountIO;
        fptype* op = op0;
        int l = l0;

        if (!Vars->UseSRGBGamma) {
            if (ElCountIO == 1) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)op;
                    v[0] = (fptypeatom)ip[0];
                    op += ElCount;
                    ip++;
                    l--;
                }
            } else if (ElCountIO == 4) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)op;
                    v[0] = (fptypeatom)ip[0];
                    v[1] = (fptypeatom)ip[1];
                    v[2] = (fptypeatom)ip[2];
                    v[3] = (fptypeatom)ip[3];
                    op += ElCount;
                    ip += 4;
                    l--;
                }
            } else if (ElCountIO == 3) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)op;
                    v[0] = (fptypeatom)ip[0];
                    v[1] = (fptypeatom)ip[1];
                    v[2] = (fptypeatom)ip[2];
                    op += ElCount;
                    ip += 3;
                    l--;
                }
            } else if (ElCountIO == 2) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)op;
                    v[0] = (fptypeatom)ip[0];
                    v[1] = (fptypeatom)ip[1];
                    op += ElCount;
                    ip += 2;
                    l--;
                }
            }
        } else {
            const fptypeatom gm = (fptypeatom)Vars->InGammaMult;

            if (ElCountIO == 1) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)op;
                    v[0] = convertSRGB2Lin(ip[0], gm);
                    op += ElCount;
                    ip++;
                    l--;
                }
            } else if (ElCountIO == 4) {
                if (Vars->AlphaIndex == 0) {
                    while (l > 0) {
                        fptypeatom* v = (fptypeatom*)op;
                        v[0] = (fptypeatom)ip[0] * gm;
                        v[1] = convertSRGB2Lin(ip[1], gm);
                        v[2] = convertSRGB2Lin(ip[2], gm);
                        v[3] = convertSRGB2Lin(ip[3], gm);
                        op += ElCount;
                        ip += 4;
                        l--;
                    }
                } else if (Vars->AlphaIndex == 3) {
                    while (l > 0) {
                        fptypeatom* v = (fptypeatom*)op;
                        v[0] = convertSRGB2Lin(ip[0], gm);
                        v[1] = convertSRGB2Lin(ip[1], gm);
                        v[2] = convertSRGB2Lin(ip[2], gm);
                        v[3] = (fptypeatom)ip[3] * gm;
                        op += ElCount;
                        ip += 4;
                        l--;
                    }
                } else {
                    while (l > 0) {
                        fptypeatom* v = (fptypeatom*)op;
                        v[0] = convertSRGB2Lin(ip[0], gm);
                        v[1] = convertSRGB2Lin(ip[1], gm);
                        v[2] = convertSRGB2Lin(ip[2], gm);
                        v[3] = convertSRGB2Lin(ip[3], gm);
                        op += ElCount;
                        ip += 4;
                        l--;
                    }
                }
            } else if (ElCountIO == 3) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)op;
                    v[0] = convertSRGB2Lin(ip[0], gm);
                    v[1] = convertSRGB2Lin(ip[1], gm);
                    v[2] = convertSRGB2Lin(ip[2], gm);
                    op += ElCount;
                    ip += 3;
                    l--;
                }
            } else if (ElCountIO == 2) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)op;
                    v[0] = convertSRGB2Lin(ip[0], gm);
                    v[1] = convertSRGB2Lin(ip[1], gm);
                    op += ElCount;
                    ip += 2;
                    l--;
                }
            }
        }

        const int ZeroCount = ElCount * Vars->fppack - ElCountIO;
        op = (fptype*)((fptypeatom*)op0 + ElCountIO);
        l = l0;

        if (ZeroCount == 1) {
            while (l > 0) {
                fptypeatom* v = (fptypeatom*)op;
                v[0] = (fptypeatom)0;
                op += ElCount;
                l--;
            }
        } else if (ZeroCount == 2) {
            while (l > 0) {
                fptypeatom* v = (fptypeatom*)op;
                v[0] = (fptypeatom)0;
                v[1] = (fptypeatom)0;
                op += ElCount;
                l--;
            }
        } else if (ZeroCount == 3) {
            while (l > 0) {
                fptypeatom* v = (fptypeatom*)op;
                v[0] = (fptypeatom)0;
                v[1] = (fptypeatom)0;
                v[2] = (fptypeatom)0;
                op += ElCount;
                l--;
            }
        }
    }


    static void applySRGBGamma(fptype* p, int l, const CImageResizerVars& Vars0) {
        const int ElCount = Vars0.ElCount;
        const int ElCountIO = Vars0.ElCountIO;
        const fptypeatom gm = (fptypeatom)Vars0.OutGammaMult;

        if (ElCountIO == 1) {
            while (l > 0) {
                fptypeatom* v = (fptypeatom*)p;
                v[0] = convertLin2SRGB(v[0]) * gm;
                p += ElCount;
                l--;
            }
        } else if (ElCountIO == 4) {
            if (Vars0.AlphaIndex == 0) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)p;
                    v[0] *= gm;
                    v[1] = convertLin2SRGB(v[1]) * gm;
                    v[2] = convertLin2SRGB(v[2]) * gm;
                    v[3] = convertLin2SRGB(v[3]) * gm;
                    p += ElCount;
                    l--;
                }
            } else if (Vars0.AlphaIndex == 3) {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)p;
                    v[0] = convertLin2SRGB(v[0]) * gm;
                    v[1] = convertLin2SRGB(v[1]) * gm;
                    v[2] = convertLin2SRGB(v[2]) * gm;
                    v[3] *= gm;
                    p += ElCount;
                    l--;
                }
            } else {
                while (l > 0) {
                    fptypeatom* v = (fptypeatom*)p;
                    v[0] = convertLin2SRGB(v[0]) * gm;
                    v[1] = convertLin2SRGB(v[1]) * gm;
                    v[2] = convertLin2SRGB(v[2]) * gm;
                    v[3] = convertLin2SRGB(v[3]) * gm;
                    p += ElCount;
                    l--;
                }
            }
        } else if (ElCountIO == 3) {
            while (l > 0) {
                fptypeatom* v = (fptypeatom*)p;
                v[0] = convertLin2SRGB(v[0]) * gm;
                v[1] = convertLin2SRGB(v[1]) * gm;
                v[2] = convertLin2SRGB(v[2]) * gm;
                p += ElCount;
                l--;
            }
        } else if (ElCountIO == 2) {
            while (l > 0) {
                fptypeatom* v = (fptypeatom*)p;
                v[0] = convertLin2SRGB(v[0]) * gm;
                v[1] = convertLin2SRGB(v[1]) * gm;
                p += ElCount;
                l--;
            }
        }
    }


    void convertVtoH(const fptype* ip, fptype* op, const int SrcLen, const int SrcIncr) const {
        const int ElCount = Vars->ElCount;
        int j;

        if (ElCount == 1) {
            for (j = 0; j < SrcLen; j++) {
                op[0] = ip[0];
                ip += SrcIncr;
                op++;
            }
        } else if (ElCount == 4) {
            for (j = 0; j < SrcLen; j++) {
                op[0] = ip[0];
                op[1] = ip[1];
                op[2] = ip[2];
                op[3] = ip[3];
                ip += SrcIncr;
                op += 4;
            }
        } else if (ElCount == 3) {
            for (j = 0; j < SrcLen; j++) {
                op[0] = ip[0];
                op[1] = ip[1];
                op[2] = ip[2];
                ip += SrcIncr;
                op += 3;
            }
        } else if (ElCount == 2) {
            for (j = 0; j < SrcLen; j++) {
                op[0] = ip[0];
                op[1] = ip[1];
                ip += SrcIncr;
                op += 2;
            }
        }
    }


    template <typename Tout>
    static void unpackScanline(const fptype* ip, Tout* op, int l, const CImageResizerVars& Vars0) {
        const int ElCount = Vars0.ElCount;
        const int ElCountIO = Vars0.ElCountIO;

        if (ElCountIO == 1) {
            while (l > 0) {
                const fptypeatom* v = (const fptypeatom*)ip;
                op[0] = (Tout)v[0];
                ip += ElCount;
                op++;
                l--;
            }
        } else if (ElCountIO == 4) {
            while (l > 0) {
                const fptypeatom* v = (const fptypeatom*)ip;
                op[0] = (Tout)v[0];
                op[1] = (Tout)v[1];
                op[2] = (Tout)v[2];
                op[3] = (Tout)v[3];
                ip += ElCount;
                op += 4;
                l--;
            }
        } else if (ElCountIO == 3) {
            while (l > 0) {
                const fptypeatom* v = (const fptypeatom*)ip;
                op[0] = (Tout)v[0];
                op[1] = (Tout)v[1];
                op[2] = (Tout)v[2];
                ip += ElCount;
                op += 3;
                l--;
            }
        } else if (ElCountIO == 2) {
            while (l > 0) {
                const fptypeatom* v = (const fptypeatom*)ip;
                op[0] = (Tout)v[0];
                op[1] = (Tout)v[1];
                ip += ElCount;
                op += 2;
                l--;
            }
        }
    }

    template <int Channels>
    static void accumulateFilterBlock(const fptype* const f, const int flen, const fptype* const ip, fptype*& op) {
        for (int i = 0; i < flen; i++) {
            op[0] += f[i] * ip[0];
            if (Channels > 1) {
                op[1] += f[i] * ip[1];
            }
            if (Channels > 2) {
                op[2] += f[i] * ip[2];
            }
            if (Channels > 3) {
                op[3] += f[i] * ip[3];
            }
            op += Channels;
        }
    }

    template <int Channels>
    static void accumulateDcBlock(const fptype* const ip, fptype*& op, const fptype*& dc, int& l) {
        while (l > 0) {
            op[0] += ip[0] * dc[0];
            if (Channels > 1) {
                op[1] += ip[1] * dc[0];
            }
            if (Channels > 2) {
                op[2] += ip[2] * dc[0];
            }
            if (Channels > 3) {
                op[3] += ip[3] * dc[0];
            }
            dc++;
            op += Channels;
            l--;
        }
    }

    template <int Channels>
    static void doSymmetricFilter(const fptype* const f, const int flen, const int ipstep, const fptype*& ip,
                                  fptype*& Dst, const int DstIncr, int& l) {
        while (l > 0) {
            fptype sum[Channels];
            for (int c = 0; c < Channels; ++c) {
                sum[c] = f[0] * ip[c];
            }

            const fptype* ip1 = ip;
            const fptype* ip2 = ip;

            for (int i = 1; i < flen; i++) {
                ip1 += Channels;
                ip2 -= Channels;

                for (int c = 0; c < Channels; ++c) {
                    sum[c] += f[i] * (ip1[c] + ip2[c]);
                }
            }

            for (int c = 0; c < Channels; ++c) {
                Dst[c] = sum[c];
            }

            Dst += DstIncr;
            ip += ipstep;
            l--;
        }
    }

    template <int Channels>
    static void copyUpsampleBlock(const fptype* const ip, fptype* const op0) {
        op0[0] = ip[0];
        if (Channels > 1) {
            op0[1] = ip[1];
        }
        if (Channels > 2) {
            op0[2] = ip[2];
        }
        if (Channels > 3) {
            op0[3] = ip[3];
        }
    }

    template <int Channels, bool AdvanceInput>
    static void runUpsampleCopyPass(const fptype*& ip, fptype*& op0, const int opstep, int l) {
        while (l > 0) {
            copyUpsampleBlock<Channels>(ip, op0);
            op0 += opstep;

            if (AdvanceInput) {
                ip += Channels;
            }

            l--;
        }
    }

    template <int Channels>
    static void doUnfilteredUpsample(const fptype*& ip, fptype*& op0, const int opstep, const int PrefixCount,
                                     const int MainCount, const int SuffixCount) {
        runUpsampleCopyPass<Channels, false>(ip, op0, opstep, PrefixCount);
        runUpsampleCopyPass<Channels, true>(ip, op0, opstep, MainCount);
        runUpsampleCopyPass<Channels, false>(ip, op0, opstep, SuffixCount);
    }

    template <int Channels, bool AdvanceInput>
    static void runUpsampleFilterPass(const fptype* const f, const int flen, const fptype*& ip, fptype*& op0,
                                      const int opstep, int l) {
        while (l > 0) {
            fptype* op = op0;
            accumulateFilterBlock<Channels>(f, flen, ip, op);
            op0 += opstep;

            if (AdvanceInput) {
                ip += Channels;
            }

            l--;
        }
    }

    template <int Channels>
    static void doFilteredUpsample(const fptype* const f, const int flen, const fptype*& ip, fptype*& op0,
                                   const int opstep, const int PrefixCount, const int MainCount,
                                   const int SuffixCount) {
        runUpsampleFilterPass<Channels, false>(f, flen, ip, op0, opstep, PrefixCount);
        runUpsampleFilterPass<Channels, true>(f, flen, ip, op0, opstep, MainCount);
        runUpsampleFilterPass<Channels, false>(f, flen, ip, op0, opstep, SuffixCount);
    }

    template <int Channels>
    static void applyDcAccumulation(const fptype* const ip, fptype*& op, const fptype*& dc, const int Len) {
        int l = Len;
        accumulateDcBlock<Channels>(ip, op, dc, l);
    }

    typedef CImageResizerFilterStepINL<fptype, fptypeatom> CThis;

    template <class Dispatcher>
    static void dispatchByElCount(const int ElCount, const Dispatcher& Dispatcher0) {
        switch (ElCount) {
            case 1:
                Dispatcher0.template run<1>();
                break;

            case 4:
                Dispatcher0.template run<4>();
                break;

            case 3:
                Dispatcher0.template run<3>();
                break;

            case 2:
            default:
                Dispatcher0.template run<2>();
                break;
        }
    }

    struct CUpsampleDispatcher {
        CUpsampleDispatcher(const CThis* const aSelf, const fptype* const aSrc, fptype* const aDst, fptype* const aOp0,
                            const int aOpStep)
            : Self(aSelf), Src(aSrc), Dst(aDst), Op0(aOp0), OpStep(aOpStep) {}

        template <int Channels>
        void run() const {
            Self->template doUpsampleImpl<Channels>(Src, Dst, Op0, OpStep);
        }

        const CThis* Self;
        const fptype* Src;
        fptype* Dst;
        fptype* Op0;
        int OpStep;
    };

    struct CFilterDispatcher {
        CFilterDispatcher(const fptype* const aF, const int aFLen, const int aIpStep, const fptype*& aIp, fptype*& aDst,
                          const int aDstIncr, int& aL)
            : F(aF), FLen(aFLen), IpStep(aIpStep), Ip(aIp), Dst(aDst), DstIncr(aDstIncr), L(aL) {}

        template <int Channels>
        void run() const {
            doSymmetricFilter<Channels>(F, FLen, IpStep, Ip, Dst, DstIncr, L);
        }

        const fptype* F;
        int FLen;
        int IpStep;
        const fptype*& Ip;
        fptype*& Dst;
        int DstIncr;
        int& L;
    };

    template <int ResampleFactor0, bool IsOrder1, bool IsResize2>
    struct CResizeDispatcher {
        CResizeDispatcher(const CThis* const aSelf, const fptype* const aSrcLine, fptype* const aDstLine,
                          const int aDstLineIncr, const int aFilterLen)
            : Self(aSelf), SrcLine(aSrcLine), DstLine(aDstLine), DstLineIncr(aDstLineIncr), FilterLen(aFilterLen) {}

        template <int Channels>
        void run() const {
            Self->template doResizeImpl<Channels, ResampleFactor0, IsOrder1, IsResize2>(SrcLine, DstLine, DstLineIncr,
                                                                                        FilterLen);
        }

        const CThis* Self;
        const fptype* SrcLine;
        fptype* DstLine;
        int DstLineIncr;
        int FilterLen;
    };


    void prepareInBuf(fptype* Src) const {
        if (IsUpsample || InPrefix + InSuffix == 0) {
            return;
        }

        const int ElCount = Vars->ElCount;
        replicateArray(Src, ElCount, Src - ElCount, InPrefix, -ElCount);

        Src += (InLen - 1) * ElCount;
        replicateArray(Src, ElCount, Src + ElCount, InSuffix, ElCount);
    }

    template <int Channels>
    void doUpsampleImpl(const fptype* const Src, fptype* const Dst, fptype* op0, const int opstep) const {
        const fptype* ip = Src;

        if (FltOrig.getCapacity() > 0) {
            op0 += (OutPrefix % ResampleFactor) * Channels;
            doUnfilteredUpsample<Channels>(ip, op0, opstep, OutPrefix / ResampleFactor, InLen - 1,
                                           OutSuffix / ResampleFactor + 1);

            return;
        }

        doFilteredUpsample<Channels>(Flt, Flt.getCapacity(), ip, op0, opstep, InPrefix, InLen - 1, InSuffix + 1);

        fptype* op = op0;
        const fptype* dc = SuffixDC;
        applyDcAccumulation<Channels>(ip, op, dc, SuffixDC.getCapacity());

        ip = Src;
        op = Dst - InPrefix * opstep;
        dc = PrefixDC;
        applyDcAccumulation<Channels>(ip, op, dc, PrefixDC.getCapacity());
    }


    void doUpsample(const fptype* const Src, fptype* const Dst) const {
        const int ElCount = Vars->ElCount;
        fptype* op0 = &Dst[-OutPrefix * ElCount];
        memset(op0, 0, (size_t)(OutPrefix + OutLen + OutSuffix) * (size_t)ElCount * sizeof(fptype));

        const int opstep = ElCount * ResampleFactor;
        const CUpsampleDispatcher Dispatcher0(this, Src, Dst, op0, opstep);
        dispatchByElCount(ElCount, Dispatcher0);
    }


    void doFilter(const fptype* const Src, fptype* Dst, const int DstIncr) const {
        const int ElCount = Vars->ElCount;
        const fptype* const f = &Flt[FltLatency];
        const int flen = FltLatency + 1;
        const int ipstep = ElCount * ResampleFactor;
        const fptype* ip = Src - EdgePixelCount * ipstep;
        int l = OutLen;
        const CFilterDispatcher Dispatcher0(f, flen, ipstep, ip, Dst, DstIncr, l);
        dispatchByElCount(ElCount, Dispatcher0);
    }

    template <int ElCount, int Step, bool Linear, bool UsePositionFilterLen>
    void doResizeImpl(const fptype* SrcLine, fptype* DstLine, const int DstLineIncr, const int FilterLenBase) const {
        const typename CImageResizerFilterStep<fptype, fptypeatom>::CResizePos* rpos = &(*RPosBuf)[0];
        const typename CImageResizerFilterStep<fptype, fptypeatom>::CResizePos* const rpose = rpos + OutLen;

        while (rpos < rpose) {
            const fptype x = (fptype)rpos->x;
            const fptype* const ftp = rpos->ftp;
            const int IntFltLen = UsePositionFilterLen ? rpos->fl : FilterLenBase;
            const fptype* const ftp2 = ftp + FilterLenBase;
            const fptype* Src = SrcLine + rpos->SrcOffs;
            fptype sum[ElCount] = {};

            for (int i = 0; i < IntFltLen; i += Step) {
                const fptype xx = Linear ? ftp[i] + ftp2[i] * x : ftp[i];

                for (int c = 0; c < ElCount; ++c) {
                    sum[c] += xx * Src[c];
                }

                Src += ElCount * Step;
            }

            for (int c = 0; c < ElCount; ++c) {
                DstLine[c] = sum[c];
            }

            DstLine += DstLineIncr;
            rpos++;
        }
    }

    template <int ResampleFactor, bool IsOrder1, bool IsResize2>
    void dispatchResizeImpl(const fptype* SrcLine, fptype* DstLine, const int DstLineIncr, const int FilterLen) const {
        const int ElCount = Vars->ElCount;
        const CResizeDispatcher<ResampleFactor, IsOrder1, IsResize2> Dispatcher0(this, SrcLine, DstLine, DstLineIncr,
                                                                                 FilterLen);
        dispatchByElCount(ElCount, Dispatcher0);
    }

    template <int ResampleFactor, bool IsResize2>
    void dispatchResize(const fptype* SrcLine, fptype* DstLine, const int DstLineIncr) const {
        const int FilterLen = FltBank->getFilterLen();
        if (FltBank->getOrder() == 1) {
            dispatchResizeImpl<ResampleFactor, true, IsResize2>(SrcLine, DstLine, DstLineIncr, FilterLen);
        } else {
            dispatchResizeImpl<ResampleFactor, false, IsResize2>(SrcLine, DstLine, DstLineIncr, FilterLen);
        }
    }


    void doResize(const fptype* SrcLine, fptype* DstLine, const int DstLineIncr, fptype* const) const {
        dispatchResize<1, false>(SrcLine, DstLine, DstLineIncr);
    }


    void doResize2(const fptype* SrcLine, fptype* DstLine, const int DstLineIncr, fptype* const) const {
        dispatchResize<2, true>(SrcLine, DstLine, DstLineIncr);
    }
};

template <typename fptype>
class CImageResizerDithererDefINL {
   public:

    void init(const int aLen, const CImageResizerVars& aVars, const double aTrMul, const double aPkOut) {
        Len = aLen;
        Vars = &aVars;
        LenE = aLen * Vars->ElCount;
        TrMul0 = aTrMul;
        PkOut0 = aPkOut;
    }

    static bool isRecursive() {
        return (false);
    }


    void dither(fptype* const ResScanline) const {
        const fptype c0 = 0;
        const fptype PkOut = (fptype)PkOut0;
        int j;

        if (TrMul0 == 1.0) {
            for (j = 0; j < LenE; j++) {
                ResScanline[j] = clamp(round(ResScanline[j]), c0, PkOut);
            }
        } else {
            const fptype TrMul = (fptype)TrMul0;
            const fptype TrMulI = (fptype)(1.0 / TrMul0);

            for (j = 0; j < LenE; j++) {
                const fptype z0 = round(ResScanline[j] * TrMulI) * TrMul;
                ResScanline[j] = clamp(z0, c0, PkOut);
            }
        }
    }

   protected:
    int Len;
    const CImageResizerVars* Vars;
    int LenE;
    double TrMul0;
    double PkOut0;
};

template <typename fptype>
class CImageResizerDithererErrdINL : public CImageResizerDithererDefINL<fptype> {
   public:

    void init(const int aLen, const CImageResizerVars& aVars, const double aTrMul, const double aPkOut) {
        CImageResizerDithererDefINL<fptype>::init(aLen, aVars, aTrMul, aPkOut);

        ResScanlineDith0.alloc(LenE + Vars->ElCount, sizeof(fptype));
        ResScanlineDith = ResScanlineDith0 + Vars->ElCount;
        int i;

        for (i = 0; i < LenE + Vars->ElCount; i++) {
            ResScanlineDith0[i] = 0;
        }
    }

    static bool isRecursive() {
        return (true);
    }

    void dither(fptype* const ResScanline) {
        const int ElCount = Vars->ElCount;
        const fptype c0 = 0;
        const fptype TrMul = (fptype)TrMul0;
        const fptype TrMulI = (fptype)(1.0 / TrMul0);
        const fptype PkOut = (fptype)PkOut0;
        int j;

        for (j = 0; j < LenE; j++) {
            ResScanline[j] += ResScanlineDith[j];
            ResScanlineDith[j] = 0;
        }

        for (j = 0; j < LenE - ElCount; j++) {
            const fptype z0 = round(ResScanline[j] * TrMulI) * TrMul;
            const fptype Noise = ResScanline[j] - z0;
            ResScanline[j] = clamp(z0, c0, PkOut);

            const fptype NoiseM1 = Noise * (fptype)0.364842;
            ResScanline[j + ElCount] += NoiseM1;
            ResScanlineDith[j - ElCount] += Noise * (fptype)0.207305;
            ResScanlineDith[j] += NoiseM1;
            ResScanlineDith[j + ElCount] += Noise * (fptype)0.063011;
        }

        while (j < LenE) {
            const fptype z0 = round(ResScanline[j] * TrMulI) * TrMul;
            const fptype Noise = ResScanline[j] - z0;
            ResScanline[j] = clamp(z0, c0, PkOut);

            ResScanlineDith[j - ElCount] += Noise * (fptype)0.207305;
            ResScanlineDith[j] += Noise * (fptype)0.364842;
            j++;
        }
    }

   protected:
    using CImageResizerDithererDefINL<fptype>::Len;
    using CImageResizerDithererDefINL<fptype>::Vars;
    using CImageResizerDithererDefINL<fptype>::LenE;
    using CImageResizerDithererDefINL<fptype>::TrMul0;
    using CImageResizerDithererDefINL<fptype>::PkOut0;

    CBuffer<fptype> ResScanlineDith0;
    fptype* ResScanlineDith;
};

template <typename afptype, typename afptypeatom = afptype, class adith = CImageResizerDithererDefINL<afptype>>
class fpclass_def {
   public:
    typedef afptype fptype;
    typedef afptypeatom fptypeatom;
    static const int fppack = sizeof(fptype) / sizeof(fptypeatom);
    static const int fpalign = sizeof(fptype);
    static const int elalign = 1;
    static const int packmode = 0;
    typedef CImageResizerFilterStepINL<fptype, fptypeatom> CFilterStep;
    typedef adith CDitherer;
};

template <class fpclass = fpclass_def<float>>
class CImageResizer {
    AVIR_NOCTOR(CImageResizer)

   public:

    CImageResizer(const int aResBitDepth = 8, const int aSrcBitDepth = 0,
                  const CImageResizerParams& aParams = CImageResizerParamsDef())
        : Params(aParams), ResBitDepth(aResBitDepth) {
        SrcBitDepth = (aSrcBitDepth == 0 ? ResBitDepth : aSrcBitDepth);

        initFilterBank(FixedFilterBank, 1.0, false, CFltBuffer());
        FixedFilterBank.createAllFilters();
    }


    template <typename Tin, typename Tout>
    void resizeImage(const Tin* const SrcBuf, const int SrcWidth, const int SrcHeight, int SrcScanlineSize,
                     Tout* const NewBuf, const int NewWidth, const int NewHeight, const int ElCountIO, const double k,
                     CImageResizerVars* const aVars = nullptr) const {
        if (SrcWidth == 0 || SrcHeight == 0) {
            memset(NewBuf, 0, (size_t)NewWidth * (size_t)NewHeight * sizeof(Tout));

            return;
        } else if (NewWidth == 0 || NewHeight == 0) {
            return;
        }

        CImageResizerVars DefVars;
        CImageResizerVars& Vars = (aVars == nullptr ? DefVars : *aVars);

        CImageResizerThreadPool DefThreadPool;
        CImageResizerThreadPool& ThreadPool = (Vars.ThreadPool == nullptr ? DefThreadPool : *Vars.ThreadPool);

        double kx;
        double ky;
        double ox = Vars.ox;
        double oy = Vars.oy;

        if (k == 0.0) {
            kx = (double)SrcWidth / NewWidth;
            ox += (kx - 1.0) * 0.5;

            ky = (double)SrcHeight / NewHeight;
            oy += (ky - 1.0) * 0.5;
        } else if (k > 0.0) {
            kx = k;
            ky = k;

            const double ko = (k - 1.0) * 0.5;
            ox += ko;
            oy += ko;
        } else {
            kx = -k;
            ky = -k;
        }

        const bool IsInFloat = ((Tin)0.25 != 0);
        const bool IsOutFloat = ((Tout)0.25 != 0);
        double OutMul;

        if (Vars.UseSRGBGamma) {
            if (IsInFloat) {
                Vars.InGammaMult = 1.0;
            } else {
                Vars.InGammaMult = 1.0 / (sizeof(Tin) == 1 ? 255.0 : 65535.0);
            }

            if (IsOutFloat) {
                Vars.OutGammaMult = 1.0;
            } else {
                Vars.OutGammaMult = (sizeof(Tout) == 1 ? 255.0 : 65535.0);
            }

            OutMul = 1.0;
        } else {
            if (IsOutFloat) {
                OutMul = 1.0;
            } else {
                OutMul = (sizeof(Tout) == 1 ? 255.0 : 65535.0);
            }

            if (!IsInFloat) {
                OutMul /= (sizeof(Tin) == 1 ? 255.0 : 65535.0);
            }
        }

        const int ElCount = (ElCountIO + fpclass ::fppack - 1) / fpclass ::fppack;

        const int NewWidthE = NewWidth * ElCount;

        if (SrcScanlineSize < 1) {
            SrcScanlineSize = SrcWidth * ElCountIO;
        }

        Vars.ElCount = ElCount;
        Vars.ElCountIO = ElCountIO;
        Vars.fppack = fpclass ::fppack;
        Vars.fpalign = fpclass ::fpalign;
        Vars.elalign = fpclass ::elalign;
        Vars.packmode = fpclass ::packmode;

        CDSPFracFilterBankLin<fptype> FltBank;
        CFilterSteps FltSteps;
        typename CFilterStep ::CRPosBufArray RPosBufArray;
        CBuffer<char> UsedFracMap;

        int UseBuildMode = 1;
        const int BuildModeCount = (FixedFilterBank.getOrder() == 0 ? 4 : 2);

        int m;

        if (Vars.BuildMode >= 0) {
            UseBuildMode = Vars.BuildMode;
        } else {
            int BestScore = 0x7FFFFFFF;

            for (m = 0; m < BuildModeCount; m++) {
                CDSPFracFilterBankLin<fptype> TmpBank;
                CFilterSteps TmpSteps;
                Vars.k = kx;
                Vars.o = ox;
                buildFilterSteps(TmpSteps, Vars, TmpBank, OutMul, m, true);
                updateFilterStepBuffers(TmpSteps, Vars, RPosBufArray, SrcWidth, NewWidth);

                fillUsedFracMap(TmpSteps[Vars.ResizeStep], UsedFracMap);
                const int c = calcComplexity(TmpSteps, Vars, UsedFracMap, SrcHeight);

                if (c < BestScore) {
                    UseBuildMode = m;
                    BestScore = c;
                }
            }
        }

        Vars.k = kx;
        Vars.o = ox;
        buildFilterSteps(FltSteps, Vars, FltBank, OutMul, UseBuildMode, false);

        updateFilterStepBuffers(FltSteps, Vars, RPosBufArray, SrcWidth, NewWidth);

        updateBufLenAndRPosPtrs(FltSteps, Vars, NewWidth);

        const int ThreadCount = ThreadPool.getSuggestedWorkloadCount();

        CStructArray<CThreadData<Tin, Tout>> td;
        td.setItemCount(ThreadCount);
        int i;

        for (i = 0; i < ThreadCount; i++) {
            if (i > 0) {
                ThreadPool.addWorkload(&td[i]);
            }

            td[i].init(i, ThreadCount, FltSteps, Vars);

            td[i].initScanlineQueue(td[i].sopResizeH, SrcHeight, SrcWidth);
        }

        CBuffer<fptype, size_t> FltBuf((size_t)NewWidthE * (size_t)SrcHeight, fpclass ::fpalign);

        for (i = 0; i < SrcHeight; i++) {
            td[i % ThreadCount].addScanlineToQueue((void*)&SrcBuf[(size_t)i * (size_t)SrcScanlineSize],
                                                   &FltBuf[(size_t)i * (size_t)NewWidthE]);
        }

        ThreadPool.startAllWorkloads();
        td[0].processScanlineQueue();
        ThreadPool.waitAllWorkloadsToFinish();

        const int PrevUseBuildMode = UseBuildMode;

        if (Vars.BuildMode >= 0) {
            UseBuildMode = Vars.BuildMode;
        } else {
            CImageResizerVars TmpVars(Vars);
            int BestScore = 0x7FFFFFFF;

            for (m = 0; m < BuildModeCount; m++) {
                CDSPFracFilterBankLin<fptype> TmpBank;
                TmpBank.copyInitParams(FltBank);
                CFilterSteps TmpSteps;
                TmpVars.k = ky;
                TmpVars.o = oy;
                buildFilterSteps(TmpSteps, TmpVars, TmpBank, 1.0, m, true);
                updateFilterStepBuffers(TmpSteps, TmpVars, RPosBufArray, SrcHeight, NewHeight);

                fillUsedFracMap(TmpSteps[TmpVars.ResizeStep], UsedFracMap);

                const int c = calcComplexity(TmpSteps, TmpVars, UsedFracMap, NewWidth);

                if (c < BestScore) {
                    UseBuildMode = m;
                    BestScore = c;
                }
            }
        }

        Vars.k = ky;
        Vars.o = oy;

        if (UseBuildMode == PrevUseBuildMode && ky == kx) {
            if (OutMul != 1.0) {
                modifyCorrFilterDCGain(FltSteps, 1.0 / OutMul);
            }
        } else {
            buildFilterSteps(FltSteps, Vars, FltBank, 1.0, UseBuildMode, false);
        }

        updateFilterStepBuffers(FltSteps, Vars, RPosBufArray, SrcHeight, NewHeight);

        updateBufLenAndRPosPtrs(FltSteps, Vars, NewWidth);

        if (IsOutFloat && sizeof(FltBuf[0]) == sizeof(Tout) && fpclass ::packmode == 0) {
            for (i = 0; i < ThreadCount; i++) {
                td[i].initScanlineQueue(td[i].sopResizeV, NewWidth, SrcHeight, NewWidthE, NewWidthE);
            }

            for (i = 0; i < NewWidth; i++) {
                td[i % ThreadCount].addScanlineToQueue(&FltBuf[i * ElCount], (fptype*)&NewBuf[i * ElCount]);
            }

            ThreadPool.startAllWorkloads();
            td[0].processScanlineQueue();
            ThreadPool.waitAllWorkloadsToFinish();
            ThreadPool.removeAllWorkloads();

            return;
        }

        CBuffer<fptype, size_t> ResBuf((size_t)NewWidthE * (size_t)NewHeight, fpclass ::fpalign);

        for (i = 0; i < ThreadCount; i++) {
            td[i].initScanlineQueue(td[i].sopResizeV, NewWidth, SrcHeight, NewWidthE, NewWidthE);
        }

        const int im = (fpclass ::packmode == 0 ? ElCount : 1);

        for (i = 0; i < NewWidth; i++) {
            td[i % ThreadCount].addScanlineToQueue(&FltBuf[i * im], &ResBuf[i * im]);
        }

        ThreadPool.startAllWorkloads();
        td[0].processScanlineQueue();
        ThreadPool.waitAllWorkloadsToFinish();

        if (IsOutFloat) {
            for (i = 0; i < ThreadCount; i++) {
                td[i].initScanlineQueue(td[i].sopUnpackH, NewHeight, NewWidth);
            }

            for (i = 0; i < NewHeight; i++) {
                td[i % ThreadCount].addScanlineToQueue(&ResBuf[(size_t)i * (size_t)NewWidthE],
                                                       &NewBuf[(size_t)i * (size_t)(NewWidth * ElCountIO)]);
            }

            ThreadPool.startAllWorkloads();
            td[0].processScanlineQueue();
            ThreadPool.waitAllWorkloadsToFinish();
            ThreadPool.removeAllWorkloads();

            return;
        }

        int TruncBits;
        int OutRange;

        if (sizeof(Tout) == 1) {
            TruncBits = 8 - ResBitDepth;
            OutRange = 255;
        } else {
            TruncBits = 16 - ResBitDepth;
            OutRange = 65535;
        }

        const double PkOut = OutRange;
        const double TrMul = (TruncBits > 0 ? PkOut / (OutRange >> TruncBits) : 1.0);

        if (CDitherer ::isRecursive()) {
            td[0].getDitherer().init(NewWidth, Vars, TrMul, PkOut);

            for (i = 0; i < NewHeight; i++) {
                fptype* const ResScanline = &ResBuf[(size_t)i * (size_t)NewWidthE];

                if (Vars.UseSRGBGamma) {
                    CFilterStep ::applySRGBGamma(ResScanline, NewWidth, Vars);
                }

                td[0].getDitherer().dither(ResScanline);

                CFilterStep ::unpackScanline(ResScanline, &NewBuf[(size_t)i * (size_t)(NewWidth * ElCountIO)], NewWidth,
                                             Vars);
            }
        } else {
            for (i = 0; i < ThreadCount; i++) {
                td[i].initScanlineQueue(td[i].sopDitherAndUnpackH, NewHeight, NewWidth);

                td[i].getDitherer().init(NewWidth, Vars, TrMul, PkOut);
            }

            for (i = 0; i < NewHeight; i++) {
                td[i % ThreadCount].addScanlineToQueue(&ResBuf[(size_t)i * (size_t)NewWidthE],
                                                       &NewBuf[(size_t)i * (size_t)(NewWidth * ElCountIO)]);
            }

            ThreadPool.startAllWorkloads();
            td[0].processScanlineQueue();
            ThreadPool.waitAllWorkloadsToFinish();
        }

        ThreadPool.removeAllWorkloads();
    }

   private:
    typedef typename fpclass ::fptype fptype;
    typedef typename fpclass ::CFilterStep CFilterStep;
    typedef typename fpclass ::CDitherer CDitherer;
    CImageResizerParams Params;
    int SrcBitDepth;
    int ResBitDepth;
    CDSPFracFilterBankLin<fptype> FixedFilterBank;

    typedef CStructArray<CFilterStep> CFilterSteps;


    void initFilterBank(CDSPFracFilterBankLin<fptype>& FltBank, const double CutoffMult, const bool ForceHiOrder,
                        const CFltBuffer& ExtFilter) const {
        const int IntBitDepth = (ResBitDepth > SrcBitDepth ? ResBitDepth : SrcBitDepth);

        const double SNR = -6.02 * (IntBitDepth + 3);
        int UseOrder;
        int FracCount;

        if (ForceHiOrder || IntBitDepth > 8) {
            UseOrder = 1;
            FracCount = (int)ceil(0.23134052 * exp(-0.058062929 * SNR));
        } else {
            UseOrder = 0;
            FracCount = (int)ceil(0.33287686 * exp(-0.11334583 * SNR));
        }

        if (FracCount < 2) {
            FracCount = 2;
        }

        FltBank.init(FracCount, UseOrder, Params.IntFltLen / CutoffMult, Params.IntFltCutoff * CutoffMult,
                     Params.IntFltAlpha, ExtFilter, fpclass ::fpalign, fpclass ::elalign);
    }


    static void allocFilter(CBuffer<fptype>& Flt, const int ReqCapacity, const bool IsModel = false,
                            int* const FltExt = nullptr) {
        int UseCapacity = (ReqCapacity + fpclass ::elalign - 1) & ~(fpclass ::elalign - 1);

        int Ext = UseCapacity - ReqCapacity;

        if (FltExt != nullptr) {
            *FltExt = Ext;
        }

        if (IsModel) {
            Flt.forceCapacity(UseCapacity);
            return;
        }

        Flt.alloc(UseCapacity, fpclass ::fpalign);

        while (Ext > 0) {
            Ext--;
            Flt[ReqCapacity + Ext] = 0;
        }
    }


    void assignFilterParams(CFilterStep& fs, const bool IsUpsample, const int ResampleFactor, const double FltCutoff,
                            const double DCGain, const bool UseFltOrig, const bool IsModel) const {
        double FltAlpha;
        double Len2;
        double Freq;

        if (FltCutoff == 0.0) {
            const double m = 2.0 / ResampleFactor;
            FltAlpha = Params.HBFltAlpha;
            Len2 = 0.5 * Params.HBFltLen / m;
            Freq = AVIR_PI * Params.HBFltCutoff * m;
        } else {
            FltAlpha = Params.LPFltAlpha;
            Len2 = 0.25 * Params.LPFltBaseLen / FltCutoff;
            Freq = AVIR_PI * Params.LPFltCutoffMult * FltCutoff;
        }

        if (IsUpsample) {
            Len2 *= ResampleFactor;
            Freq /= ResampleFactor;
            fs.DCGain = DCGain * ResampleFactor;
        } else {
            fs.DCGain = DCGain;
        }

        fs.FltOrig.Len2 = Len2;
        fs.FltOrig.Freq = Freq;
        fs.FltOrig.Alpha = FltAlpha;
        fs.FltOrig.DCGain = fs.DCGain;

        CDSPPeakedCosineLPF w(Len2, Freq, FltAlpha);

        fs.IsUpsample = IsUpsample;
        fs.ResampleFactor = ResampleFactor;
        fs.FltLatency = w.fl2;

        int FltExt;

        if (IsModel) {
            allocFilter(fs.Flt, w.FilterLen, true, &FltExt);

            if (UseFltOrig) {
                fs.FltOrig.alloc(w.FilterLen);
                memset(&fs.FltOrig[0], 0, (size_t)w.FilterLen * sizeof(fs.FltOrig[0]));
            }
        } else {
            fs.FltOrig.alloc(w.FilterLen);

            w.generateLPF(&fs.FltOrig[0], fs.DCGain);

            allocFilter(fs.Flt, fs.FltOrig.getCapacity(), false, &FltExt);
            copyArray(&fs.FltOrig[0], &fs.Flt[0], fs.FltOrig.getCapacity());

            if (!UseFltOrig) {
                fs.FltOrig.free();
            }
        }

        if (IsUpsample) {
            int l = fs.Flt.getCapacity() - fs.FltLatency - ResampleFactor - FltExt;

            allocFilter(fs.PrefixDC, l, IsModel);
            allocFilter(fs.SuffixDC, fs.FltLatency, IsModel);

            if (IsModel) {
                return;
            }

            const fptype* ip = &fs.Flt[fs.FltLatency + ResampleFactor];
            copyArray(ip, &fs.PrefixDC[0], l);

            while (true) {
                ip += ResampleFactor;
                l -= ResampleFactor;

                if (l <= 0) {
                    break;
                }

                addArray(ip, &fs.PrefixDC[0], l);
            }

            l = fs.FltLatency;
            fptype* op = &fs.SuffixDC[0];
            copyArray(&fs.Flt[0], op, l);

            while (true) {
                op += ResampleFactor;
                l -= ResampleFactor;

                if (l <= 0) {
                    break;
                }

                addArray(&fs.Flt[0], op, l);
            }
        } else if (!UseFltOrig) {
            fs.EdgePixelCount = fs.EdgePixelCountDef;
        }
    }


    void addCorrectionFilter(CFilterSteps& Steps, const double bw, const bool IsPreCorrection,
                             const bool IsModel) const {
        CFilterStep& nfs = (IsPreCorrection ? Steps[0] : Steps.add());
        nfs.IsUpsample = false;
        nfs.ResampleFactor = 1;
        nfs.DCGain = 1.0;
        nfs.EdgePixelCount = (IsPreCorrection ? nfs.EdgePixelCountDef : 0);

        if (IsModel) {
            allocFilter(nfs.Flt, CDSPFIREQ ::calcFilterLength(Params.CorrFltLen, nfs.FltLatency), true);

            return;
        }

        const int BinCount = 65;
        const int BinCount1 = BinCount - 1;
        double curbw = 1.0;
        int i;
        int j;
        double re;
        double im;

        CBuffer<double> Bins(BinCount);

        for (j = 0; j < BinCount; j++) {
            Bins[j] = 1.0;
        }

        const int si = (IsPreCorrection ? 1 : 0);

        for (i = si; i < Steps.getItemCount() - (si ^ 1); i++) {
            const CFilterStep& fs = Steps[i];

            if (fs.IsUpsample) {
                curbw *= fs.ResampleFactor;

                if (fs.FltOrig.getCapacity() > 0) {
                    continue;
                }
            }

            const fptype* Flt;
            int FltLen;

            if (fs.ResampleFactor == 0) {
                if (fs.FltBankDyn == nullptr) {
                    Flt = fs.FltBank->getFilterConst(0);
                    FltLen = fs.FltBank->getFilterLen();
                } else {
                    Flt = fs.FltBankDyn->getFilter(0);
                    FltLen = fs.FltBankDyn->getFilterLen();
                }
            } else {
                Flt = &fs.Flt[0];
                FltLen = fs.Flt.getCapacity();
            }

            const double thm = AVIR_PI * bw / (curbw * BinCount1);

            for (j = 0; j < BinCount; j++) {
                calcFIRFilterResponse(Flt, FltLen, j * thm, re, im);

                Bins[j] *= fs.DCGain / sqrt(re * re + im * im);
            }

            if (!fs.IsUpsample && fs.ResampleFactor > 1) {
                curbw /= fs.ResampleFactor;
            }
        }

        CDSPFIREQ EQ;
        EQ.init(bw * 2.0, Params.CorrFltLen, BinCount, 0.0, bw, false, Params.CorrFltAlpha);

        nfs.FltLatency = EQ.getFilterLatency();

        CBuffer<double> Filter(EQ.getFilterLength());
        EQ.buildFilter(Bins, &Filter[0]);
        normalizeFIRFilter(&Filter[0], Filter.getCapacity(), 1.0);

        allocFilter(nfs.Flt, Filter.getCapacity());
        copyArray(&Filter[0], &nfs.Flt[0], Filter.getCapacity());
    }


    static void addSharpenTest(CFilterSteps& Steps, const double bw, const bool IsModel) {
        if (bw <= 1.0) {
            return;
        }

        const double FltLen = 10.0 * bw;

        CFilterStep& fs = Steps.add();
        fs.IsUpsample = false;
        fs.ResampleFactor = 1;
        fs.DCGain = 1.0;
        fs.EdgePixelCount = 0;

        if (IsModel) {
            allocFilter(fs.Flt, CDSPFIREQ ::calcFilterLength(FltLen, fs.FltLatency), true);

            return;
        }

        const int BinCount = 200;
        CBuffer<double> Bins(BinCount);
        int Thresh = (int)round(BinCount / bw * 1.75);

        if (Thresh > BinCount) {
            Thresh = BinCount;
        }

        int j;

        for (j = 0; j < Thresh; j++) {
            Bins[j] = 1.0;
        }

        for (j = Thresh; j < BinCount; j++) {
            Bins[j] = 256.0;
        }

        CDSPFIREQ EQ;
        EQ.init(bw * 2.0, FltLen, BinCount, 0.0, bw, false, 1.7);

        fs.FltLatency = EQ.getFilterLatency();

        CBuffer<double> Filter(EQ.getFilterLength());
        EQ.buildFilter(Bins, &Filter[0]);
        normalizeFIRFilter(&Filter[0], Filter.getCapacity(), 1.0);

        allocFilter(fs.Flt, Filter.getCapacity());
        copyArray(&Filter[0], &fs.Flt[0], Filter.getCapacity());
    }


    void buildFilterSteps(CFilterSteps& Steps, CImageResizerVars& Vars, CDSPFracFilterBankLin<fptype>& FltBank,
                          const double DCGain, const int ModeFlags, const bool IsModel) const {
        Steps.clear();

        const bool DoFltAndIntCombo = ((ModeFlags & 1) != 0);
        const bool ForceHiOrderInt = ((ModeFlags & 2) != 0);
        const bool UseHalfband = ((ModeFlags & 4) != 0);

        const double bw = 1.0 / Vars.k;
        const int UpsampleFactor = ((int)floor(Vars.k) < 2 ? 2 : 1);
        double IntCutoffMult;
        CFilterStep* ReuseStep;
        CFilterStep* ExtFltStep;
        bool IsPreCorrection;
        double FltCutoff;
        double corrbw;

        if (Vars.k <= 1.0) {
            IsPreCorrection = true;
            FltCutoff = 1.0;
            corrbw = 1.0;
            Steps.add();
        } else {
            IsPreCorrection = false;
            FltCutoff = bw;
            corrbw = bw;
        }

        if (UpsampleFactor > 1) {
            CFilterStep& fs = Steps.add();
            assignFilterParams(fs, true, UpsampleFactor, FltCutoff, DCGain, DoFltAndIntCombo, IsModel);

            IntCutoffMult = FltCutoff * 2.0 / UpsampleFactor;
            ReuseStep = nullptr;
            ExtFltStep = (DoFltAndIntCombo ? &fs : nullptr);
        } else {
            int DownsampleFactor;

            while (true) {
                DownsampleFactor = (int)floor(0.5 / FltCutoff);
                bool DoHBFltAdd = (UseHalfband && DownsampleFactor > 1);

                if (DoHBFltAdd) {
                    assignFilterParams(Steps.add(), false, DownsampleFactor, 0.0, 1.0, false, IsModel);

                    FltCutoff *= DownsampleFactor;
                } else {
                    if (DownsampleFactor < 1) {
                        DownsampleFactor = 1;
                    }

                    break;
                }
            }

            CFilterStep& fs = Steps.add();
            assignFilterParams(fs, false, DownsampleFactor, FltCutoff, DCGain, DoFltAndIntCombo, IsModel);

            IntCutoffMult = FltCutoff / 0.5;

            if (DoFltAndIntCombo) {
                ReuseStep = &fs;
                ExtFltStep = &fs;
            } else {
                IntCutoffMult *= DownsampleFactor;
                ReuseStep = nullptr;
                ExtFltStep = nullptr;
            }
        }

        CFilterStep& fs = (ReuseStep == nullptr ? Steps.add() : *ReuseStep);

        Vars.ResizeStep = Steps.getItemCount() - 1;
        fs.IsUpsample = false;
        fs.ResampleFactor = 0;
        fs.DCGain = (ExtFltStep == nullptr ? 1.0 : ExtFltStep->DCGain);

        initFilterBank(FltBank, IntCutoffMult, ForceHiOrderInt,
                       (ExtFltStep == nullptr ? fs.FltOrig : ExtFltStep->FltOrig));

        if (FltBank == FixedFilterBank) {
            fs.FltBank = &FixedFilterBank;
            fs.FltBankDyn = nullptr;
        } else {
            fs.FltBank = &FltBank;
            fs.FltBankDyn = &FltBank;
        }

        addCorrectionFilter(Steps, corrbw, IsPreCorrection, IsModel);
    }


    static void extendUpsample(CFilterStep& fs, CFilterStep& NextStep) {
        fs.InPrefix = (NextStep.InPrefix + fs.ResampleFactor - 1) / fs.ResampleFactor;

        fs.OutPrefix += fs.InPrefix * fs.ResampleFactor;
        NextStep.InPrefix = 0;

        fs.InSuffix = (NextStep.InSuffix + fs.ResampleFactor - 1) / fs.ResampleFactor;

        fs.OutSuffix += fs.InSuffix * fs.ResampleFactor;
        NextStep.InSuffix = 0;
    }


    static void fillRPosBuf(CFilterStep& fs, const CImageResizerVars& Vars) {
        const int PrevLen = fs.RPosBuf->getCapacity();

        if (fs.OutLen > PrevLen) {
            fs.RPosBuf->increaseCapacity(fs.OutLen);
        }

        typename CFilterStep ::CResizePos* rpos = &(*fs.RPosBuf)[PrevLen];
        const int FracCount = fs.FltBank->getFracCount();
        const double o = Vars.o;
        const double k = Vars.k;
        int i;

        for (i = PrevLen; i < fs.OutLen; i++) {
            const double SrcPos = o + k * i;
            const int SrcPosInt = (int)floor(SrcPos);
            const double x = (SrcPos - SrcPosInt) * FracCount;
            const int fti = (int)x;
            rpos->x = (typename fpclass ::fptypeatom)(x - fti);
            rpos->fti = fti;
            rpos->SrcPosInt = SrcPosInt;
            rpos++;
        }
    }


    static void updateFilterStepBuffers(CFilterSteps& Steps, CImageResizerVars& Vars,
                                        typename CFilterStep ::CRPosBufArray& RPosBufArray, int SrcLen,
                                        const int NewLen) {
        int upstep = -1;
        int InBuf = 0;
        int i;

        for (i = 0; i < Steps.getItemCount(); i++) {
            CFilterStep& fs = Steps[i];

            fs.Vars = &Vars;
            fs.InLen = SrcLen;
            fs.InBuf = InBuf;
            fs.OutBuf = (InBuf + 1) & 1;

            if (fs.IsUpsample) {
                upstep = i;
                Vars.k *= fs.ResampleFactor;
                Vars.o *= fs.ResampleFactor;
                fs.InPrefix = 0;
                fs.InSuffix = 0;
                fs.OutLen = fs.InLen * fs.ResampleFactor;
                fs.OutPrefix = fs.FltLatency;
                fs.OutSuffix = fs.Flt.getCapacity() - fs.FltLatency - fs.ResampleFactor;

                int l0 = fs.OutPrefix + fs.OutLen + fs.OutSuffix;
                int l = fs.InLen * fs.ResampleFactor + fs.SuffixDC.getCapacity();

                if (l > l0) {
                    fs.OutSuffix += l - l0;
                }

                l0 = fs.OutLen + fs.OutSuffix;

                if (fs.PrefixDC.getCapacity() > l0) {
                    fs.OutSuffix += fs.PrefixDC.getCapacity() - l0;
                }
            } else if (fs.ResampleFactor == 0) {
                const int FilterLenD2 = fs.FltBank->getFilterLen() / 2;
                const int FilterLenD21 = FilterLenD2 - 1;

                const int ResizeLPix = (int)floor(Vars.o) - FilterLenD21;
                fs.InPrefix = (ResizeLPix < 0 ? -ResizeLPix : 0);
                const int ResizeRPix = (int)floor(Vars.o + (NewLen - 1) * Vars.k) + FilterLenD2 + 1;

                fs.InSuffix = (ResizeRPix > fs.InLen ? ResizeRPix - fs.InLen : 0);

                fs.OutLen = NewLen;
                fs.RPosBuf = &RPosBufArray.getRPosBuf(Vars.k, Vars.o, fs.FltBank->getFracCount());

                fillRPosBuf(fs, Vars);
            } else {
                Vars.k /= fs.ResampleFactor;
                Vars.o /= fs.ResampleFactor;
                Vars.o += fs.EdgePixelCount;

                fs.InPrefix = fs.FltLatency;
                fs.InSuffix = fs.Flt.getCapacity() - fs.FltLatency - 1;

                fs.OutLen = (fs.InLen + fs.ResampleFactor - 1) / fs.ResampleFactor + fs.EdgePixelCount;

                fs.InSuffix += (fs.OutLen - 1) * fs.ResampleFactor + 1 - fs.InLen;

                fs.InPrefix += fs.EdgePixelCount * fs.ResampleFactor;
                fs.OutLen += fs.EdgePixelCount;
            }

            InBuf = fs.OutBuf;
            SrcLen = fs.OutLen;
        }

        Steps[Steps.getItemCount() - 1].OutBuf = 2;
        Vars.IsResize2 = false;

        if (upstep != -1) {
            extendUpsample(Steps[upstep], Steps[upstep + 1]);

            if (Steps[upstep].ResampleFactor == 2 && Vars.ResizeStep == upstep + 1 && fpclass ::packmode == 0 &&
                Steps[upstep].FltOrig.getCapacity() > 0) {
                Vars.IsResize2 = true;
            }
        }
    }


    static void updateBufLenAndRPosPtrs(CFilterSteps& Steps, CImageResizerVars& Vars, const int ResElIncr) {
        int MaxPrefix[2] = {0, 0};
        int MaxLen[2] = {0, 0};
        int i;

        for (i = 0; i < Steps.getItemCount(); i++) {
            CFilterStep& fs = Steps[i];
            const int ib = fs.InBuf;

            if (fs.InPrefix > MaxPrefix[ib]) {
                MaxPrefix[ib] = fs.InPrefix;
            }

            int l = fs.InLen + fs.InSuffix;

            if (l > MaxLen[ib]) {
                MaxLen[ib] = l;
            }

            fs.InElIncr = fs.InPrefix + l;

            if (fs.OutBuf == 2) {
                break;
            }

            const int ob = fs.OutBuf;

            if (fs.IsUpsample) {
                if (fs.OutPrefix > MaxPrefix[ob]) {
                    MaxPrefix[ob] = fs.OutPrefix;
                }

                l = fs.OutLen + fs.OutSuffix;

                if (l > MaxLen[ob]) {
                    MaxLen[ob] = l;
                }
            } else {
                if (fs.OutLen > MaxLen[ob]) {
                    MaxLen[ob] = fs.OutLen;
                }
            }
        }

        for (i = 0; i < Steps.getItemCount(); i++) {
            CFilterStep& fs = Steps[i];

            if (fs.OutBuf == 2) {
                fs.OutElIncr = ResElIncr;
                break;
            }

            CFilterStep& fs2 = Steps[i + 1];

            if (fs.IsUpsample) {
                fs.OutElIncr = fs.OutPrefix + fs.OutLen + fs.OutSuffix;

                if (fs.OutElIncr > fs2.InElIncr) {
                    fs2.InElIncr = fs.OutElIncr;
                } else {
                    fs.OutElIncr = fs2.InElIncr;
                }
            } else {
                fs.OutElIncr = fs2.InElIncr;
            }
        }

        for (i = 0; i < 2; i++) {
            Vars.BufLen[i] = MaxPrefix[i] + MaxLen[i];
            Vars.BufOffs[i] = MaxPrefix[i];

            if (Vars.packmode == 0) {
                Vars.BufOffs[i] *= Vars.ElCount;
            }

            Vars.BufLen[i] *= Vars.ElCount;
        }

        CFilterStep& fs = Steps[Vars.ResizeStep];
        typename CFilterStep ::CResizePos* rpos = &(*fs.RPosBuf)[0];
        const int em = (fpclass ::packmode == 0 ? Vars.ElCount : 1);
        const int fl = (fs.FltBankDyn == nullptr ? fs.FltBank->getFilterLen() : fs.FltBankDyn->getFilterLen());

        const int FilterLenD21 = fl / 2 - 1;

        if (Vars.IsResize2) {
            if (fs.FltBankDyn == nullptr) {
                for (i = 0; i < fs.OutLen; i++) {
                    const int p = rpos->SrcPosInt - FilterLenD21;
                    const int fo = p & 1;
                    rpos->SrcOffs = (p + fo) * em;
                    rpos->ftp = fs.FltBank->getFilterConst(rpos->fti) + fo;

                    rpos->fl = fl - fo;
                    rpos++;
                }
            } else {
                for (i = 0; i < fs.OutLen; i++) {
                    const int p = rpos->SrcPosInt - FilterLenD21;
                    const int fo = p & 1;
                    rpos->SrcOffs = (p + fo) * em;
                    rpos->ftp = fs.FltBankDyn->getFilter(rpos->fti) + fo;

                    rpos->fl = fl - fo;
                    rpos++;
                }
            }
        } else {
            if (fs.FltBankDyn == nullptr) {
                for (i = 0; i < fs.OutLen; i++) {
                    rpos->SrcOffs = (rpos->SrcPosInt - FilterLenD21) * em;

                    rpos->ftp = fs.FltBank->getFilterConst(rpos->fti);
                    rpos++;
                }
            } else {
                for (i = 0; i < fs.OutLen; i++) {
                    rpos->SrcOffs = (rpos->SrcPosInt - FilterLenD21) * em;

                    rpos->ftp = fs.FltBankDyn->getFilter(rpos->fti);
                    rpos++;
                }
            }
        }
    }


    void modifyCorrFilterDCGain(CFilterSteps& Steps, const double m) const {
        CBuffer<fptype>* Flt;
        const int z = Steps.getItemCount() - 1;

        if (!Steps[z].IsUpsample && Steps[z].ResampleFactor == 1) {
            Flt = &Steps[z].Flt;
        } else {
            Flt = &Steps[0].Flt;
        }

        int i;

        for (i = 0; i < Flt->getCapacity(); i++) {
            (*Flt)[i] = (fptype)((double)(*Flt)[i] * m);
        }
    }


    static void fillUsedFracMap(const CFilterStep& fs, CBuffer<char>& UsedFracMap) {
        const int FracCount = fs.FltBank->getFracCount();
        UsedFracMap.increaseCapacity(FracCount, false);
        memset(&UsedFracMap[0], 0, (size_t)FracCount * sizeof(UsedFracMap[0]));

        typename CFilterStep ::CResizePos* rpos = &(*fs.RPosBuf)[0];
        int i;

        for (i = 0; i < fs.OutLen; i++) {
            UsedFracMap[rpos->fti] |= 1;
            rpos++;
        }
    }


    static int calcComplexity(const CFilterSteps& Steps, const CImageResizerVars& Vars,
                              const CBuffer<char>& UsedFracMap, const int ScanlineCount) {
        int fcnum;
        int fcdenom;

        if (Vars.packmode != 0) {
            fcnum = 1;
            fcdenom = 1;
        } else {
            fcnum = 3;
            fcdenom = 4;
        }

        int s = 0;
        int s2 = 0;
        int i;

        for (i = 0; i < Steps.getItemCount(); i++) {
            const CFilterStep& fs = Steps[i];

            s2 += 65 * fs.Flt.getCapacity();

            if (fs.IsUpsample) {
                if (fs.FltOrig.getCapacity() > 0) {
                    continue;
                }

                s += (fs.Flt.getCapacity() * (fs.InPrefix + fs.InLen + fs.InSuffix) + fs.SuffixDC.getCapacity() +
                      fs.PrefixDC.getCapacity()) *
                     Vars.ElCount;
            } else if (fs.ResampleFactor == 0) {
                s += fs.FltBank->getFilterLen() * (fs.FltBank->getOrder() + Vars.ElCount) * fs.OutLen;

                if (i == Vars.ResizeStep && Vars.IsResize2) {
                    s >>= 1;
                }

                s2 += fs.FltBank->calcInitComplexity(UsedFracMap);
            } else {
                s += fs.Flt.getCapacity() * Vars.ElCount * fs.OutLen * fcnum / fcdenom;
            }
        }

        return (s + s2 / ScanlineCount);
    }

    template <typename Tin, typename Tout>
    class CThreadData : public CImageResizerThreadPool ::CWorkload {
       public:
        virtual void process()
#if __cplusplus >= 201103L
            override
#endif  // __cplusplus >= 201103L
        {
            processScanlineQueue();
        }

        enum EScanlineOperation {
            sopResizeH,
            sopResizeV,
            sopDitherAndUnpackH,
            sopUnpackH
        };


        void init(const int aThreadIndex, const int aThreadCount, const CFilterSteps& aSteps,
                  const CImageResizerVars& aVars) {
            ThreadIndex = aThreadIndex;
            ThreadCount = aThreadCount;
            Steps = &aSteps;
            Vars = &aVars;
        }


        void initScanlineQueue(const EScanlineOperation aOp, const int TotalLines, const int aSrcLen,
                               const int aSrcIncr = 0, const int aResIncr = 0) {
            const int l = Vars->BufLen[0] + Vars->BufLen[1];

            if (Bufs.getCapacity() < l) {
                Bufs.alloc(l, fpclass ::fpalign);
            }

            BufPtrs[0] = Bufs + Vars->BufOffs[0];
            BufPtrs[1] = Bufs + Vars->BufLen[0] + Vars->BufOffs[1];

            int j;
            int ml = 0;

            for (j = 0; j < Steps->getItemCount(); j++) {
                const CFilterStep& fs = (*Steps)[j];

                if (fs.ResampleFactor == 0 && ml < fs.FltBank->getFilterLen()) {
                    ml = fs.FltBank->getFilterLen();
                }
            }

            TmpFltBuf.alloc(ml, fpclass ::fpalign);
            ScanlineOp = aOp;
            SrcLen = aSrcLen;
            SrcIncr = aSrcIncr;
            ResIncr = aResIncr;
            QueueLen = 0;
            Queue.increaseCapacity((TotalLines + ThreadCount - 1) / ThreadCount, false);
        }


        void addScanlineToQueue(void* const SrcBuf, void* const ResBuf) {
            Queue[QueueLen].SrcBuf = SrcBuf;
            Queue[QueueLen].ResBuf = ResBuf;
            QueueLen++;
        }

        void processScanlineQueue() {
            int i;

            switch (ScanlineOp) {
                case sopResizeH: {
                    for (i = 0; i < QueueLen; i++) {
                        resizeScanlineH((Tin*)Queue[i].SrcBuf, (fptype*)Queue[i].ResBuf);
                    }

                    break;
                }

                case sopResizeV: {
                    for (i = 0; i < QueueLen; i++) {
                        resizeScanlineV((fptype*)Queue[i].SrcBuf, (fptype*)Queue[i].ResBuf);
                    }

                    break;
                }

                case sopDitherAndUnpackH: {
                    for (i = 0; i < QueueLen; i++) {
                        if (Vars->UseSRGBGamma) {
                            CFilterStep ::applySRGBGamma((fptype*)Queue[i].SrcBuf, SrcLen, *Vars);
                        }

                        Ditherer.dither((fptype*)Queue[i].SrcBuf);

                        CFilterStep ::unpackScanline((fptype*)Queue[i].SrcBuf, (Tout*)Queue[i].ResBuf, SrcLen, *Vars);
                    }

                    break;
                }

                case sopUnpackH: {
                    for (i = 0; i < QueueLen; i++) {
                        if (Vars->UseSRGBGamma) {
                            CFilterStep ::applySRGBGamma((fptype*)Queue[i].SrcBuf, SrcLen, *Vars);
                        }

                        CFilterStep ::unpackScanline((fptype*)Queue[i].SrcBuf, (Tout*)Queue[i].ResBuf, SrcLen, *Vars);
                    }

                    break;
                }
            }
        }

        CDitherer& getDitherer() {
            return (Ditherer);
        }

       private:
        int ThreadIndex;
        int ThreadCount;
        const CFilterSteps* Steps;
        const CImageResizerVars* Vars;
        CBuffer<fptype> Bufs;
        fptype* BufPtrs[3];
        CBuffer<fptype> TmpFltBuf;
        EScanlineOperation ScanlineOp;
        int SrcLen;
        int SrcIncr;
        int ResIncr;
        CDitherer Ditherer;

        struct CQueueItem {
            void* SrcBuf;
            void* ResBuf;
        };

        CBuffer<CQueueItem> Queue;
        int QueueLen;


        void resizeScanlineH(const Tin* const SrcBuf, fptype* const ResBuf) {
            const CFilterStep& fs0 = (*Steps)[0];

            fs0.packScanline(SrcBuf, BufPtrs[0], SrcLen);
            BufPtrs[2] = ResBuf;

            int j;

            for (j = 0; j < Steps->getItemCount(); j++) {
                const CFilterStep& fs = (*Steps)[j];
                fs.prepareInBuf(BufPtrs[fs.InBuf]);
                const int DstIncr = (Vars->packmode == 0 ? Vars->ElCount : 1);
                resizeScanlineStep(fs, BufPtrs[fs.InBuf], BufPtrs[fs.OutBuf], DstIncr);
            }
        }

        void resizeScanlineStep(const CFilterStep& fs, const fptype* const SrcBuf, fptype* const DstBuf,
                                const int DstIncr) {
            if (fs.ResampleFactor != 0) {
                if (fs.IsUpsample) {
                    fs.doUpsample(SrcBuf, DstBuf);
                } else {
                    fs.doFilter(SrcBuf, DstBuf, DstIncr);
                }
            } else {
                if (Vars->IsResize2) {
                    fs.doResize2(SrcBuf, DstBuf, DstIncr, TmpFltBuf);
                } else {
                    fs.doResize(SrcBuf, DstBuf, DstIncr, TmpFltBuf);
                }
            }
        }


        void resizeScanlineV(const fptype* const SrcBuf, fptype* const ResBuf) {
            const CFilterStep& fs0 = (*Steps)[0];

            fs0.convertVtoH(SrcBuf, BufPtrs[0], SrcLen, SrcIncr);
            BufPtrs[2] = ResBuf;

            int j;

            for (j = 0; j < Steps->getItemCount(); j++) {
                const CFilterStep& fs = (*Steps)[j];
                fs.prepareInBuf(BufPtrs[fs.InBuf]);
                const int DstIncr = (fs.OutBuf == 2 ? ResIncr : (Vars->packmode == 0 ? Vars->ElCount : 1));
                resizeScanlineStep(fs, BufPtrs[fs.InBuf], BufPtrs[fs.OutBuf], DstIncr);
            }
        }
    };
};

#undef AVIR_PI
#undef AVIR_PId2
#undef AVIR_NOCTOR

#if defined(AVIR_NULLPTR)
#undef nullptr
#undef AVIR_NULLPTR
#endif  // defined( AVIR_NULLPTR )

}  

#endif  // AVIR_CIMAGERESIZER_INCLUDED
