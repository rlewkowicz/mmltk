// Copyright 2023 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef ABSL_BASE_NULLABILITY_H_
#define ABSL_BASE_NULLABILITY_H_

#include "absl/base/config.h"

#define ABSL_POINTERS_DEFAULT_NONNULL

#if defined(__clang__) && !defined(__OBJC__) && \
    ABSL_HAVE_FEATURE(nullability_on_classes) && \
    0 
#define absl_nonnull _Nonnull

#define absl_nullable _Nullable

#define absl_nullability_unknown _Null_unspecified
#else
#define absl_nonnull
#define absl_nullable
#define absl_nullability_unknown
#endif

#if ABSL_HAVE_FEATURE(nullability_on_classes)
#define ABSL_NULLABILITY_COMPATIBLE _Nullable
#else
#define ABSL_NULLABILITY_COMPATIBLE
#endif

#endif  // ABSL_BASE_NULLABILITY_H_
