#include "detail/shiftlut_onnx_ops.h"
#include <onnxruntime_cxx_api.h>

#define ONNX_NAMESPACE mmltk_onnx
#include <onnx/checker.h>
#include <onnx/onnx_pb.h>

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

namespace onnx = mmltk_onnx;

void image_type(onnx::ValueInfoProto& value, const char* name, bool output) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    auto* shape = tensor->mutable_shape();
    shape->add_dim()->set_dim_value(1);
    shape->add_dim()->set_dim_value(3);
    shape->add_dim()->set_dim_param(output ? "height4" : "height");
    shape->add_dim()->set_dim_param(output ? "width4" : "width");
}

void metadata(onnx::ModelProto& model, const char* name, std::string_view value) {
    auto* item = model.add_metadata_props();
    item->set_key(name);
    item->set_value(std::string(value));
}

void generate(const std::filesystem::path& tables, const std::filesystem::path& output,
              std::string_view lut_digest, std::string_view source_digest) {
    namespace lut = mmltk::backend::imaging::upscale::shiftlut;
    static_assert(std::endian::native == std::endian::little);
    const std::size_t bytes = lut::kTableElements * sizeof(float);
    if (std::filesystem::file_size(tables) != bytes) throw std::invalid_argument("invalid checked ShiftLUT interchange length");
    std::string data(bytes, '\0');
    std::ifstream input(tables, std::ios::binary);
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size()))) throw std::runtime_error("read ShiftLUT interchange");
    onnx::ModelProto model;
    model.set_ir_version(10);
    model.set_producer_name("mmltk_shiftlut_model_generator");
    model.set_producer_version("1");
    auto* standard = model.add_opset_import();
    standard->set_domain("");
    standard->set_version(18);
    auto* custom = model.add_opset_import();
    custom->set_domain(lut::kDomain);
    custom->set_version(lut::kVersion);
    auto* graph = model.mutable_graph();
    graph->set_name("ShiftLUT_s7_strict_fp32");
    image_type(*graph->add_input(), "image", false);
    image_type(*graph->add_output(), "upscaled", true);
    auto* initializer = graph->add_initializer();
    initializer->set_name("tables");
    initializer->set_data_type(onnx::TensorProto::FLOAT);
    initializer->add_dims(static_cast<std::int64_t>(lut::kTableElements));
    initializer->set_raw_data(std::move(data));
    auto* node = graph->add_node();
    node->set_name("resident_s7");
    node->set_domain(lut::kDomain);
    node->set_op_type(lut::kOperator);
    node->add_input("image");
    node->add_input("tables");
    node->add_output("upscaled");
    metadata(model, "mmltk.input_range", "0..255 RGB FP32");
    metadata(model, "mmltk.output_range", "0..255 RGB FP32");
    metadata(model, "mmltk.precision", "strict-fp32");
    metadata(model, "mmltk.source_artifact", "LUT_test/LUTs/ShiftLUT_sr_s7_int");
    metadata(model, "mmltk.source_assets_sha256", lut_digest);
    metadata(model, "mmltk.source_inference_sha256", source_digest);
    metadata(model, "mmltk.source_inference", "LUT_test/sr/model_LUT.py::inference");
    onnx::checker::check_model(model);
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!model.SerializeToOstream(&stream)) throw std::runtime_error("write ShiftLUT model");
    stream.close();
    if (!stream) throw std::runtime_error("close ShiftLUT model");
}

void cuda_checked(cudaError_t status, const char* context) {
    if (status != cudaSuccess) throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
}

