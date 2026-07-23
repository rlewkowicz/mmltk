// Copyright 2019 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef ABSL_STATUS_STATUS_PAYLOAD_PRINTER_H_
#define ABSL_STATUS_STATUS_PAYLOAD_PRINTER_H_

#include <optional>
#include <string>

#include "absl/base/nullability.h"
#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace status_internal {

using StatusPayloadPrinter = std::optional<std::string> (*absl_nullable)(
    absl::string_view, const absl::Cord&);

void SetStatusPayloadPrinter(StatusPayloadPrinter);

StatusPayloadPrinter GetStatusPayloadPrinter();

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STATUS_STATUS_PAYLOAD_PRINTER_H_
