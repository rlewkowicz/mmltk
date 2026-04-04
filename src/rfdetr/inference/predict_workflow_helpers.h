#pragma once

#include "rfdetr/predict_runtime_internal.h"
#include "image_resize.h"

#include "stb_image.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::rfdetr::predict_internal {

using json = nlohmann::json;

inline void ensure_raw_image_resize_scratch(std::vector<std::uint8_t>& resize_scratch,
                                            std::size_t capacity) {
    if (resize_scratch.capacity() < capacity) {
        resize_scratch.reserve(capacity);
    }
}

inline void hwc_uint8_to_nchw_float(const std::uint8_t* src,
                                    float* dst,
                                    int height,
                                    int width) {
    const int hw = height * width;
    const float scale = 1.0f / 255.0f;
    float* dst_r = dst;
    float* dst_g = dst + hw;
    float* dst_b = dst + static_cast<ptrdiff_t>(hw) * 2;
    for (int index = 0; index < hw; ++index) {
        const std::size_t pixel_offset = static_cast<std::size_t>(index) * 3U;
        dst_r[index] = static_cast<float>(src[pixel_offset]) * scale;
        dst_g[index] = static_cast<float>(src[pixel_offset + 1U]) * scale;
        dst_b[index] = static_cast<float>(src[pixel_offset + 2U]) * scale;
    }
}

inline RgbImageResizer& raw_image_resizer() {
    thread_local RgbImageResizer resizer(1);
    return resizer;
}

inline RawImageBatchItem decode_raw_image_into_tensor(const PredictImageInput& input,
                                                      int64_t dataset_index,
                                                      int64_t default_image_id,
                                                      int resolution,
                                                      float* destination,
                                                      std::vector<std::uint8_t>& resize_scratch) {
    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    stbi_uc* raw_pixels = stbi_load(input.image_path.c_str(), &raw_width, &raw_height, &raw_channels, 3);
    if (raw_pixels == nullptr) {
        throw std::runtime_error("failed to load prediction image: " + input.image_path.string());
    }
    if (raw_width <= 0 || raw_height <= 0) {
        stbi_image_free(raw_pixels);
        throw std::runtime_error("invalid prediction image dimensions: " + input.image_path.string());
    }

    const std::uint8_t* resized_pixels = raw_pixels;
    if (raw_width != resolution || raw_height != resolution) {
        const std::size_t resize_bytes = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution) * 3U;
        ensure_raw_image_resize_scratch(resize_scratch, resize_bytes);
        resize_scratch.resize(resize_bytes);
        raw_image_resizer().resize(raw_pixels,
                                   raw_width,
                                   raw_height,
                                   resize_scratch.data(),
                                   resolution,
                                   resolution);
        resized_pixels = resize_scratch.data();
    }

    hwc_uint8_to_nchw_float(resized_pixels, destination, resolution, resolution);
    stbi_image_free(raw_pixels);

    RawImageBatchItem item;
    item.dataset_index = dataset_index;
    item.image_id = input.image_id != 0 ? input.image_id : default_image_id;
    item.source_name = input.source_name.empty() ? input.image_path.string() : input.source_name;
    item.original_height = raw_height;
    item.original_width = raw_width;
    return item;
}

inline std::vector<RawImageBatchItem> load_raw_image_batch(WorkerPool& cpu_pool,
                                                           const std::vector<PredictImageInput>& inputs,
                                                           size_t start_index,
                                                           size_t batch_count,
                                                           int resolution,
                                                           const torch::Tensor& batch_cpu,
                                                           std::vector<std::vector<std::uint8_t>>& resize_scratch_slots) {
    std::vector<std::future<RawImageBatchItem>> futures;
    futures.reserve(batch_count);
    if (resize_scratch_slots.size() < batch_count) {
        resize_scratch_slots.resize(batch_count);
    }
    const std::size_t resize_capacity = static_cast<std::size_t>(resolution) *
                                        static_cast<std::size_t>(resolution) * 3U;
    for (size_t offset = 0; offset < batch_count; ++offset) {
        PredictImageInput input = inputs[start_index + offset];
        torch::Tensor batch_slot = batch_cpu[static_cast<int64_t>(offset)];
        std::vector<std::uint8_t>& resize_scratch = resize_scratch_slots[offset];
        if (resize_scratch.capacity() < resize_capacity) {
            resize_scratch.reserve(resize_capacity);
        }
        futures.push_back(cpu_pool.enqueue([input = std::move(input),
                                            dataset_index = static_cast<int64_t>(start_index + offset),
                                            image_id = static_cast<int64_t>(start_index + offset + 1),
                                            resolution,
                                            batch_slot = std::move(batch_slot),
                                            &resize_scratch]() mutable {
            return decode_raw_image_into_tensor(input,
                                                dataset_index,
                                                image_id,
                                                resolution,
                                                batch_slot.data_ptr<float>(),
                                                resize_scratch);
        }));
    }

    std::vector<RawImageBatchItem> items;
    items.reserve(batch_count);
    for (auto& future : futures) {
        items.push_back(future.get());
    }
    return items;
}