class VerificationStorage final {
   public:
    void Allocate() {
        cuda_checked(cudaMalloc(reinterpret_cast<void**>(&data_), (kInputElements + kOutputElements) * sizeof(float)),
                     "allocate fixed verification tensors");
        cuda_checked(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "create verification stream");
    }
    ~VerificationStorage() {
        if (stream_ != nullptr && cudaStreamSynchronize(stream_) != cudaSuccess) std::terminate();
        if (data_ != nullptr && cudaFree(data_) != cudaSuccess) std::terminate();
        if (stream_ != nullptr && cudaStreamDestroy(stream_) != cudaSuccess) std::terminate();
    }
    VerificationStorage() = default;
    VerificationStorage(const VerificationStorage&) = delete;
    VerificationStorage& operator=(const VerificationStorage&) = delete;
    float* Input() noexcept { return data_; }
    float* Output() noexcept { return data_ + kInputElements; }
    cudaStream_t Stream() noexcept { return stream_; }
   private:
    static constexpr std::size_t kInputElements = 3 * 256 * 256;
    static constexpr std::size_t kOutputElements = kInputElements * 16;
    float* data_ = nullptr;
    cudaStream_t stream_ = nullptr;
};

std::vector<float> read_vector(const std::filesystem::path& path, std::size_t count) {
    if (std::filesystem::file_size(path) != count * sizeof(float)) throw std::invalid_argument("invalid upstream oracle size");
    std::vector<float> values(count);
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(float))))
        throw std::runtime_error("read independent upstream oracle");
    return values;
}

