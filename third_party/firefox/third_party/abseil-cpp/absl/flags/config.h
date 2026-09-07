//  Copyright 2019 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#if !defined(ABSL_FLAGS_CONFIG_H_)
#define ABSL_FLAGS_CONFIG_H_

#if !defined(ABSL_FLAGS_STRIP_NAMES)


#endif

#if !defined(ABSL_FLAGS_STRIP_NAMES)
#define ABSL_FLAGS_STRIP_NAMES 0
#endif

#if !defined(ABSL_FLAGS_STRIP_HELP)
#define ABSL_FLAGS_STRIP_HELP ABSL_FLAGS_STRIP_NAMES
#endif

#define ABSL_FLAGS_INTERNAL_BUILTIN_TYPES(A) \
  A(bool, bool)                              \
  A(short, short)                            \
  A(unsigned short, unsigned_short)          \
  A(int, int)                                \
  A(unsigned int, unsigned_int)              \
  A(long, long)                              \
  A(unsigned long, unsigned_long)            \
  A(long long, long_long)                    \
  A(unsigned long long, unsigned_long_long)  \
  A(double, double)                          \
  A(float, float)

#define ABSL_FLAGS_INTERNAL_SUPPORTED_TYPES(A) \
  ABSL_FLAGS_INTERNAL_BUILTIN_TYPES(A)         \
  A(std::string, std_string)                   \
  A(std::vector<std::string>, std_vector_of_string)

#endif
