/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if defined(FREEBL_NO_DEPEND)
#include "stubs.h"
#endif

#include "blapii.h"
#include "mpi.h"
#include "secerr.h"
#include "prtypes.h"
#include "prinit.h"
#include "prenv.h"

#if defined(_MSC_VER) && !defined(_M_IX86)
#include <intrin.h> /* for _xgetbv() */
#endif



static PRCallOnceType coFreeblInit;

static PRBool aesni_support_ = PR_FALSE;
static PRBool clmul_support_ = PR_FALSE;
static PRBool sha_support_ = PR_FALSE;
static PRBool avx_support_ = PR_FALSE;
static PRBool avx2_support_ = PR_FALSE;
static PRBool adx_support_ = PR_FALSE;
static PRBool ssse3_support_ = PR_FALSE;
static PRBool sse4_1_support_ = PR_FALSE;
static PRBool sse4_2_support_ = PR_FALSE;
static PRBool arm_neon_support_ = PR_FALSE;
static PRBool arm_aes_support_ = PR_FALSE;
static PRBool arm_sha1_support_ = PR_FALSE;
static PRBool arm_sha2_support_ = PR_FALSE;
static PRBool arm_pmull_support_ = PR_FALSE;
static PRBool ppc_crypto_support_ = PR_FALSE;

#if defined(NSS_X86_OR_X64)
static PRBool
check_xcr0_ymm()
{
    PRUint32 xcr0;
#if defined(_MSC_VER)
#if defined(_M_IX86)
    __asm {
        mov ecx, 0
        xgetbv
        mov xcr0, eax
    }
#else
    xcr0 = (PRUint32)_xgetbv(0); 
#endif
#else
    __asm__(".byte 0x0F, 0x01, 0xd0"
            : "=a"(xcr0)
            : "c"(0)
            : "%edx");
#endif
    return (xcr0 & 6) == 6;
}

#define ECX_AESNI (1 << 25)
#define ECX_CLMUL (1 << 1)
#define ECX_XSAVE (1 << 26)
#define ECX_OSXSAVE (1 << 27)
#define ECX_AVX (1 << 28)
#define EBX_AVX2 (1 << 5)
#define EBX_ADX (1 << 19)
#define EBX_BMI1 (1 << 3)
#define EBX_BMI2 (1 << 8)
#define EBX_SHA (1 << 29)
#define ECX_FMA (1 << 12)
#define ECX_MOVBE (1 << 22)
#define ECX_SSSE3 (1 << 9)
#define ECX_SSE4_1 (1 << 19)
#define ECX_SSE4_2 (1 << 20)
#define AVX_BITS (ECX_XSAVE | ECX_OSXSAVE | ECX_AVX)
#define AVX2_EBX_BITS (EBX_AVX2 | EBX_BMI1 | EBX_BMI2)
#define AVX2_ECX_BITS (ECX_FMA | ECX_MOVBE)

void
CheckX86CPUSupport()
{
    unsigned long eax, ebx, ecx, edx;
    unsigned long eax7, ebx7, ecx7, edx7;
    char *disable_hw_aes = PR_GetEnvSecure("NSS_DISABLE_HW_AES");
    char *disable_pclmul = PR_GetEnvSecure("NSS_DISABLE_PCLMUL");
    char *disable_hw_sha = PR_GetEnvSecure("NSS_DISABLE_HW_SHA");
    char *disable_avx = PR_GetEnvSecure("NSS_DISABLE_AVX");
    char *disable_avx2 = PR_GetEnvSecure("NSS_DISABLE_AVX2");
    char *disable_adx = PR_GetEnvSecure("NSS_DISABLE_ADX");
    char *disable_ssse3 = PR_GetEnvSecure("NSS_DISABLE_SSSE3");
    char *disable_sse4_1 = PR_GetEnvSecure("NSS_DISABLE_SSE4_1");
    char *disable_sse4_2 = PR_GetEnvSecure("NSS_DISABLE_SSE4_2");
    freebl_cpuid(1, &eax, &ebx, &ecx, &edx);
    freebl_cpuid(7, &eax7, &ebx7, &ecx7, &edx7);
    aesni_support_ = (PRBool)((ecx & ECX_AESNI) != 0 && disable_hw_aes == NULL);
    clmul_support_ = (PRBool)((ecx & ECX_CLMUL) != 0 && disable_pclmul == NULL);
    sha_support_ = (PRBool)((ebx7 & EBX_SHA) != 0 && disable_hw_sha == NULL);
    avx_support_ = (PRBool)((ecx & AVX_BITS) == AVX_BITS) && check_xcr0_ymm() &&
                   disable_avx == NULL;
    avx2_support_ = (PRBool)(avx_support_ == PR_TRUE &&
                             (ebx7 & AVX2_EBX_BITS) == AVX2_EBX_BITS &&
                             (ecx & AVX2_ECX_BITS) == AVX2_ECX_BITS &&
                             disable_avx2 == NULL);
    adx_support_ = (PRBool)((ebx7 & EBX_ADX) != 0 && disable_adx == NULL);
    ssse3_support_ = (PRBool)((ecx & ECX_SSSE3) != 0 &&
                              disable_ssse3 == NULL);
    sse4_1_support_ = (PRBool)((ecx & ECX_SSE4_1) != 0 &&
                               disable_sse4_1 == NULL);
    sse4_2_support_ = (PRBool)((ecx & ECX_SSE4_2) != 0 &&
                               disable_sse4_2 == NULL);
}
#endif