void verify(const std::filesystem::path& path, const std::filesystem::path& directory,
            std::string_view lut_digest, std::string_view source_digest) {
    namespace lut = mmltk::backend::imaging::upscale::shiftlut;
    onnx::ModelProto graph;
    std::ifstream model_file(path, std::ios::binary);
    if (!graph.ParseFromIstream(&model_file)) throw std::invalid_argument("invalid production ONNX");
    const auto matches = [&](std::string_view key, std::string_view expected) {
        return std::any_of(graph.metadata_props().begin(), graph.metadata_props().end(), [&](const auto& property) {
            return property.key() == key && property.value() == expected;
        });
    };
    if (!matches("mmltk.source_assets_sha256", lut_digest) || !matches("mmltk.source_inference_sha256", source_digest))
        throw std::invalid_argument("production model does not match upstream source/LUT provenance");
    Ort::Env environment{ORT_LOGGING_LEVEL_ERROR, "shiftlut_upstream_verification"};
    std::ofstream evidence(directory / "equivalence.json", std::ios::trunc);
    evidence << "[\n";
    bool first_row = true;
    for (const bool capture : {false, true}) {
        VerificationStorage storage;
        storage.Allocate();
        lut::Operators operators{1024};
        Ort::SessionOptions options;
        operators.Register(options);
        Ort::CUDAProviderOptions cuda;
        cuda.Update(std::unordered_map<std::string, std::string>{
            {"device_id", "0"}, {"enable_cuda_graph", capture ? "1" : "0"}, {"use_tf32", "0"},
        });
        cuda.UpdateWithValue("user_compute_stream", storage.Stream());
        options.AppendExecutionProvider_CUDA_V2(*cuda);
        {
            Ort::Session session{environment, path.c_str(), options};
            Ort::MemoryInfo memory{"Cuda", OrtArenaAllocator, 0, OrtMemTypeDefault};
            std::ifstream vectors(directory / "vectors.txt");
            if (!vectors) throw std::runtime_error("missing independent upstream vector manifest");
            std::string label;
            std::int64_t height = 0, width = 0;
            std::size_t cases = 0;
            while (vectors >> label >> height >> width) {
                if (height < 1 || width < 1 || height > 256 || width > 256 ||
                    label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
                    throw std::invalid_argument("invalid upstream vector manifest");
                const auto input_count = static_cast<std::size_t>(height * width * 3);
                const auto input = read_vector(directory / (label + ".input.f32"), input_count);
                const auto expected = read_vector(directory / (label + ".expected.f32"), input_count * 16);
                cuda_checked(cudaMemcpyAsync(storage.Input(), input.data(), input_count * sizeof(float),
                    cudaMemcpyHostToDevice, storage.Stream()), "copy changed verification pixels");
                const std::array<std::int64_t, 4> input_shape{1, 3, height, width}, output_shape{1, 3, height * 4, width * 4};
                auto input_value = Ort::Value::CreateTensor<float>(memory, storage.Input(), input_count,
                    input_shape.data(), input_shape.size());
                auto output_value = Ort::Value::CreateTensor<float>(memory, storage.Output(), input_count * 16,
                    output_shape.data(), output_shape.size());
                Ort::IoBinding binding{session};
                binding.BindInput("image", input_value);
                binding.BindOutput("upscaled", output_value);
                Ort::RunOptions run;
                const auto graph_id = std::to_string(height * 257 + width);
                run.AddConfigEntry("gpu_graph_id", graph_id.c_str());
                const auto before = lut::allocation_count();
                for (int replay = 0; replay < 3; ++replay) session.Run(run, binding);
                cuda_checked(cudaStreamSynchronize(storage.Stream()), "settle verification replay");
                const auto after = lut::allocation_count();
                if (before != after) throw std::runtime_error("ShiftLUT operator allocated during execution");
                std::vector<float> actual(expected.size());
                cuda_checked(cudaMemcpy(actual.data(), storage.Output(), actual.size() * sizeof(float),
                    cudaMemcpyDeviceToHost), "read verification output");
                if (actual != expected) {
                    std::ofstream mismatch(directory / (label + (capture ? ".graph.actual.f32" : ".fallback.actual.f32")),
                                           std::ios::binary);
                    mismatch.write(reinterpret_cast<const char*>(actual.data()),
                                   static_cast<std::streamsize>(actual.size() * sizeof(float)));
                    throw std::runtime_error("independent upstream ShiftLUT mismatch: " + label);
                }
                const bool probe_decisions = height * width <= 1024;
                if (probe_decisions) {
                    const auto count = static_cast<std::size_t>(16 * 12 * 16 * height * width);
                    std::vector<std::int8_t> decisions(count), expected_decisions(count);
                    const auto decision_path = directory / (label + ".decisions.i8");
                    if (std::filesystem::file_size(decision_path) != count)
                        throw std::invalid_argument("invalid upstream decision oracle size");
                    std::ifstream decision_file(decision_path, std::ios::binary);
                    if (!decision_file.read(reinterpret_cast<char*>(expected_decisions.data()), static_cast<std::streamsize>(count)))
                        throw std::runtime_error("read upstream lookup decisions");
                    cuda_checked(operators.ReadDecisions(decisions), "read captured ShiftLUT lookup inputs");
                    if (decisions != expected_decisions) {
                        std::ofstream mismatch(directory / (label + (capture ? ".graph.decisions.i8" : ".fallback.decisions.i8")),
                                               std::ios::binary);
                        mismatch.write(reinterpret_cast<const char*>(decisions.data()), static_cast<std::streamsize>(decisions.size()));
                        throw std::runtime_error("independent upstream lookup decision mismatch: " + label);
                    }
                }
                if (!first_row) evidence << ",\n";
                first_row = false;
                evidence << "  {\"case\":\"" << label << "\",\"graph\":" << (capture ? "true" : "false")
                         << ",\"identical\":true,\"lookup_inputs_identical\":" << (probe_decisions ? "true" : "null")
                         << ",\"hot_allocations\":" << after - before << '}';
                ++cases;
            }
            if (!vectors.eof() || cases == 0) throw std::runtime_error("incomplete upstream vector manifest");
        }
        cuda_checked(operators.Release(), "release verification tables and stages");
    }
    evidence << "\n]\n";
    evidence.close();
    if (!evidence) throw std::runtime_error("write ShiftLUT verification evidence");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string_view(argv[1]) == "--verify") {
            verify(argv[2], argv[3], argv[4], argv[5]);
            return 0;
        }
        if (argc != 5) throw std::invalid_argument("usage: mmltk_shiftlut_model_generator TABLES OUTPUT LUT_SHA256 SOURCE_SHA256");
        generate(argv[1], argv[2], argv[3], argv[4]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
