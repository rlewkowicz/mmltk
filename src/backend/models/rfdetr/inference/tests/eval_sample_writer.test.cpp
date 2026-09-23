#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
// RF-DETR inference sample-output coverage.
#include <cuda_runtime.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include "src/backend/models/rfdetr/core/sample_output.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "stb_image.h"
#include <stb_image_write.h>
#include "src/test_support/async_test_utils.hpp"
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/backend/ml/cuda/torch_cuda_utils.h"
// CLEANUP-IGNORE: The evaluation writer's direct CUDA/render boundary is independent of the training fixture boundary.
import mmltk.backend.ml.cuda.gpu_quiescence;
namespace cuda_api = mmltk::backend::ml::cuda;
namespace fs = std::filesystem;
namespace {
int require_cuda_devices() {
 int device_count = 0;
 const cudaError_t status = ::cudaGetDeviceCount(&device_count);
 if (status != cudaSuccess || device_count <= 0) { throw std::runtime_error("test_rfdetr_eval_sample_writer requires at least one CUDA device"); }
 return device_count;
}
void require_eval_sample_output(mmltk::backend::models::rfdetr::EvaluationSampleWriter& writer, const int device) {
 cuda_api::TorchCudaDeviceGuard guard(cuda_api::checked_device_index(device));
 const mmltk::testsupport::ScopedTempDir temp_dir("mmltk_eval_sample_writer");
 const fs::path output_path = temp_dir.path() / "sample.png";
 auto image = torch::full({3, 16, 16}, 0.35f, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device));
 auto boxes = torch::tensor({{2.0f, 2.0f, 13.0f, 13.0f}}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device));
 auto labels = torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, device));
 auto masks = torch::zeros({1, 16, 16}, torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, device));
 masks[0].slice(0, 4, 12).slice(1, 4, 12).fill_(true);
 mmltk::backend::models::rfdetr::RenderSampleOptions options;
 options.output_path = output_path;
 options.num_classes = 4;
 writer.Draw(image, boxes, labels, masks, options);
 writer.Flush();
 const bool output_exists = fs::exists(output_path);
 REQUIRE((output_exists));
 const auto output_size = fs::file_size(output_path);
 REQUIRE((output_size > 0));
 int width = 0;
 int height = 0;
 int channels = 0;
 stbi_uc* decoded = stbi_load(output_path.c_str(), &width, &height, &channels, 3);
 if (decoded == nullptr) { throw std::runtime_error("stbi_load failed for eval sample output"); }
 const int center_offset = ((height / 2) * width + (width / 2)) * 3;
 const int center_sum = static_cast<int>(decoded[center_offset]) + static_cast<int>(decoded[center_offset + 1]) + static_cast<int>(decoded[center_offset + 2]);
 stbi_image_free(decoded);
 REQUIRE((width == 16));
 REQUIRE((height == 16));
 REQUIRE((center_sum > 100));
}
}  // namespace
TEST_CASE("test_eval_sample_writer_flushes_output", "[model][rfdetr][eval_sample_writer]") {
 const int device_count = require_cuda_devices();
 for (int device = 0; device < device_count; ++device) {
  CAPTURE(device);
  mmltk::backend::models::rfdetr::EvaluationSampleWriter writer;
  require_eval_sample_output(writer, device);
  require_eval_sample_output(writer, device);
 }
}

