#include "src/backend/ml/cuda/numa_host_tensor.h"
#include <ATen/Context.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "detection_types.h"
#include "mask_pack_cuda.h"
#include "postprocess.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"

import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.models.rfdetr.core.dataset_utils;
import mmltk.backend.models.rfdetr.core.evaluator;
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

#define MMLTK_TRAINING_EVALUATION_RUNTIME
#include "detail/evaluation_runtime_private.inc"
static_assert(sizeof(mmltk::backend::models::rfdetr::PredictionBatchMetadata) > 0U);
#include "../core/detail/evaluator_implementation.inc"
#undef MMLTK_TRAINING_EVALUATION_RUNTIME
