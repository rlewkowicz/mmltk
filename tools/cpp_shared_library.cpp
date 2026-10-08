module;
#include <nlohmann/json.hpp>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>

export module mmltk.tools.cpp_shared_library;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

export {
namespace mmltk::tools {

class SharedLibrary {
public:
    explicit SharedLibrary(const char* name) : handle_(::dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
        if (!handle_) throw ToolError(::dlerror());
    }
    ~SharedLibrary() { ::dlclose(handle_); }
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    template<class Function = void*>
    Function symbol(const char* name, bool require_address = false) const {
        ::dlerror();
        auto* address = ::dlsym(handle_, name);
        if (const auto* error = ::dlerror()) throw ToolError(error);
        if (require_address && !address) throw ToolError(std::string(name) + " resolved to a null address");
        return reinterpret_cast<Function>(address);
    }
private:
    void* handle_;
};

} // namespace mmltk::tools

}
