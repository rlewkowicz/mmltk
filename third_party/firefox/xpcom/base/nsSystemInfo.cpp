/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAppRunner.h"
#include "nsSystemInfo.h"
#include "prsystem.h"
#include "prio.h"
#include "mozilla/SSE.h"
#include "mozilla/arm.h"
#include "mozilla/dom/DOMMozPromiseRequestHolder.h"
#include "mozilla/Hal.h"
#include "mozilla/LazyIdleThread.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Try.h"
#include "mozilla/Vector.h"
#include "jsapi.h"
#include "js/PropertyAndElement.h"  // JS_SetProperty
#include "mozilla/dom/Promise.h"



#ifdef MOZ_WIDGET_GTK
#  include <gtk/gtk.h>
#  include <dlfcn.h>
#  include "mozilla/WidgetUtilsGtk.h"
#endif

#  include <unistd.h>
#  include <fstream>
#    include <link.h>
#  include "mozilla/Tokenizer.h"
#  include "mozilla/widget/LSBUtils.h"
#  include "nsCharSeparatedTokenizer.h"

#  include <map>
#  include <string>




uint32_t nsSystemInfo::gUserUmask = 0;

using namespace mozilla::dom;


static void SimpleParseKeyValuePairs(
    const std::string& aFilename,
    std::map<nsCString, nsCString>& aKeyValuePairs) {
  std::ifstream input(aFilename.c_str());
  if (!input.is_open()) {
    return;
  }
  for (std::string line; std::getline(input, line);) {
    nsAutoCString key, value;

    nsCCharSeparatedTokenizer tokens(nsDependentCString(line.c_str()), ':');
    if (tokens.hasMoreTokens()) {
      key = tokens.nextToken();
      if (tokens.hasMoreTokens()) {
        value = tokens.nextToken();
      }
      aKeyValuePairs[key] = std::move(value);
    }
  }
}





static nsresult GetLinuxProductName(nsAutoCString& aProductName) {
  std::ifstream input("/sys/devices/virtual/dmi/id/product_name");
  if (!input.is_open()) {
    return NS_ERROR_FAILURE;
  }
  std::string line;
  if (!std::getline(input, line)) {
    return NS_ERROR_FAILURE;
  }
  aProductName = line.c_str();
  return NS_OK;
}

static nsresult GetLinuxProductSku(nsAutoCString& aProductSku) {
  std::ifstream input("/sys/devices/virtual/dmi/id/product_sku");
  if (!input.is_open()) {
    return NS_ERROR_FAILURE;
  }
  std::string line;
  if (!std::getline(input, line)) {
    return NS_ERROR_FAILURE;
  }
  aProductSku = line.c_str();
  return NS_OK;
}

using namespace mozilla;

nsSystemInfo::nsSystemInfo() = default;

nsSystemInfo::~nsSystemInfo() = default;

static const struct PropItems {
  const char* name;
  bool (*propfun)(void);
} cpuPropItems[] = {
    {"hasMMX", mozilla::supports_mmx},
    {"hasSSE", mozilla::supports_sse},
    {"hasSSE2", mozilla::supports_sse2},
    {"hasSSE3", mozilla::supports_sse3},
    {"hasSSSE3", mozilla::supports_ssse3},
    {"hasSSE4A", mozilla::supports_sse4a},
    {"hasSSE4_1", mozilla::supports_sse4_1},
    {"hasSSE4_2", mozilla::supports_sse4_2},
    {"hasAVX", mozilla::supports_avx},
    {"hasAVX2", mozilla::supports_avx2},
    {"hasAES", mozilla::supports_aes},
    {"hasEDSP", mozilla::supports_edsp},
    {"hasARMv6", mozilla::supports_armv6},
    {"hasARMv7", mozilla::supports_armv7},
    {"hasNEON", mozilla::supports_neon}};