TEST_CASE("rendered image writer atomically publishes owned pixels and preserves completed files on failure", "[model][rfdetr][eval_sample_writer]") {
 namespace gpu = mmltk::frameworks::gpu;
 const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0,gpu::cuda_image_copy_backend(),gpu::DeviceContextMode::Isolated,execution.placement.numa_node,execution);
 gpu::SystemImageRuntime runtime({.device=0,.numa_node=execution.placement.numa_node,.execution=execution,.adopted_context=context});
 mmltk::backend::models::rfdetr::RenderedImageWriter writer(context);
 const mmltk::testsupport::ScopedTempDir directory("rendered-image-writer");
 const auto fill = [&](int value) {
  auto candidate=runtime.AcquireOutput();
  runtime.PublishRetained(candidate,7U,5U,[&](auto clean,auto,auto stream) {
   REQUIRE(cudaMemset2DAsync(reinterpret_cast<void*>(clean.data),clean.descriptor.pitch_bytes,value,clean.descriptor.row_bytes(),clean.descriptor.height,reinterpret_cast<cudaStream_t>(stream))==cudaSuccess);
  });
  static_cast<void>(runtime.CommitOutput(std::move(candidate)));
 };
 fill(73);
 const auto destination=directory.path()/"samples"/"sample-7.png";
 writer.Write(runtime.Borrow(),destination);
 fill(201); // Source is reusable before the asynchronous file worker finishes.
 REQUIRE(writer.Flush()==destination);
 int width=0,height=0,channels=0;
 std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(stbi_load(destination.c_str(),&width,&height,&channels,4),stbi_image_free);
 REQUIRE(pixels);
 CHECK(width==7);
 CHECK(height==5);
 for(int index=0;index<width*height*4;++index) CHECK(pixels.get()[index]==73U);
 const auto refused=directory.path()/"occupied.png";
 fs::create_directory(refused);
 writer.Write(runtime.Borrow(),refused);
 CHECK_THROWS(writer.Flush());
 CHECK(fs::is_regular_file(destination));
 CHECK(fs::is_directory(refused));
 CHECK_FALSE(fs::exists(refused.string()+".partial"));
 writer.Write(runtime.Borrow(),directory.path()/"retry.png");
 CHECK(writer.Flush()==directory.path()/"retry.png");
}

TEST_CASE("rendered image writer shutdown retains engaged encoder pixels through atomic publication", "[model][rfdetr][eval_sample_writer]") {
 namespace gpu=mmltk::frameworks::gpu;
 const auto execution=gpu::resolve_device_execution(0,mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0,gpu::cuda_image_copy_backend(),gpu::DeviceContextMode::Isolated,execution.placement.numa_node,execution);
 gpu::SystemImageRuntime runtime({.device=0,.numa_node=execution.placement.numa_node,.execution=execution,.adopted_context=context});
 mmltk::testsupport::TestGate encoding("rendered writer pending encode");
 auto writer=std::make_unique<mmltk::backend::models::rfdetr::RenderedImageWriter>(context,
  [gate=encoding.receipt()](const char* path,int width,int height,int channels,const void* pixels,int stride){gate.ArriveAndWait();return stbi_write_png(path,width,height,channels,pixels,stride);});
 mmltk::testsupport::ScopedTestCleanup release([&]{encoding.Release();});
 const mmltk::testsupport::ScopedTempDir directory("writer-shutdown-output");
 const auto path=directory.path()/"sample.png";
 auto candidate=runtime.AcquireOutput();
 runtime.PublishRetained(candidate,7U,5U,[](auto clean,auto,auto stream){
  REQUIRE(cudaMemset2DAsync(reinterpret_cast<void*>(clean.data),clean.descriptor.pitch_bytes,73,clean.descriptor.row_bytes(),clean.descriptor.height,reinterpret_cast<cudaStream_t>(stream))==cudaSuccess);
 });
 static_cast<void>(runtime.CommitOutput(std::move(candidate)));
 writer->Write(runtime.Borrow(),path);
 REQUIRE(encoding.WaitEntered(std::chrono::seconds(5)));
 std::promise<void> destroying;
 auto stopped=std::async(std::launch::async,[&]{destroying.set_value();writer.reset();});
 mmltk::testsupport::ScopedTestCleanup unblock([&]{encoding.Release();});
 mmltk::testsupport::await_test_promise(destroying,"writer destruction entered");
 CHECK(stopped.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
 CHECK_FALSE(fs::exists(path));
 encoding.Release();
 mmltk::testsupport::await_test_future(stopped,"writer destruction drained",std::chrono::seconds(5));
 CHECK_FALSE(fs::exists(path.string()+".partial"));
 int width=0,height=0,channels=0;
 std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(stbi_load(path.c_str(),&width,&height,&channels,4),stbi_image_free);
 REQUIRE(pixels);CHECK(width==7);CHECK(height==5);
 for(int index=0;index<width*height*4;++index) CHECK(pixels.get()[index]==73U);
}
