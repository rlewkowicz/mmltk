/*
 * Copyright © 2000 SuSE, Inc.
 * Copyright © 2007 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of SuSE not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  SuSE makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * SuSE DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL SuSE
 * BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#if defined(HAVE_CONFIG_H)
#include <pixman-config.h>
#endif

#include "pixman-private.h"

typedef enum
{
    ARM_V7		= (1 << 0),
    ARM_V6		= (1 << 1),
    ARM_VFP		= (1 << 2),
    ARM_NEON		= (1 << 3)
} arm_cpu_features_t;

#if defined(USE_ARM_SIMD) || defined(USE_ARM_NEON)

#if defined(_MSC_VER)

#include <windows.h>

extern int pixman_msvc_try_arm_neon_op ();
extern int pixman_msvc_try_arm_simd_op ();

static arm_cpu_features_t
detect_cpu_features (void)
{
    arm_cpu_features_t features = 0;

    __try
    {
	pixman_msvc_try_arm_simd_op ();
	features |= ARM_V6;
    }
    __except (GetExceptionCode () == EXCEPTION_ILLEGAL_INSTRUCTION)
    {
    }

    __try
    {
	pixman_msvc_try_arm_neon_op ();
	features |= ARM_NEON;
    }
    __except (GetExceptionCode () == EXCEPTION_ILLEGAL_INSTRUCTION)
    {
    }

    return features;
}

#elif defined (__linux__) /* linux ELF */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <elf.h>

static arm_cpu_features_t
detect_cpu_features (void)
{
    arm_cpu_features_t features = 0;
    Elf32_auxv_t aux;
    int fd;

    fd = open ("/proc/self/auxv", O_RDONLY);
    if (fd >= 0)
    {
	while (read (fd, &aux, sizeof(Elf32_auxv_t)) == sizeof(Elf32_auxv_t))
	{
	    if (aux.a_type == AT_HWCAP)
	    {
		uint32_t hwcap = aux.a_un.a_val;

		if ((hwcap & 64) != 0)
		    features |= ARM_VFP;
		if ((hwcap & 4096) != 0)
		    features |= ARM_NEON;
	    }
	    else if (aux.a_type == AT_PLATFORM)
	    {
		const char *plat = (const char*) aux.a_un.a_val;

		if (strncmp (plat, "v7l", 3) == 0)
		    features |= (ARM_V7 | ARM_V6);
		else if (strncmp (plat, "v6l", 3) == 0)
		    features |= ARM_V6;
	    }
	}
	close (fd);
    }

    return features;
}

#elif defined (_3DS) /* 3DS homebrew (devkitARM) */

static arm_cpu_features_t
detect_cpu_features (void)
{
    arm_cpu_features_t features = 0;

    features |= ARM_V6;

    return features;
}

#elif defined (PSP2) || defined (__SWITCH__)

static arm_cpu_features_t
detect_cpu_features (void)
{
    arm_cpu_features_t features = 0;

    features |= ARM_NEON;

    return features;
}

#else

static arm_cpu_features_t
detect_cpu_features (void)
{
    return 0;
}

#endif

static pixman_bool_t
have_feature (arm_cpu_features_t feature)
{
    static pixman_bool_t initialized;
    static arm_cpu_features_t features;

    if (!initialized)
    {
	features = detect_cpu_features();
	initialized = TRUE;
    }

    return (features & feature) == feature;
}

#endif

pixman_implementation_t *
_pixman_arm_get_implementations (pixman_implementation_t *imp)
{
#if defined(USE_ARM_SIMD)
    if (!_pixman_disabled ("arm-simd") && have_feature (ARM_V6))
	imp = _pixman_implementation_create_arm_simd (imp);
#endif

#if defined(USE_ARM_NEON)
    if (!_pixman_disabled ("arm-neon") && have_feature (ARM_NEON))
	imp = _pixman_implementation_create_arm_neon (imp);
#endif

#if defined(USE_ARM_A64_NEON)
    if (!_pixman_disabled ("arm-neon"))
        imp = _pixman_implementation_create_arm_neon (imp);
#endif

    return imp;
}