nsresult CollectProcessInfo(ProcessInfo& info) {
  nsAutoCString cpuVendor;
  nsAutoCString cpuName;
  int cpuSpeed = -1;
  int cpuFamily = -1;
  int cpuModel = -1;
  int cpuStepping = -1;
  int logicalCPUs = -1;
  int physicalCPUs = -1;
  int cacheSizeL2 = -1;
  int cacheSizeL3 = -1;

  {
    std::map<nsCString, nsCString> keyValuePairs;
    SimpleParseKeyValuePairs("/proc/cpuinfo", keyValuePairs);

#  if defined(__arm__) || defined(__aarch64__)

    /* clang-format off */
    struct id_part {
        const int id;
        const char* name;
    };

    static const struct id_part arm_part[] = {
        { 0x810, "ARM810" },
        { 0x920, "ARM920" },
        { 0x922, "ARM922" },
        { 0x926, "ARM926" },
        { 0x940, "ARM940" },
        { 0x946, "ARM946" },
        { 0x966, "ARM966" },
        { 0xa20, "ARM1020" },
        { 0xa22, "ARM1022" },
        { 0xa26, "ARM1026" },
        { 0xb02, "ARM11 MPCore" },
        { 0xb36, "ARM1136" },
        { 0xb56, "ARM1156" },
        { 0xb76, "ARM1176" },
        { 0xc05, "Cortex-A5" },
        { 0xc07, "Cortex-A7" },
        { 0xc08, "Cortex-A8" },
        { 0xc09, "Cortex-A9" },
        { 0xc0d, "Cortex-A17" },	
        { 0xc0f, "Cortex-A15" },
        { 0xc0e, "Cortex-A17" },
        { 0xc14, "Cortex-R4" },
        { 0xc15, "Cortex-R5" },
        { 0xc17, "Cortex-R7" },
        { 0xc18, "Cortex-R8" },
        { 0xc20, "Cortex-M0" },
        { 0xc21, "Cortex-M1" },
        { 0xc23, "Cortex-M3" },
        { 0xc24, "Cortex-M4" },
        { 0xc27, "Cortex-M7" },
        { 0xc60, "Cortex-M0+" },
        { 0xd01, "Cortex-A32" },
        { 0xd02, "Cortex-A34" },
        { 0xd03, "Cortex-A53" },
        { 0xd04, "Cortex-A35" },
        { 0xd05, "Cortex-A55" },
        { 0xd06, "Cortex-A65" },
        { 0xd07, "Cortex-A57" },
        { 0xd08, "Cortex-A72" },
        { 0xd09, "Cortex-A73" },
        { 0xd0a, "Cortex-A75" },
        { 0xd0b, "Cortex-A76" },
        { 0xd0c, "Neoverse-N1" },
        { 0xd0d, "Cortex-A77" },
        { 0xd0e, "Cortex-A76AE" },
        { 0xd13, "Cortex-R52" },
        { 0xd15, "Cortex-R82" },
        { 0xd16, "Cortex-R52+" },
        { 0xd20, "Cortex-M23" },
        { 0xd21, "Cortex-M33" },
        { 0xd22, "Cortex-M55" },
        { 0xd23, "Cortex-M85" },
        { 0xd40, "Neoverse-V1" },
        { 0xd41, "Cortex-A78" },
        { 0xd42, "Cortex-A78AE" },
        { 0xd43, "Cortex-A65AE" },
        { 0xd44, "Cortex-X1" },
        { 0xd46, "Cortex-A510" },
        { 0xd47, "Cortex-A710" },
        { 0xd48, "Cortex-X2" },
        { 0xd49, "Neoverse-N2" },
        { 0xd4a, "Neoverse-E1" },
        { 0xd4b, "Cortex-A78C" },
        { 0xd4c, "Cortex-X1C" },
        { 0xd4d, "Cortex-A715" },
        { 0xd4e, "Cortex-X3" },
        { 0xd4f, "Neoverse-V2" },
        { 0xd80, "Cortex-A520" },
        { 0xd81, "Cortex-A720" },
        { 0xd82, "Cortex-X4" },
        { 0xd84, "Neoverse-V3" },
        { 0xd8e, "Neoverse-N3" },
        { -1, "unknown" },
    };

    static const struct id_part brcm_part[] = {
        { 0x0f, "Brahma-B15" },
        { 0x100, "Brahma-B53" },
        { 0x516, "ThunderX2" },
        { -1, "unknown" },
    };

    static const struct id_part dec_part[] = {
        { 0xa10, "SA110" },
        { 0xa11, "SA1100" },
        { -1, "unknown" },
    };

    static const struct id_part cavium_part[] = {
        { 0x0a0, "ThunderX" },
        { 0x0a1, "ThunderX-88XX" },
        { 0x0a2, "ThunderX-81XX" },
        { 0x0a3, "ThunderX-83XX" },
        { 0x0af, "ThunderX2-99xx" },
        { 0x0b0, "OcteonTX2" },
        { 0x0b1, "OcteonTX2-98XX" },
        { 0x0b2, "OcteonTX2-96XX" },
        { 0x0b3, "OcteonTX2-95XX" },
        { 0x0b4, "OcteonTX2-95XXN" },
        { 0x0b5, "OcteonTX2-95XXMM" },
        { 0x0b6, "OcteonTX2-95XXO" },
        { 0x0b8, "ThunderX3-T110" },
        { -1, "unknown" },
    };

    static const struct id_part apm_part[] = {
        { 0x000, "X-Gene" },
        { -1, "unknown" },
    };

    static const struct id_part qcom_part[] = {
        { 0x00f, "Scorpion" },
        { 0x02d, "Scorpion" },
        { 0x04d, "Krait" },
        { 0x06f, "Krait" },
        { 0x201, "Kryo" },
        { 0x205, "Kryo" },
        { 0x211, "Kryo" },
        { 0x800, "Falkor-V1/Kryo" },
        { 0x801, "Kryo-V2" },
        { 0x802, "Kryo-3XX-Gold" },
        { 0x803, "Kryo-3XX-Silver" },
        { 0x804, "Kryo-4XX-Gold" },
        { 0x805, "Kryo-4XX-Silver" },
        { 0xc00, "Falkor" },
        { 0xc01, "Saphira" },
        { -1, "unknown" },
    };

    static const struct id_part samsung_part[] = {
        { 0x001, "exynos-m1" },
        { 0x002, "exynos-m3" },
        { 0x003, "exynos-m4" },
        { 0x004, "exynos-m5" },
        { -1, "unknown" },
    };

    static const struct id_part nvidia_part[] = {
        { 0x000, "Denver" },
        { 0x003, "Denver 2" },
        { 0x004, "Carmel" },
        { -1, "unknown" },
    };

    static const struct id_part marvell_part[] = {
        { 0x131, "Feroceon-88FR131" },
        { 0x581, "PJ4/PJ4b" },
        { 0x584, "PJ4B-MP" },
        { -1, "unknown" },
    };

    static const struct id_part apple_part[] = {
        { 0x000, "Swift" },
        { 0x001, "Cyclone" },
        { 0x002, "Typhoon" },
        { 0x003, "Typhoon/Capri" },
        { 0x004, "Twister" },
        { 0x005, "Twister/Elba/Malta" },
        { 0x006, "Hurricane" },
        { 0x007, "Hurricane/Myst" },
        { 0x008, "Monsoon" },
        { 0x009, "Mistral" },
        { 0x00b, "Vortex" },
        { 0x00c, "Tempest" },
        { 0x00f, "Tempest-M9" },
        { 0x010, "Vortex/Aruba" },
        { 0x011, "Tempest/Aruba" },
        { 0x012, "Lightning" },
        { 0x013, "Thunder" },
        { 0x020, "Icestorm-A14" },
        { 0x021, "Firestorm-A14" },
        { 0x022, "Icestorm-M1" },
        { 0x023, "Firestorm-M1" },
        { 0x024, "Icestorm-M1-Pro" },
        { 0x025, "Firestorm-M1-Pro" },
        { 0x026, "Thunder-M10" },
        { 0x028, "Icestorm-M1-Max" },
        { 0x029, "Firestorm-M1-Max" },
        { 0x030, "Blizzard-A15" },
        { 0x031, "Avalanche-A15" },
        { 0x032, "Blizzard-M2" },
        { 0x033, "Avalanche-M2" },
        { 0x034, "Blizzard-M2-Pro" },
        { 0x035, "Avalanche-M2-Pro" },
        { 0x036, "Sawtooth-A16" },
        { 0x037, "Everest-A16" },
        { 0x038, "Blizzard-M2-Max" },
        { 0x039, "Avalanche-M2-Max" },
        { -1, "unknown" },
    };

    static const struct id_part faraday_part[] = {
        { 0x526, "FA526" },
        { 0x626, "FA626" },
        { -1, "unknown" },
    };

    static const struct id_part intel_part[] = {
        { 0x200, "i80200" },
        { 0x210, "PXA250A" },
        { 0x212, "PXA210A" },
        { 0x242, "i80321-400" },
        { 0x243, "i80321-600" },
        { 0x290, "PXA250B/PXA26x" },
        { 0x292, "PXA210B" },
        { 0x2c2, "i80321-400-B0" },
        { 0x2c3, "i80321-600-B0" },
        { 0x2d0, "PXA250C/PXA255/PXA26x" },
        { 0x2d2, "PXA210C" },
        { 0x411, "PXA27x" },
        { 0x41c, "IPX425-533" },
        { 0x41d, "IPX425-400" },
        { 0x41f, "IPX425-266" },
        { 0x682, "PXA32x" },
        { 0x683, "PXA930/PXA935" },
        { 0x688, "PXA30x" },
        { 0x689, "PXA31x" },
        { 0xb11, "SA1110" },
        { 0xc12, "IPX1200" },
        { -1, "unknown" },
    };

    static const struct id_part fujitsu_part[] = {
        { 0x001, "A64FX" },
        { -1, "unknown" },
    };

    static const struct id_part hisi_part[] = {
        { 0xd01, "TaiShan-v110" },	
        { 0xd02, "TaiShan-v120" },	
        { 0xd40, "Cortex-A76" },	
        { 0xd41, "Cortex-A77" },	
        { -1, "unknown" },
    };

    static const struct id_part ampere_part[] = {
        { 0xac3, "Ampere-1" },
        { 0xac4, "Ampere-1a" },
        { -1, "unknown" },
    };

    static const struct id_part ft_part[] = {
        { 0x303, "FTC310" },
        { 0x660, "FTC660" },
        { 0x661, "FTC661" },
        { 0x662, "FTC662" },
        { 0x663, "FTC663" },
        { 0x664, "FTC664" },
        { 0x862, "FTC862" },
        { -1, "unknown" },
    };

    static const struct id_part ms_part[] = {
        { 0xd49, "Azure-Cobalt-100" },
        { -1, "unknown" },
    };

    static const struct id_part unknown_part[] = {
        { -1, "unknown" },
    };

    struct hw_impl {
       const int    id;
       const struct id_part     *parts;
       const char   *name;
    };

    static const struct hw_impl hw_implementer[] = {
        { 0x41, arm_part,     "ARM" },
        { 0x42, brcm_part,    "Broadcom" },
        { 0x43, cavium_part,  "Cavium" },
        { 0x44, dec_part,     "DEC" },
        { 0x46, fujitsu_part, "FUJITSU" },
        { 0x48, hisi_part,    "HiSilicon" },
        { 0x49, unknown_part, "Infineon" },
        { 0x4d, unknown_part, "Motorola/Freescale" },
        { 0x4e, nvidia_part,  "NVIDIA" },
        { 0x50, apm_part,     "APM" },
        { 0x51, qcom_part,    "Qualcomm" },
        { 0x53, samsung_part, "Samsung" },
        { 0x56, marvell_part, "Marvell" },
        { 0x61, apple_part,   "Apple" },
        { 0x66, faraday_part, "Faraday" },
        { 0x69, intel_part,   "Intel" },
        { 0x6d, ms_part,      "Microsoft" },
        { 0x70, ft_part,      "Phytium" },
        { 0xc0, ampere_part,  "Ampere" },
        { -1,   unknown_part, "unknown" },
    };
    /* clang-format on */

    (void)Tokenizer(keyValuePairs["CPU implementer"_ns])
        .ReadHexadecimal(&cpuFamily);

    (void)Tokenizer(keyValuePairs["CPU part"_ns]).ReadHexadecimal(&cpuModel);

    (void)Tokenizer(keyValuePairs["CPU variant"_ns])
        .ReadHexadecimal(&cpuStepping);

    for (auto& hw_impl : hw_implementer) {
      if (hw_impl.id == (int)cpuFamily) {
        cpuVendor.Assign(hw_impl.name);
        for (auto* p = &hw_impl.parts[0]; p->id != -1; ++p) {
          if (p->id == (int)cpuModel) {
            cpuName.Assign(p->name);
          }
        }
      }
    }
#  else
    cpuVendor.Assign(keyValuePairs["vendor_id"_ns]);

    cpuName.Assign(keyValuePairs["model name"_ns]);

    (void)Tokenizer(keyValuePairs["cpu family"_ns]).ReadInteger(&cpuFamily);

    (void)Tokenizer(keyValuePairs["model"_ns]).ReadInteger(&cpuModel);

    (void)Tokenizer(keyValuePairs["stepping"_ns]).ReadInteger(&cpuStepping);
#  endif
  }

  {
    std::ifstream input(
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    std::string line;
    if (getline(input, line)) {
      (void)Tokenizer(line.c_str()).ReadInteger(&cpuSpeed);
      cpuSpeed /= 1000;
    }
  }

  {
    std::ifstream input("/sys/devices/system/cpu/cpu0/cache/index2/size");
    std::string line;
    if (getline(input, line)) {
      (void)Tokenizer(line.c_str(), nullptr, "K").ReadInteger(&cacheSizeL2);
    }
  }

  {
    std::ifstream input("/sys/devices/system/cpu/cpu0/cache/index3/size");
    std::string line;
    if (getline(input, line)) {
      (void)Tokenizer(line.c_str(), nullptr, "K").ReadInteger(&cacheSizeL3);
    }
  }

  info.cpuCount = PR_GetNumberOfProcessors();
  if (XRE_IsParentProcess()) {

  }
  int max_cpu_bits = [&] {
    std::ifstream input("/sys/devices/system/cpu/possible");
    std::string line;
    if (getline(input, line)) {
      int num;
      Tokenizer p(line.c_str());
      if (p.ReadInteger(&num) && num == 0 && p.CheckChar('-') &&
          p.ReadInteger(&num) && p.CheckEOF()) {
        return num + 1;
      }
    }
    return info.cpuCount;
  }();

  constexpr int mask_bits = sizeof(uint32_t) * 8;

  Vector<uint32_t> cpumasks;
  physicalCPUs = [&] {
    int cores = 0;
    if (!cpumasks.appendN(0, (max_cpu_bits + mask_bits - 1) / mask_bits)) {
      return -1;
    }
    for (int32_t cpu = 0; cpu < info.cpuCount; ++cpu) {
      nsPrintfCString core_cpus(
          "/sys/devices/system/cpu/cpu%d/topology/core_cpus", cpu);
      std::ifstream input(core_cpus.get());
      if (input.fail()) {
        core_cpus.Truncate(core_cpus.Length() - sizeof("core_cpus") + 1);
        core_cpus.AppendLiteral("thread_siblings");
        input.open(core_cpus.get());
      }
      std::string line;
      if (!getline(input, line)) {
        return -1;
      }
      Tokenizer p(line.c_str());
      bool unknown_core = false;
      for (auto& mask : cpumasks) {
        uint32_t m;
        if (NS_WARN_IF(!p.ReadHexadecimal(&m,  false))) {
          return -1;
        }
        if (!p.CheckEOF() && !p.CheckChar(',')) {
          return -1;
        }
        if ((mask & m) != m) {
          unknown_core = true;
        }
        mask |= m;
      }
      if (unknown_core) {
        cores++;
      }
    }
    return cores;
  }();

  if (Maybe<hal::HeterogeneousCpuInfo> hetCpuInfo =
          hal::GetHeterogeneousCpuInfo()) {
    info.cpuPCount = int32_t(hetCpuInfo->mBigCpus.Count());
    info.cpuMCount = int32_t(hetCpuInfo->mMediumCpus.Count());
    info.cpuECount = int32_t(hetCpuInfo->mLittleCpus.Count());
    if (XRE_IsParentProcess()) {



    }
  } else {
    info.cpuPCount = physicalCPUs;
    if (XRE_IsParentProcess()) {

    }
    info.cpuMCount = 0;
    info.cpuECount = 0;
  }

  if (cpuSpeed >= 0) {
    info.cpuSpeed = cpuSpeed;
    if (XRE_IsParentProcess()) {

    }
  } else {
    info.cpuSpeed = 0;
  }
  if (!cpuVendor.IsEmpty()) {
    info.cpuVendor = cpuVendor;
    if (XRE_IsParentProcess()) {

    }
  }
  if (!cpuName.IsEmpty()) {
    info.cpuName = cpuName;
    if (XRE_IsParentProcess()) {

    }
  }
  if (cpuFamily >= 0) {
    info.cpuFamily = cpuFamily;
    if (XRE_IsParentProcess()) {

    }
  }
  if (cpuModel >= 0) {
    info.cpuModel = cpuModel;
    if (XRE_IsParentProcess()) {

    }
  }
  if (cpuStepping >= 0) {
    info.cpuStepping = cpuStepping;
    if (XRE_IsParentProcess()) {

    }
  }

  if (logicalCPUs >= 0) {
    info.cpuCount = logicalCPUs;
    if (XRE_IsParentProcess()) {

    }
  }
  if (physicalCPUs >= 0) {
    info.cpuCores = physicalCPUs;
    if (XRE_IsParentProcess()) {

    }
  }

  if (cacheSizeL2 >= 0) {
    info.l2cacheKB = cacheSizeL2;
    if (XRE_IsParentProcess()) {

    }
  }
  if (cacheSizeL3 >= 0) {
    info.l3cacheKB = cacheSizeL3;
    if (XRE_IsParentProcess()) {

    }
  }

  return NS_OK;
}

#if defined(__MINGW32__)
WINBASEAPI
BOOL WINAPI IsUserCetAvailableInEnvironment(_In_ DWORD UserCetEnvironment);

#  define USER_CET_ENVIRONMENT_WIN32_PROCESS 0x00000000
#endif

nsresult nsSystemInfo::Init() {
  MOZ_ASSERT(NS_IsMainThread());

  nsresult rv;

  static const struct {
    PRSysInfo cmd;
    const char* name;
  } items[] = {{PR_SI_SYSNAME, "name"},
               {PR_SI_ARCHITECTURE, "arch"},
               {PR_SI_RELEASE, "version"},
               {PR_SI_RELEASE_BUILD, "build"}};

  for (auto item : items) {
    char buf[SYS_INFO_BUFFER_LENGTH];
    if (PR_GetSystemInfo(item.cmd, buf, sizeof(buf)) == PR_SUCCESS) {
      rv = SetPropertyAsACString(NS_ConvertASCIItoUTF16(item.name),
                                 nsDependentCString(buf));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    } else {
      NS_WARNING("PR_GetSystemInfo failed");
    }
  }

  SetPropertyAsBool(u"isPackagedApp"_ns, false);

  SetInt32Property(u"pagesize"_ns, PR_GetPageSize());
  SetInt32Property(u"pageshift"_ns, PR_GetPageShift());
  SetInt32Property(u"memmapalign"_ns, PR_GetMemMapAlignment());
  SetUint64Property(u"memsize"_ns, PR_GetPhysicalMemorySize());
  SetUint32Property(u"umask"_ns, nsSystemInfo::gUserUmask);

#ifdef HAVE_64BIT_BUILD
  SetUint32Property(u"archbits"_ns, 64);
#else
  SetUint32Property(u"archbits"_ns, 32);
#endif

  uint64_t virtualMem = 0;

  if (virtualMem) SetUint64Property(u"virtualmemsize"_ns, virtualMem);

  for (auto cpuPropItem : cpuPropItems) {
    rv = SetPropertyAsBool(NS_ConvertASCIItoUTF16(cpuPropItem.name),
                           cpuPropItem.propfun());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }





  nsAutoCString productName;
  if (NS_SUCCEEDED(GetLinuxProductName(productName))) {
    rv = SetPropertyAsACString(u"linuxProductName"_ns, productName);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsAutoCString productSku;
  if (NS_SUCCEEDED(GetLinuxProductSku(productSku))) {
    rv = SetPropertyAsACString(u"linuxProductSku"_ns, productSku);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  {
    nsAutoCString themeInfo;
    LookAndFeel::GetThemeInfo(themeInfo);
    MOZ_TRY(SetPropertyAsACString(u"osThemeInfo"_ns, themeInfo));
  }

#if defined(MOZ_WIDGET_GTK)
  char gtkver[64];
  ssize_t gtkver_len = 0;

  if (gtkver_len <= 0) {
    gtkver_len = SprintfLiteral(gtkver, "GTK %u.%u.%u", gtk_major_version,
                                gtk_minor_version, gtk_micro_version);
  }

  nsAutoCString secondaryLibrary;
  if (gtkver_len > 0 && gtkver_len < int(sizeof(gtkver))) {
    secondaryLibrary.Append(nsDependentCSubstring(gtkver, gtkver_len));
  }

#  ifndef MOZ_TSAN
  void* libpulse = dlopen("libpulse.so.0", RTLD_LAZY);
  const char* libpulseVersion = "not-available";
  if (libpulse) {
    auto pa_get_library_version = reinterpret_cast<const char* (*)()>(
        dlsym(libpulse, "pa_get_library_version"));

    if (pa_get_library_version) {
      libpulseVersion = pa_get_library_version();
    }
  }

  secondaryLibrary.AppendPrintf(",libpulse %s", libpulseVersion);

  if (libpulse) {
    dlclose(libpulse);
  }
#  endif

  rv = SetPropertyAsACString(u"secondaryLibrary"_ns, secondaryLibrary);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
  rv = SetPropertyAsBool(u"isPackagedApp"_ns,
                         widget::IsRunningUnderFlatpakOrSnap() ||
                             widget::IsPackagedAppFileExists());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
#endif


  nsCString dist, desc, release, codename;
  if (widget::lsb::GetLSBRelease(dist, desc, release, codename)) {
    SetPropertyAsACString(u"distro"_ns, dist);
    SetPropertyAsACString(u"distroVersion"_ns, release);
  }

  if (XRE_IsParentProcess()) {
    nsCString libstdcxxVersion;

  }


  return NS_OK;
}


void nsSystemInfo::SetInt32Property(const nsAString& aPropertyName,
                                    const int32_t aValue) {
  NS_WARNING_ASSERTION(aValue > 0, "Unable to read system value");
  if (aValue > 0) {
#ifdef DEBUG
    nsresult rv =
#endif
        SetPropertyAsInt32(aPropertyName, aValue);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "Unable to set property");
  }
}

void nsSystemInfo::SetUint32Property(const nsAString& aPropertyName,
                                     const uint32_t aValue) {
#ifdef DEBUG
  nsresult rv =
#endif
      SetPropertyAsUint32(aPropertyName, aValue);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "Unable to set property");
}

void nsSystemInfo::SetUint64Property(const nsAString& aPropertyName,
                                     const uint64_t aValue) {
  NS_WARNING_ASSERTION(aValue > 0, "Unable to read system value");
  if (aValue > 0) {
#ifdef DEBUG
    nsresult rv =
#endif
        SetPropertyAsUint64(aPropertyName, aValue);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "Unable to set property");
  }
}


