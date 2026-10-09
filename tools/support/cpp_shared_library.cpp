module;
#include <dlfcn.h>
export module mmltk.tools.cpp_shared_library;
import std;
import mmltk.tools.cpp_support;
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
  template <class Function = void*>
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
 }  // namespace mmltk::tools
}
