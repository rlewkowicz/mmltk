#pragma once

#include <NvInfer.h>

#include "src/backend/ml/runtime/tensorrt_runtime.h"

namespace mmltk::backend::ml::runtime::detail {

class TensorRtEngineAccess final {
   public:
    [[nodiscard]] static nvinfer1::ICudaEngine& Get(const TensorRtEngine& owner) noexcept;
};

}  // namespace mmltk::backend::ml::runtime::detail