JSObject* GetJSObjForProcessInfo(JSContext* aCx, const ProcessInfo& info) {
  JS::Rooted<JSObject*> jsInfo(aCx, JS_NewPlainObject(aCx));


  JS::Rooted<JS::Value> valCountInfo(aCx, JS::Int32Value(info.cpuCount));
  JS_SetProperty(aCx, jsInfo, "count", valCountInfo);

  JS::Rooted<JS::Value> valCoreInfo(
      aCx, info.cpuCores ? JS::Int32Value(info.cpuCores) : JS::NullValue());
  JS_SetProperty(aCx, jsInfo, "cores", valCoreInfo);

  JS::Rooted<JS::Value> valPCountInfo(
      aCx, info.cpuCores ? JS::Int32Value(info.cpuPCount) : JS::NullValue());
  JS_SetProperty(aCx, jsInfo, "pcount", valPCountInfo);

  JS::Rooted<JS::Value> valMCountInfo(
      aCx, info.cpuCores ? JS::Int32Value(info.cpuMCount) : JS::NullValue());
  JS_SetProperty(aCx, jsInfo, "mcount", valMCountInfo);

  JS::Rooted<JS::Value> valECountInfo(
      aCx, info.cpuCores ? JS::Int32Value(info.cpuECount) : JS::NullValue());
  JS_SetProperty(aCx, jsInfo, "ecount", valECountInfo);

  JSString* strVendor =
      JS_NewStringCopyN(aCx, info.cpuVendor.get(), info.cpuVendor.Length());
  JS::Rooted<JS::Value> valVendor(aCx, JS::StringValue(strVendor));
  JS_SetProperty(aCx, jsInfo, "vendor", valVendor);

  JSString* strName =
      JS_NewStringCopyN(aCx, info.cpuName.get(), info.cpuName.Length());
  JS::Rooted<JS::Value> valName(aCx, JS::StringValue(strName));
  JS_SetProperty(aCx, jsInfo, "name", valName);

  JS::Rooted<JS::Value> valFamilyInfo(aCx, JS::Int32Value(info.cpuFamily));
  JS_SetProperty(aCx, jsInfo, "family", valFamilyInfo);

  JS::Rooted<JS::Value> valModelInfo(aCx, JS::Int32Value(info.cpuModel));
  JS_SetProperty(aCx, jsInfo, "model", valModelInfo);

  JS::Rooted<JS::Value> valSteppingInfo(aCx, JS::Int32Value(info.cpuStepping));
  JS_SetProperty(aCx, jsInfo, "stepping", valSteppingInfo);

  JS::Rooted<JS::Value> valL2CacheInfo(aCx, JS::Int32Value(info.l2cacheKB));
  JS_SetProperty(aCx, jsInfo, "l2cacheKB", valL2CacheInfo);

  JS::Rooted<JS::Value> valL3CacheInfo(aCx, JS::Int32Value(info.l3cacheKB));
  JS_SetProperty(aCx, jsInfo, "l3cacheKB", valL3CacheInfo);

  JS::Rooted<JS::Value> valSpeedInfo(aCx, JS::Int32Value(info.cpuSpeed));
  JS_SetProperty(aCx, jsInfo, "speedMHz", valSpeedInfo);

  return jsInfo;
}