inline torch::Tensor raw_image_target_sizes(const std::vector<RawImageBatchItem>& items,
                                            int device_id,
                                            const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
    std::vector<int64_t> target_sizes;
    target_sizes.reserve(items.size() * 2);
    for (const RawImageBatchItem& item : items) {
        target_sizes.push_back(item.original_height);
        target_sizes.push_back(item.original_width);
    }

    const auto options = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, device_id);
    if (!stream.has_value()) {
        return torch::tensor(target_sizes, options)
            .view({static_cast<int64_t>(items.size()), 2});
    }
    c10::cuda::CUDAStreamGuard stream_guard(*stream);
    return torch::tensor(target_sizes, options)
        .view({static_cast<int64_t>(items.size()), 2});
}

inline torch::Tensor raw_image_target_sizes(const RawImageBatchItem& item,
                                            int device_id,
                                            const std::optional<c10::cuda::CUDAStream>& stream = std::nullopt) {
    const std::vector<int64_t> target_sizes{item.original_height, item.original_width};
    const auto options = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, device_id);
    if (!stream.has_value()) {
        return torch::tensor(target_sizes, options).view({1, 2});
    }
    c10::cuda::CUDAStreamGuard stream_guard(*stream);
    return torch::tensor(target_sizes, options).view({1, 2});
}

inline std::string format_prediction_summary(const PredictOptions& options,
                                             const PredictionRunResult& result) {
    const char* source_kind = options.source_kind == PredictSourceKind::CompiledDataset
                                  ? "compiled_dataset"
                                  : "image_files";
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(4);
    stream << "rfdetr predict[" << result.backend_name << "]: source=" << source_kind
           << " input=" << result.artifacts.input_path.string()
           << " preset=" << result.artifacts.config.preset_name
           << " records=" << result.records.size()
           << " threshold=" << options.threshold;
    stream.precision(2);
    stream << " img_per_s=" << result.timing.img_per_s
           << " output=" << options.output_path.string();
    return stream.str();
}

inline std::string format_evaluation_summary(const EvaluateOptions&,
                                             const EvaluationRunResult& result) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(6);
    stream << "rfdetr evaluate[" << result.backend_name << "]: input=" << result.artifacts.input_path.string()
           << " preset=" << result.artifacts.config.preset_name
           << " images=" << result.image_count
           << " bbox_ap=" << result.result.summary.bbox.ap
           << " bbox_ap50=" << result.result.summary.bbox.ap50
           << " bbox_ap75=" << result.result.summary.bbox.ap75
           << " mask_ap=" << result.result.summary.mask.ap
           << " mask_ap50=" << result.result.summary.mask.ap50
           << " mask_ap75=" << result.result.summary.mask.ap75;
    stream.precision(2);
    stream << " img_per_s=" << result.result.timing.img_per_s;
    return stream.str();
}

inline std::string encode_mask_rle(const EncodedMask& mask) {
    std::string encoded;
    for (size_t index = 0; index < mask.runs.size(); ++index) {
        if (index > 0) {
            encoded.push_back(' ');
        }
        encoded += std::to_string(mask.runs[index].first);
        encoded.push_back(':');
        encoded += std::to_string(mask.runs[index].second);
    }
    return encoded;
}

inline json prediction_record_to_json(const PredictionRecord& record,
                                     const std::vector<std::string>& class_names) {
    json detections = json::array();
    for (const Prediction& prediction : record.detections) {
        const int class_index = prediction.category_id - 1;
        std::string label = std::to_string(prediction.category_id);
        if (class_index >= 0 && static_cast<size_t>(class_index) < class_names.size()) {
            label = class_names[static_cast<size_t>(class_index)];
        }
        json detection = {
            {"label", std::move(label)},
            {"score", prediction.score},
            {"xyxy", prediction.bbox_xyxy},
        };
        if (prediction.has_mask) {
            detection["mask_rle"] = encode_mask_rle(prediction.mask);
        }
        detections.push_back(std::move(detection));
    }

    json payload = {
        {"dataset_index", record.dataset_index},
        {"image_id", record.image_id},
        {"detections", std::move(detections)},
    };
    if (!record.source_name.empty()) {
        payload["source_name"] = record.source_name;
    }
    return payload;
}

inline void append_raw_prediction_records(std::vector<PredictionRecord>& records,
                                          std::vector<RawImageBatchItem>& items,
                                          const std::vector<TensorMap>& outputs,
                                          std::size_t category_count,
                                          std::size_t max_dets_per_image,
                                          float threshold) {
    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        auto detections = filter_threshold(
            result_to_predictions(static_cast<int>(items[item_index].image_id),
                                  outputs[item_index],
                                  category_count,
                                  max_dets_per_image),
            threshold);
        records.push_back(make_prediction_record(items[item_index].dataset_index,
                                                 items[item_index].image_id,
                                                 std::move(items[item_index].source_name),
                                                 std::move(detections)));
    }
}

} // namespace mmltk::rfdetr::predict_internal
