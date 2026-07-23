

#include <catch2/internal/catch_debugger.hpp>
#include <catch2/internal/catch_errno_guard.hpp>
#include <catch2/internal/catch_platform.hpp>
#include <catch2/internal/catch_stdstreams.hpp>

#if defined(CATCH_PLATFORM_MAC) || defined(CATCH_PLATFORM_IPHONE)

#include <cassert>
#include <sys/types.h>
#include <unistd.h>
#include <cstddef>
#include <ostream>

#ifdef __apple_build_version__
#include <sys/sysctl.h>
#endif

namespace Catch {
#ifdef __apple_build_version__

bool isDebuggerActive() {
    int mib[4];
    struct kinfo_proc info;
    std::size_t size;

    info.kp_proc.p_flag = 0;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    size = sizeof(info);
    if (sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, nullptr, 0) != 0) {
        Catch::cerr() << "\n** Call to sysctl failed - unable to determine if debugger is active **\n\n" << std::flush;
        return false;
    }

    return ((info.kp_proc.p_flag & P_TRACED) != 0);
}
#else
bool isDebuggerActive() {
    return false;
}
#endif
}  

#elif defined(CATCH_PLATFORM_LINUX) || defined(CATCH_PLATFORM_QNX)
#include <fstream>
#include <string>

namespace Catch {
bool isDebuggerActive() {
    ErrnoGuard guard;
    std::ifstream in("/proc/self/status");
    for (std::string line; std::getline(in, line);) {
        static const int PREFIX_LEN = 11;
        if (line.compare(0, PREFIX_LEN, "TracerPid:\t") == 0) {
            return line.length() > PREFIX_LEN && line[PREFIX_LEN] != '0';
        }
    }

    return false;
}
}  
#elif defined(_MSC_VER)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent();
namespace Catch {
bool isDebuggerActive() {
    return IsDebuggerPresent() != 0;
}
}  
#elif defined(__MINGW32__)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent();
namespace Catch {
bool isDebuggerActive() {
    return IsDebuggerPresent() != 0;
}
}  
#else
namespace Catch {
bool isDebuggerActive() {
    return false;
}
}  
#endif  // Platform
