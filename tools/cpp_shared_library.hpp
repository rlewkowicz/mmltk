#pragma once

#include "cpp_support.hpp"
#include <dlfcn.h>

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
