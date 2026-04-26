#pragma once

#include "mmltk/live/live_capture_region.h"
#include "mmltk/rfdetr/artifacts.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mmltk::live {
class FrameAnalyzer;
}

namespace mmltk::rfdetr {

using LiveCaptureRegion = mmltk::live::LiveCaptureRegion;

struct LiveVideoSourceOptions {
    std::string device_path = "/dev/video0";
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 120;
    uint32_t v4l2_buffer_count = 1;
    uint32_t preview_buffer_count = 1;
    LiveCaptureRegion initial_region{};
};

struct LivePredictOptions : ModelArtifactRequest {
    LiveVideoSourceOptions source;
    std::string backend = "auto";
    size_t max_dets_per_image = 500;
    uint32_t split_count = 1;
    int device_id = 0;
    float threshold = 0.0f;
    bool include_masks = false;
    bool include_status_detections = false;
    bool allow_fp16 = true;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

bool live_capture_supported();
std::unique_ptr<mmltk::live::FrameAnalyzer> make_live_rfdetr_frame_analyzer(const LivePredictOptions& options);

}  // namespace mmltk::rfdetr