/* clang-format off */
#if (defined(__aarch64__) || defined(__arm__)) && !0
#if !defined(__has_include)
#define __has_include(x) 0
#endif
#if (__has_include(<sys/auxv.h>) || defined(__linux__)) && \
    defined(__GNUC__) && __GNUC__ >= 2 && defined(__ELF__)
#include <sys/auxv.h>
extern unsigned long getauxval(unsigned long type) __attribute__((weak));
#else
static unsigned long (*getauxval)(unsigned long) = NULL;
#endif


#if !defined(AT_HWCAP2)
#define AT_HWCAP2 26
#endif
#if !defined(AT_HWCAP)
#define AT_HWCAP 16
#endif

#endif
/* clang-format on */

#if defined(__aarch64__)

#if defined(__linux__)
#if !defined(HWCAP_AES)
#define HWCAP_AES (1 << 3)
#endif
#if !defined(HWCAP_PMULL)
#define HWCAP_PMULL (1 << 4)
#endif
#if !defined(HWCAP_SHA1)
#define HWCAP_SHA1 (1 << 5)
#endif
#if !defined(HWCAP_SHA2)
#define HWCAP_SHA2 (1 << 6)
#endif
#endif



void
CheckARMSupport()
{
#if (defined(__linux__) || 0 || 0) && \
    __has_include(<sys/auxv.h>)
    if (getauxval) {
        long hwcaps = getauxval(AT_HWCAP);
        arm_aes_support_ = (hwcaps & HWCAP_AES) == HWCAP_AES;
        arm_pmull_support_ = (hwcaps & HWCAP_PMULL) == HWCAP_PMULL;
        arm_sha1_support_ = (hwcaps & HWCAP_SHA1) == HWCAP_SHA1;
        arm_sha2_support_ = (hwcaps & HWCAP_SHA2) == HWCAP_SHA2;
    }
#elif defined(__ARM_FEATURE_CRYPTO)
    arm_aes_support_ = PR_TRUE;
    arm_pmull_support_ = PR_TRUE;
    arm_sha1_support_ = PR_TRUE;
    arm_sha2_support_ = PR_TRUE;
#endif
    arm_neon_support_ = PR_GetEnvSecure("NSS_DISABLE_ARM_NEON") == NULL;
    arm_aes_support_ &= PR_GetEnvSecure("NSS_DISABLE_HW_AES") == NULL;
    arm_pmull_support_ &= PR_GetEnvSecure("NSS_DISABLE_PMULL") == NULL;
    arm_sha1_support_ &= PR_GetEnvSecure("NSS_DISABLE_HW_SHA1") == NULL;
    arm_sha2_support_ &= PR_GetEnvSecure("NSS_DISABLE_HW_SHA2") == NULL;
}
#endif

#if defined(__arm__)
#if !defined(HWCAP_NEON)
#define HWCAP_NEON (1 << 12)
#endif

#if !defined(HWCAP2_AES)
#define HWCAP2_AES (1 << 0)
#endif
#if !defined(HWCAP2_PMULL)
#define HWCAP2_PMULL (1 << 1)
#endif
#if !defined(HWCAP2_SHA1)
#define HWCAP2_SHA1 (1 << 2)
#endif
#if !defined(HWCAP2_SHA2)
#define HWCAP2_SHA2 (1 << 3)
#endif

