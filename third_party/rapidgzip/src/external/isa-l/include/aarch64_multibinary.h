/**********************************************************************
  Copyright(c) 2020 Arm Corporation All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Arm Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************/
#ifndef __AARCH64_MULTIBINARY_H__
#define __AARCH64_MULTIBINARY_H__
#ifndef __aarch64__
#error "This file is for aarch64 only"
#endif
#include "aarch64_label.h"
#ifdef __ASSEMBLY__
.macro mbin_interface name : req.extern cdecl(\name\() _dispatcher)
                                 .data.balign
                             8 .global cdecl(\name\() _dispatcher_info)
#ifndef __APPLE__
                                 .type   \name\() _dispatcher_info,
    % object
#endif
        cdecl(\name\() _dispatcher_info)
    :.quad   \name\() _mbinit
#ifndef __APPLE__
          .size   \name\() _dispatcher_info,
.- \name\() _dispatcher_info
#endif
        .balign 8 .text
	\name\() _mbinit :.cfi_startproc stp x29,
    x30,
    [ sp, -224 ] !

        .cfi_def_cfa_offset 224 .cfi_offset 29,
    -224 .cfi_offset 30,
    -216

    stp x8,
    x9, [ sp, 16 ].cfi_offset 8, -208 .cfi_offset 9, -200 stp x0, x1, [ sp, 32 ].cfi_offset 0, -192 .cfi_offset 1,
    -184 stp x2, x3, [ sp, 48 ].cfi_offset 2, -176 .cfi_offset 3, -168 stp x4, x5, [ sp, 64 ].cfi_offset 4,
    -160 .cfi_offset 5, -152 stp x6, x7, [ sp, 80 ].cfi_offset 6, -144 .cfi_offset 7, -136 stp q0, q1,
    [ sp, 96 ].cfi_offset 64, -128 .cfi_offset 65, -112 stp q2, q3, [ sp, 128 ].cfi_offset 66, -96 .cfi_offset 67,
    -80 stp q4, q5, [ sp, 160 ].cfi_offset 68, -64 .cfi_offset 69, -48 stp q6, q7, [ sp, 192 ].cfi_offset 70,
    -32 .cfi_offset 71,
    -16

    bl cdecl(\name\() _dispatcher) ldp x8,
    x9,
    [ sp, 16 ].cfi_restore 8 .cfi_restore 9

    str x0,
    [x9]

    ldp x0,
    x1, [ sp, 32 ].cfi_restore 0 .cfi_restore 1 ldp x2, x3, [ sp, 48 ].cfi_restore 2 .cfi_restore 3 ldp x4, x5,
    [ sp, 64 ].cfi_restore 4 .cfi_restore 5 ldp x6, x7, [ sp, 80 ].cfi_restore 6 .cfi_restore 7 ldp q0, q1,
    [ sp, 96 ].cfi_restore 64 .cfi_restore 65 ldp q2, q3, [ sp, 128 ].cfi_restore 66 .cfi_restore 67 ldp q4, q5,
    [ sp, 160 ].cfi_restore 68 .cfi_restore 69 ldp q6, q7,
    [ sp, 192 ].cfi_restore 70 .cfi_restore 71 ldp x29, x30, [sp],
    224 .cfi_restore 30 .cfi_restore 29 .cfi_def_cfa_offset
    0 .cfi_endproc

        .global cdecl(\name)
#ifndef __APPLE__
        .type \name,
    % function
#endif
            .align 2 cdecl(\name\())
    :
#ifndef __APPLE__
      adrp x9,
    : got :\name\() _dispatcher_info ldr x9,
    [ x9, #:got_lo12:\name\() _dispatcher_info ]
#else
      adrp x9,
cdecl(\name\() _dispatcher_info) @GOTPAGE ldr x9, [ x9, #cdecl(\name\() _dispatcher_info) @GOTPAGEOFF ]
#endif
    ldr x10,
    [x9] br x10
#ifndef __APPLE__
        .size \name,
    .- \name
#endif
            .endm

            .macro mbin_interface_base name : req,
    base : req.extern \base.data.balign
           8 .global cdecl(\name\() _dispatcher_info)
#ifndef __APPLE__
               .type   \name\() _dispatcher_info,
    % object
#endif
        cdecl(\name\() _dispatcher_info)
    :.quad   \base
#ifndef __APPLE__
          .size   \name\() _dispatcher_info,
    .- \name\() _dispatcher_info
#endif
            .balign 8 .text
            .global cdecl(\name)
#ifndef __APPLE__
            .type \name,
    % function
#endif
            .align 2 cdecl(\name\())
    :
#ifndef __APPLE__
      adrp x9,
    : got : cdecl(_\name\() _dispatcher_info) ldr x9,
            [ x9, #:got_lo12:cdecl(_\name\() _dispatcher_info) ]
#else
      adrp x9,
cdecl(_\name\() _dispatcher_info) @GOTPAGE ldr x9, [ x9, #cdecl(_\name\() _dispatcher_info) @GOTPAGEOFF ]
#endif
            ldr x10,
            [x9] br x10
#ifndef __APPLE__
                .size \name,
            .- \name
#endif
                    .endm

#else /* __ASSEMBLY__ */
#include <stdint.h>
#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(__APPLE__)
#define SYSCTL_PMULL_KEY "hw.optional.arm.FEAT_PMULL"  // from macOS 12 FEAT_* sysctl infos are available
#define SYSCTL_CRC32_KEY "hw.optional.armv8_crc32"
#define SYSCTL_SVE_KEY "hw.optional.arm.FEAT_SVE"  // this one is just a guess and need to check macOS update
#include <stddef.h>
#include <sys/sysctl.h>
static inline int sysctlEnabled(const char* name) {
    int enabled;
    size_t size = sizeof(enabled);
    int status = sysctlbyname(name, &enabled, &size, NULL, 0);
    return status ? 0 : enabled;
}
#endif

#define DEFINE_INTERFACE_DISPATCHER(name) void* name##_dispatcher(void)

#define CPU_IMPLEMENTER_RESERVE 0x00
#define CPU_IMPLEMENTER_ARM 0x41

#define CPU_PART_CORTEX_A57 0xD07
#define CPU_PART_CORTEX_A72 0xD08
#define CPU_PART_NEOVERSE_N1 0xD0C

#define MICRO_ARCH_ID(imp, part) (((CPU_IMPLEMENTER_##imp & 0xff) << 24) | ((CPU_PART_##part & 0xfff) << 4))

#ifndef HWCAP_CPUID
#define HWCAP_CPUID (1 << 11)
#endif

static inline uint32_t get_micro_arch_id(void) {
    uint64_t id = CPU_IMPLEMENTER_RESERVE;
#ifndef __APPLE__
    if ((getauxval(AT_HWCAP) & HWCAP_CPUID)) {
        asm("mrs %0, MIDR_EL1 " : "=r"(id));
    }
#endif
    return id & 0xff00fff0;
}

#endif /* __ASSEMBLY__ */
#endif