RefPtr<nsISerialEventTarget> nsSystemInfo::GetBackgroundTarget() {
  if (!mBackgroundET) {
    MOZ_ALWAYS_SUCCEEDS(NS_CreateBackgroundTaskQueue(
        "SystemInfoThread", getter_AddRefs(mBackgroundET)));
  }
  return mBackgroundET;
}

NS_IMETHODIMP
nsSystemInfo::GetOsInfo(JSContext* aCx, Promise** aResult) {
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = nullptr;
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSystemInfo::GetDiskInfo(JSContext* aCx, Promise** aResult) {
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = nullptr;
  if (!XRE_IsParentProcess()) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED(nsSystemInfo, nsHashPropertyBag, nsISystemInfo)

NS_IMETHODIMP
nsSystemInfo::GetCountryCode(JSContext* aCx, Promise** aResult) {
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = nullptr;

  if (!XRE_IsParentProcess()) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSystemInfo::GetProcessInfo(JSContext* aCx, Promise** aResult) {
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = nullptr;

  if (!XRE_IsParentProcess()) {
    return NS_ERROR_FAILURE;
  }

  nsIGlobalObject* global = xpc::CurrentNativeGlobal(aCx);
  if (NS_WARN_IF(!global)) {
    return NS_ERROR_FAILURE;
  }

  ErrorResult erv;
  RefPtr<Promise> promise = Promise::Create(global, erv);
  if (NS_WARN_IF(erv.Failed())) {
    return erv.StealNSResult();
  }

  if (!mProcessInfoPromise) {
    RefPtr<nsISerialEventTarget> backgroundET = GetBackgroundTarget();

    mProcessInfoPromise = InvokeAsync(backgroundET, __func__, []() {
      ProcessInfo info;
      nsresult rv = CollectProcessInfo(info);
      if (NS_SUCCEEDED(rv)) {
        return ProcessInfoPromise::CreateAndResolve(info, __func__);
      }
      return ProcessInfoPromise::CreateAndReject(rv, __func__);
    });
  };

  auto requestHolder =
      MakeRefPtr<dom::DOMMozPromiseRequestHolder<ProcessInfoPromise>>(global);

  RefPtr<Promise> capturedPromise = promise;
  mProcessInfoPromise
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          [requestHolder, capturedPromise](const ProcessInfo& info) {
            requestHolder->Complete();
            AutoJSAPI jsapi;
            if (NS_WARN_IF(!jsapi.Init(capturedPromise->GetGlobalObject()))) {
              capturedPromise->MaybeReject(NS_ERROR_UNEXPECTED);
              return;
            }
            JSContext* cx = jsapi.cx();
            JS::Rooted<JS::Value> val(
                cx, JS::ObjectValue(*GetJSObjForProcessInfo(cx, info)));
            capturedPromise->MaybeResolve(val);
          },
          [requestHolder, capturedPromise](const nsresult rv) {
            requestHolder->Complete();
            capturedPromise->MaybeResolve(JS::NullHandleValue);
          })
      ->Track(*requestHolder);

  promise.forget(aResult);

  return NS_OK;
}
