/***************************************************************************
 * Copyright (c) Johan Mabille, Sylvain Corlay, Wolf Vollprecht and         *
 * Martin Renou                                                             *
 * Copyright (c) QuantStack                                                 *
 * Copyright (c) Serge Guelton                                              *
 *                                                                          *
 * Distributed under the terms of the BSD 3-Clause License.                 *
 *                                                                          *
 * The full license is in the file LICENSE, distributed with this software. *
 ***************************************************************************/

#if !defined(XSIMD_CPU_FEATURES_PPC_HPP)
#define XSIMD_CPU_FEATURES_PPC_HPP

#include "./xsimd_config.hpp"
#include "./xsimd_getauxval.hpp"

namespace xsimd
{
    class ppc_cpu_features : private linux_hwcap_backend_default
    {
    public:
        inline bool vsx() const noexcept;
    };


    inline bool ppc_cpu_features::vsx() const noexcept
    {
#if XSIMD_TARGET_PPC && XSIMD_HAVE_LINUX_GETAUXVAL
#if defined(PPC_FEATURE_HAS_VSX)
        return hwcap().has_feature(PPC_FEATURE_HAS_VSX);
#else
        return hwcap().has_feature(0x00000080);
#endif
#else
        return XSIMD_WITH_VSX;
#endif
    }
}

#endif
