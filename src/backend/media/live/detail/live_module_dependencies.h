#pragma once

// Live's private declarations are attached to the Live implementation module
// after its imports. Materialize every C/C++ library dependency in the global
// module fragment first so those headers never acquire Live module linkage.
#include <cuda.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"

#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/device_execution.h"
