#pragma once
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_authority.h"
#include <catch2/catch_test_macros.hpp>
namespace mmltk::frameworks::gpu::test_support {
inline void check_terminal_occupancy(const TerminalCudaRetirementAuthority& authority, bool terminal) {
 CHECK(authority.admission_open() == !terminal);
 CHECK(authority.fact().occupancy == (terminal ? 1U : 0U));
}
}  // namespace mmltk::frameworks::gpu::test_support