PRBool
GetNeonSupport()
{
    char *disable_arm_neon = PR_GetEnvSecure("NSS_DISABLE_ARM_NEON");
    if (disable_arm_neon) {
        return PR_FALSE;
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return PR_TRUE;
#else
    if (getauxval) {
        return (getauxval(AT_HWCAP) & HWCAP_NEON);
    }
#endif
    return PR_FALSE;
}

#if defined(__linux__)
static long
ReadCPUInfoForHWCAP2()
{
    FILE *cpuinfo;
    char buf[512];
    char *p;
    long hwcap2 = 0;

    cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        return 0;
    }
    while (fgets(buf, 511, cpuinfo)) {
        if (!memcmp(buf, "Features", 8)) {
            p = strstr(buf, " aes");
            if (p && (p[4] == ' ' || p[4] == '\n')) {
                hwcap2 |= HWCAP2_AES;
            }
            p = strstr(buf, " sha1");
            if (p && (p[5] == ' ' || p[5] == '\n')) {
                hwcap2 |= HWCAP2_SHA1;
            }
            p = strstr(buf, " sha2");
            if (p && (p[5] == ' ' || p[5] == '\n')) {
                hwcap2 |= HWCAP2_SHA2;
            }
            p = strstr(buf, " pmull");
            if (p && (p[6] == ' ' || p[6] == '\n')) {
                hwcap2 |= HWCAP2_PMULL;
            }
            break;
        }
    }

    fclose(cpuinfo);
    return hwcap2;
}
#endif

void
CheckARMSupport()
{
    char *disable_hw_aes = PR_GetEnvSecure("NSS_DISABLE_HW_AES");
    if (getauxval) {
        long hwcaps = getauxval(AT_HWCAP2);
#if defined(__linux__)
        if (!hwcaps) {
            hwcaps = ReadCPUInfoForHWCAP2();
        }
#endif
        arm_aes_support_ = hwcaps & HWCAP2_AES && disable_hw_aes == NULL;
        arm_pmull_support_ = hwcaps & HWCAP2_PMULL;
        arm_sha1_support_ = hwcaps & HWCAP2_SHA1;
        arm_sha2_support_ = hwcaps & HWCAP2_SHA2;
    }
    arm_neon_support_ = GetNeonSupport();
    arm_sha1_support_ &= PR_GetEnvSecure("NSS_DISABLE_HW_SHA1") == NULL;
    arm_sha2_support_ &= PR_GetEnvSecure("NSS_DISABLE_HW_SHA2") == NULL;
}
#endif


PRBool
aesni_support()
{
    return aesni_support_;
}
PRBool
clmul_support()
{
    return clmul_support_;
}
PRBool
sha_support()
{
    return sha_support_;
}
PRBool
avx_support()
{
    return avx_support_;
}
PRBool
avx2_support()
{
    return avx2_support_;
}
PRBool
adx_support()
{
    return adx_support_;
}
PRBool
ssse3_support()
{
    return ssse3_support_;
}
PRBool
sse4_1_support()
{
    return sse4_1_support_;
}
PRBool
sse4_2_support()
{
    return sse4_2_support_;
}
PRBool
arm_neon_support()
{
    return arm_neon_support_;
}
PRBool
arm_aes_support()
{
    return arm_aes_support_;
}
PRBool
arm_pmull_support()
{
    return arm_pmull_support_;
}
PRBool
arm_sha1_support()
{
    return arm_sha1_support_;
}
PRBool
arm_sha2_support()
{
    return arm_sha2_support_;
}
PRBool
ppc_crypto_support()
{
    return ppc_crypto_support_;
}

#if defined(__powerpc__)

#if !defined(__has_include)
#define __has_include(x) 0
#endif

/* clang-format off */
#if (defined(__linux__) || 0 || 0) && \
    __has_include(<sys/auxv.h>)
#include <sys/auxv.h>
#endif

#if !defined(PPC_FEATURE2_VEC_CRYPTO)
#define PPC_FEATURE2_VEC_CRYPTO 0x02000000
#endif

static void
CheckPPCSupport()
{
    char *disable_hw_crypto = PR_GetEnvSecure("NSS_DISABLE_PPC_GHASH");

    unsigned long hwcaps = 0;
#if defined(__linux__) && __has_include(<sys/auxv.h>)
    hwcaps = getauxval(AT_HWCAP2);
#endif

    ppc_crypto_support_ = hwcaps & PPC_FEATURE2_VEC_CRYPTO && disable_hw_crypto == NULL;
}
/* clang-format on */

#endif

static PRStatus
FreeblInit(void)
{
#if defined(NSS_X86_OR_X64)
    CheckX86CPUSupport();
#elif (defined(__aarch64__) || defined(__arm__))
    CheckARMSupport();
#elif (defined(__powerpc__))
    CheckPPCSupport();
#endif
    return PR_SUCCESS;
}

SECStatus
BL_Init(void)
{
    if (PR_CallOnce(&coFreeblInit, FreeblInit) != PR_SUCCESS) {
        PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
        return SECFailure;
    }
    RSA_Init();

    return SECSuccess;
}
