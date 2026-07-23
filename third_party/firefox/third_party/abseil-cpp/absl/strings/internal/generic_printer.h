// Copyright 2025 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_STRINGS_INTERNAL_GENERIC_PRINTER_H_
#define ABSL_STRINGS_INTERNAL_GENERIC_PRINTER_H_

#include "absl/strings/internal/generic_printer_internal.h"  // IWYU pragma: export


#include <ostream>
#include <utility>

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace strings_internal {

inline constexpr internal_generic_printer::GenericPrintAdapterFactory
    GenericPrint{};

template <typename T>
using GenericPrinter = internal_generic_printer::GenericPrinter<T>;

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_GENERIC_PRINTER_H_
