#pragma once

#include "mmltk/live/live_types.h"
#include "mmltk/rfdetr/evaluation.h"

#include <torch/torch.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mmltk::live {

struct AnalyzerSplitResult {
    LiveCaptureRegion source_region{};
    torch::Tensor boxes_xyxy;
    torch::Tensor labels_zero_based;
    torch::Tensor scores;
    torch::Tensor colors_rgb;
    torch::Tensor masks;
    std::vector<mmltk::rfdetr::Prediction> detections;
};

struct AnalyzerResult {
    LiveFrameId frame_id{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    cudaEvent_t ready_event = nullptr;
    cudaStream_t producer_stream = nullptr;
    std::vector<AnalyzerSplitResult> splits;
};

class FrameAnalyzer {
public:
    virtual ~FrameAnalyzer() = default;

    [[nodiscard]] virtual AnalyzerResult analyze(const DetectBundle& bundle) = 0;
    [[nodiscard]] virtual std::string backend_name() const = 0;
    [[nodiscard]] virtual std::uint32_t model_resolution() const = 0;
    [[nodiscard]] virtual int num_classes() const = 0;
};

} // namespace mmltk::live
