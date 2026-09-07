/***************************************************************************
 * Copyright (c) Andreas Krebbel                                            *
 * Based on xsimd_cpu_features_ppc.hpp                                      *
 * Copyright (c) Johan Mabille, Sylvain Corlay, Wolf Vollprecht and         *
 * Martin Renou                                                             *
 * Copyright (c) QuantStack                                                 *
 * Copyright (c) Serge Guelton                                              *
 *                                                                          *
 * Distributed under the terms of the BSD 3-Clause License.                 *
 *                                                                          *
 * The full license is in the file LICENSE, distributed with this software. *
 ****************************************************************************/

#if !defined(XSIMD_CPU_FEATURES_S390X_HPP)
#define XSIMD_CPU_FEATURES_S390X_HPP

#include "./xsimd_config.hpp"
#include "./xsimd_getauxval.hpp"

namespace xsimd
{
    class s390x_cpu_features : private linux_hwcap_backend_default
    {
    public:
        inline bool vxe() const noexcept;
    };


    inline bool s390x_cpu_features::vxe() const noexcept
    {
#if XSIMD_TARGET_S390X && XSIMD_HAVE_LINUX_GETAUXVAL
#if defined(HWCAP_S390_VXE)
        return hwcap().has_feature(HWCAP_S390_VXE);
#else
        return hwcap().has_feature(8192);
#endif
#else
        return XSIMD_WITH_VXE;
#endif
    }
}

#endif
