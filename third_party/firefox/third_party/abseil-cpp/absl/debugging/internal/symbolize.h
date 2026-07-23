// Copyright 2018 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#if !defined(ABSL_DEBUGGING_INTERNAL_SYMBOLIZE_H_)
#define ABSL_DEBUGGING_INTERNAL_SYMBOLIZE_H_

#if defined(__cplusplus)

#include <cstddef>
#include <cstdint>
#include <memory>

#include "absl/base/config.h"
#include "absl/strings/string_view.h"

#if defined(ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE)
#error ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE cannot be directly set
#elif defined(__ELF__) && defined(__GLIBC__) && !defined(__asmjs__) \
      && !defined(__wasm__)
#define ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE 1

#include <elf.h>
#include <link.h>  // For ElfW() macro.
#include <functional>

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

bool ForEachSection(int fd,
                    const std::function<bool(absl::string_view name,
                                             const ElfW(Shdr) &)>& callback);

bool GetSectionHeaderByName(int fd, const char *name, size_t name_len,
                            ElfW(Shdr) *out);

}  
ABSL_NAMESPACE_END
}  

#endif

#if defined(ABSL_INTERNAL_HAVE_DARWIN_SYMBOLIZE)
#error ABSL_INTERNAL_HAVE_DARWIN_SYMBOLIZE cannot be directly set
#endif

#if defined(ABSL_INTERNAL_HAVE_EMSCRIPTEN_SYMBOLIZE)
#error ABSL_INTERNAL_HAVE_EMSCRIPTEN_SYMBOLIZE cannot be directly set
#elif defined(__EMSCRIPTEN__)
#define ABSL_INTERNAL_HAVE_EMSCRIPTEN_SYMBOLIZE 1
#endif

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

class SymbolDecorator;

class SymbolDecoratorDeleter {
 public:
  void operator()(SymbolDecorator* ptr);
};

using SymbolDecoratorPtr =
    std::unique_ptr<SymbolDecorator, SymbolDecoratorDeleter>;

class SymbolDecorator {
 public:
  using Factory = SymbolDecoratorPtr(int fd);

  virtual ~SymbolDecorator() = default;

  virtual void Decorate(
      const void* pc,
      ptrdiff_t relocation,
      char* symbol_buf, size_t symbol_buf_size,
      char* tmp_buf, size_t tmp_buf_size) const = 0;
};

SymbolDecorator::Factory* SetSymbolDecoratorFactory(
    SymbolDecorator::Factory* factory);

bool RegisterFileMappingHint(const void* start, const void* end,
                             uint64_t offset, const char* filename);

bool GetFileMappingHint(const void** start, const void** end, uint64_t* offset,
                        const char** filename);

}  
ABSL_NAMESPACE_END
}  

#endif

#include <stdbool.h>

#if defined(__cplusplus)
extern "C"
#endif

    bool
    AbslInternalGetFileMappingHint(const void** start, const void** end,
                                   uint64_t* offset, const char** filename);

#endif
