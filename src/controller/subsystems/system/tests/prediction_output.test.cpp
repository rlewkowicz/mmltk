#include "src/controller/subsystems/system/detail/prediction_output.h"
#include "src/controller/subsystems/system/tests/prediction_test_support.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/cuda_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <stb_image.h>
#include <stb_image_write.h>
#include <fstream>
#include <cmath>
#include <iterator>
#include <memory>
#include <stop_token>
#include <vector>
#include "src/backend/media/video/video_file_source.h"
#include "src/test_support/subprocess_test_utils.hpp"
namespace {
namespace gpu=mmltk::frameworks::gpu;
namespace rfdetr=mmltk::backend::models::rfdetr;
using namespace mmltk::controller;
using namespace mmltk::controller::test_support;
TEST_CASE("prediction samples capture native pixels independently of browser and inference population", "[controller][prediction][output][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 const auto execution=gpu::resolve_device_execution(0,mmltk::common::system::NumaTopology::Capture());
 mmltk::testsupport::ScopedTempDir directory("prediction-samples");
 const auto kind=GENERATE(rfdetr::PredictSourceKind::ImageFiles,rfdetr::PredictSourceKind::CompiledDataset,rfdetr::PredictSourceKind::VideoFile);
 const bool enabled=GENERATE(false,true);
 const bool empty=GENERATE(false,true);
 PredictionRunOutput options{.directory=directory.path(),.population=8};
 options.saving.single_enabled=enabled; options.saving.compiled_enabled=enabled; options.saving.video_enabled=enabled;
 options.saving.compiled_percent=25; options.saving.video_mode=contracts::PredictionVideoSaving::Samples; options.saving.video_samples=2;
 contracts::WorkflowOutputFacts facts;
 options.progress=[&](const auto& value) { facts=value; };
 detail::PredictionOutput output({execution},kind,options,{});
 auto catalog=std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(empty ? std::vector<std::string>{} : std::vector<std::string>{"café"});
 output.Begin({.class_catalog=catalog,.class_domain=mmltk::backend::data::catalog::ClassReferenceDomain::Foreground,.source_images=8});
 const auto total=kind==rfdetr::PredictSourceKind::ImageFiles ? 1U:8U;
 for (unsigned index=0;index<total;++index) {
  const bool selected=output.Wants(index);
  if (!selected) continue;
  rfdetr::Prediction detection;
  detection.class_reference=0; detection.score=0.75F; detection.bbox_xyxy={4,24,40,44};
  std::vector<float> pixels(64U*48U*3U,0.25F);
  const auto detections=empty ? std::vector<rfdetr::Prediction>{} : std::vector<rfdetr::Prediction>{detection};
  auto source=PredictionSource::Device(execution,{64,48},pixels,detections,catalog);
  auto raw=output.Capture({.dataset_index=index,.detections=detections},
   {.chw=source.pixels(),.width=64,.height=48,.device=0,.custody=source.custody()},source.annotations());
  REQUIRE(raw);
  REQUIRE(cudaMemset(const_cast<float*>(source.pixels()),0,pixels.size()*sizeof(float))==cudaSuccess);
 }
 output.Finish(true);
 CHECK(facts.directory==directory.path().string());
 CHECK(facts.completed_samples==(enabled ? (total==1 ? 1U:2U):0U));
 if (!enabled) return;
 int width=0,height=0,channels=0;
 std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> saved(stbi_load(facts.recent_sample.c_str(),&width,&height,&channels,4),stbi_image_free);
 REQUIRE(saved); CHECK(width==64); CHECK(height==48);
 CHECK(saved.get()[(47U*64U+63U)*4U]==64U);
 bool annotated=false;
 for (unsigned pixel=0;pixel<64U*48U;++pixel) annotated |= saved.get()[pixel*4U]!=64U || saved.get()[pixel*4U+1U]!=64U || saved.get()[pixel*4U+2U]!=64U;
 CHECK(annotated==!empty);
 unsigned pngs=0;
 for (const auto& item : std::filesystem::recursive_directory_iterator(directory.path())) {
  CHECK(item.path().extension()!=".json");
  pngs+=item.path().extension()==".png";
 }
 CHECK(pngs==facts.completed_samples);
}
TEST_CASE("video reservoir shortfall retains completed disk samples", "[controller][prediction][output][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 const auto execution=gpu::resolve_device_execution(0,mmltk::common::system::NumaTopology::Capture());
 mmltk::testsupport::ScopedTempDir directory("prediction-shortfall");
 PredictionRunOutput options{.directory=directory.path()};
 options.saving.video_mode=contracts::PredictionVideoSaving::Samples; options.saving.video_samples=100;
 contracts::WorkflowOutputFacts facts; options.progress=[&](const auto& value) { facts=value; };
 detail::PredictionOutput output({execution},rfdetr::PredictSourceKind::VideoFile,options,{});
 auto catalog=std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"frame"});
 output.Begin({.class_catalog=catalog,.class_domain=mmltk::backend::data::catalog::ClassReferenceDomain::Foreground});
 for (int index=0;index<3;++index) {
  REQUIRE(output.Wants(index));
  std::vector<std::uint8_t> pixels(16U*16U*3U,80U);
  auto source=PredictionSource::Decoded({16,16},pixels,catalog);
  REQUIRE(output.Capture({.dataset_index=index},{.width=16,.height=16,.device=0,.rgb8=source.rgb8(),.custody=source.custody()},source.annotations()));
 }
 CHECK_THROWS_WITH(output.Finish(true),"Requested 100 samples; the video contained 3 frames. Saved 3.");
 CHECK(facts.completed_samples==3);
 for (int index=0;index<3;++index) CHECK(std::filesystem::exists(directory.path()/"samples"/("frame-"+std::to_string(index)+".png")));
}
TEST_CASE("failed reservoir replacement preserves the published incumbent", "[controller][prediction][output][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 const auto execution=gpu::resolve_device_execution(0,mmltk::common::system::NumaTopology::Capture());
 mmltk::testsupport::ScopedTempDir directory("prediction-replacement-failure");
 PredictionRunOutput options{.directory=directory.path()};
 options.saving.video_mode=contracts::PredictionVideoSaving::Samples; options.saving.video_samples=1;
 contracts::WorkflowOutputFacts facts; options.progress=[&](const auto& value) { facts=value; };
 unsigned writes=0;
 detail::PredictionOutput output({execution},rfdetr::PredictSourceKind::VideoFile,options,{},
  [&](const char* path,int width,int height,int channels,const void* pixels,int stride) {
   return ++writes==1U ? stbi_write_png(path,width,height,channels,pixels,stride):0;
  },0U);
 auto catalog=std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"frame"});
 output.Begin({.class_catalog=catalog,.class_domain=mmltk::backend::data::catalog::ClassReferenceDomain::Foreground});
 const auto capture=[&](int index) {
  std::vector<std::uint8_t> pixels(16U*16U*3U,80U);
  auto source=PredictionSource::Decoded({16,16},pixels,catalog);
  REQUIRE(output.Capture({.dataset_index=index},{.width=16,.height=16,.device=0,.rgb8=source.rgb8(),.custody=source.custody()},source.annotations()));
 };
 REQUIRE(output.Wants(0)); capture(0);
 int replacement=1;
 while (replacement<10000 && !output.Wants(replacement)) ++replacement;
 REQUIRE(replacement<10000);
 capture(replacement);
 CHECK_THROWS(output.Finish(false));
 CHECK(facts.completed_samples==1);
 CHECK(std::filesystem::exists(directory.path()/"samples"/"frame-0.png"));
 CHECK_FALSE(std::filesystem::exists(directory.path()/"samples"/("frame-"+std::to_string(replacement)+".png")));
}

TEST_CASE("compiled samples select actual processing order under an unchanged limit", "[controller][prediction][output][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 const auto execution=gpu::resolve_device_execution(0,mmltk::common::system::NumaTopology::Capture());
 mmltk::testsupport::ScopedTempDir directory("prediction-limited");
 PredictionRunOutput options{.directory=directory.path(),.population=100,.processing_population=10};
 contracts::WorkflowOutputFacts facts; options.progress=[&](const auto& value) { facts=value; };
 detail::PredictionOutput output({execution},rfdetr::PredictSourceKind::CompiledDataset,options,{}, {}, 13U);
 auto catalog=std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"frame"});
 output.Begin({.class_catalog=catalog,.class_domain=mmltk::backend::data::catalog::ClassReferenceDomain::Foreground,.source_images=100});
 const std::array<int,10> order{97,4,81,0,9,53,10,86,22,78};
 for (const auto index : order) {
  REQUIRE(output.Wants(index));
  std::vector<std::uint8_t> pixels(16U*16U*3U,80U);
  auto source=PredictionSource::Decoded({16,16},pixels,catalog);
  REQUIRE(output.Capture({.dataset_index=index},{.width=16,.height=16,.device=0,.rgb8=source.rgb8(),.custody=source.custody()},source.annotations()));
 }
 output.Finish(true);
 CHECK(facts.completed_samples==order.size());
 for (const auto index : order) CHECK(std::filesystem::exists(directory.path()/"samples"/("sample-"+std::to_string(index)+".png")));
}

void read_video_pixels(const float* source,std::vector<float>& destination,std::uintptr_t stream) {
 // Next publishes conversion on this nonblocking stream. Settle it before a
 // synchronous host read; both callers retain and reuse their bounded vectors.
 REQUIRE(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream))==cudaSuccess);
 REQUIRE(cudaMemcpy(destination.data(),source,destination.size()*sizeof(float),cudaMemcpyDeviceToHost)==cudaSuccess);
}
void prediction_video_fixture(const std::filesystem::path& directory) {
 std::vector<std::uint8_t> pixels(95U*63U*3U,128U);
 for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) for (unsigned channel=0;channel<3;++channel) pixels[(y*95U+x)*3U+channel]=0;
 REQUIRE(stbi_write_png((directory/"source.png").c_str(),95,63,3,pixels.data(),95*3)!=0);
 auto made=mmltk::testsupport::run_subprocess_capture_output({"ffmpeg","-v","error","-loop","1","-framerate","4","-i",(directory/"source.png").string(),"-frames:v","8","-c:v","png",(directory/"source.mov").string()});
 REQUIRE(made.exit_code==0);
 made=mmltk::testsupport::run_subprocess_capture_output({"ffmpeg","-v","error","-i",(directory/"source.mov").string(),"-c","copy","-metadata:s:v:0","rotate=90",(directory/"rotated.mov").string()});
 REQUIRE(made.exit_code==0);
}
TEST_CASE("prediction composition reaches decoded rotated annotated video without a browser", "[controller][prediction][output][video][gpu]") {
 enum class Settlement { Complete, Cancelled, Failed };
 const auto settlement=GENERATE(Settlement::Complete,Settlement::Cancelled,Settlement::Failed);
 const bool complete=settlement==Settlement::Complete;
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 namespace media=mmltk::backend::media::video;
 const auto execution=gpu::resolve_device_execution(0,mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0,gpu::cuda_image_copy_backend(),gpu::DeviceContextMode::Isolated,execution.placement.numa_node,execution);
 context.Bind(); gpu::ImageStream stream(context);
 mmltk::testsupport::ScopedTempDir temporary("prediction-video-composition");
 prediction_video_fixture(temporary.path());
 auto catalog=std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"café"});
 // Isolate each semantic layer as well as the complete saved composition.
 for (unsigned layer=0;layer<4;++layer) {
  const auto directory=temporary.path()/std::to_string(layer); std::filesystem::create_directory(directory);
  PredictionRunOutput options{.directory=directory};
  options.preview.boxes=layer==0 || layer==3; options.preview.masks=layer==1 || layer==3; options.preview.labels=layer==2 || layer==3;
  contracts::WorkflowOutputFacts facts; options.progress=[&](const auto& value) { facts=value; };
  context.Bind();
  media::VideoFileSource source(temporary.path()/"rotated.mov",{95U*63U},0,stream.native_handle(),{});
  const auto info=source.media_info(); REQUIRE(info); REQUIRE(info->width==63U); REQUIRE(info->height==95U);
  detail::PredictionOutput output({execution},rfdetr::PredictSourceKind::VideoFile,options,context);
  output.Media(*info);
  output.Begin({.class_catalog=catalog,.class_domain=mmltk::backend::data::catalog::ClassReferenceDomain::Foreground});
  std::vector<float> original(63U*95U*3U);
  std::size_t processed=0;
  while (auto frame=source.Next()) {
   if (settlement==Settlement::Failed && processed==4U) {
    CHECK_THROWS_WITH(output.Capture({.dataset_index=static_cast<std::int64_t>(frame->index)},
     {.preview_failure="injected source preparation failure"},{}),"prediction saving pixels unavailable: injected source preparation failure");
    break;
   }
   context.Bind();
   read_video_pixels(frame->chw,original,stream.native_handle());
   rfdetr::Prediction detection{.class_reference=0,.score=.75F,.bbox_xyxy={4,30,42,72}};
   std::vector<std::uint8_t> masks(63U*95U,0U);
   for (unsigned y=45;y<65;++y) for (unsigned x=14;x<34;++x) masks[y*63U+x]=1;
   auto annotated=PredictionSource::Device(execution,{63,95},original,{detection},catalog,masks);
   REQUIRE(output.Capture({.dataset_index=static_cast<std::int64_t>(frame->index),.detections={detection}},
    {.chw=annotated.pixels(),.width=63,.height=95,.device=0,.custody=annotated.custody(),.timing=frame->timing},annotated.annotations()));
   ++processed; context.Bind();
   if (settlement==Settlement::Cancelled && processed==4U) break;
  }
  output.Finish(complete); REQUIRE(processed==(complete ? 8U:4U));
  REQUIRE(facts.partial_video.empty()==complete); REQUIRE(facts.artifacts.size()==(complete ? 1U:0U));
  if (complete) CHECK(facts.artifacts.front()==directory/"prediction.mkv");
  else CHECK(facts.partial_video==(directory/"prediction.partial.mkv").string());
  CHECK_FALSE(std::filesystem::exists(directory/"predictions.json"));
  context.Bind();
  media::VideoFileSource decoded(directory/(complete ? "prediction.mkv":"prediction.partial.mkv"),{64U*96U},0,stream.native_handle(),{});
  std::vector<float> pixels(64U*96U*3U); std::size_t frames=0;
  while (auto frame=decoded.Next()) {
   REQUIRE(frame->width==64U); REQUIRE(frame->height==96U);
   REQUIRE(frame->timing.pts);
   CHECK(std::abs(double(*frame->timing.pts)*frame->timing.time_base_numerator/frame->timing.time_base_denominator-double(frames)/4.0)<.001);
   read_video_pixels(frame->chw,pixels,stream.native_handle());
   const auto changed=[&](unsigned x,unsigned y) {
    float difference=0;
    for (unsigned channel=0;channel<3;++channel) difference=std::max(difference,std::abs(pixels[channel*64U*96U+y*64U+x]-original[channel*63U*95U+y*63U+x]));
    return difference;
   };
   CHECK((changed(4,40)>.12F)==options.preview.boxes);
   CHECK((changed(24,55)>.08F)==options.preview.masks);
   bool caption=false;
   for (unsigned y=10;y<28;++y) for (unsigned x=6;x<59;++x) caption |= changed(x,y)>.18F;
   CHECK(caption==options.preview.labels);
   // The rotated source remains aligned; the one-pixel edge padding replicates.
   CHECK(changed(56,80)<.08F);
   for (unsigned channel=0;channel<3;++channel)
    CHECK(std::abs(pixels[channel*64U*96U+95U*64U+63U]-original[channel*63U*95U+94U*63U+62U])<.08F);
   ++frames;
  }
  CHECK(frames==processed);
 }
}
TEST_CASE("video samples use actual decoded population despite AVI count metadata", "[controller][prediction][output][video][gpu]") {
 if (!mmltk::testsupport::checked_cuda_device_count()) SKIP("no CUDA device available");
 namespace media=mmltk::backend::media::video;
 const auto execution=gpu::resolve_device_execution(0,mmltk::common::system::NumaTopology::Capture());
 gpu::DeviceContext context(0,gpu::cuda_image_copy_backend(),gpu::DeviceContextMode::Isolated,execution.placement.numa_node,execution);
 context.Bind(); gpu::ImageStream stream(context);
 mmltk::testsupport::ScopedTempDir temporary("prediction-video-count");
 prediction_video_fixture(temporary.path());
 const auto made=mmltk::testsupport::run_subprocess_capture_output({"ffmpeg","-v","error","-i",(temporary.path()/"source.mov").string(),"-c:v","ffv1",(temporary.path()/"source.avi").string()});
 REQUIRE(made.exit_code==0);
 std::ifstream input(temporary.path()/"source.avi",std::ios::binary);
 const std::string original{std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
 for (const unsigned declared : {0U,1U,99U}) {
  const auto directory=temporary.path()/std::to_string(declared); std::filesystem::create_directory(directory);
  auto bytes=original;
  for (const auto [tag,offset] : {std::pair{"avih",16U},std::pair{"strh",32U}}) {
   const auto position=bytes.find(tag); REQUIRE(position!=std::string::npos); REQUIRE(position+8U+offset+4U<=bytes.size());
   for (unsigned byte=0;byte<4;++byte) bytes[position+8U+offset+byte]=static_cast<char>((declared>>(byte*8U))&255U);
  }
  const auto path=directory/"source.avi";
  { std::ofstream file(path,std::ios::binary); file.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); REQUIRE(file.good()); }
  context.Bind();
  auto source=std::make_shared<media::VideoFileSource>(path,media::VideoFrameCapacity{95U*63U},0,stream.native_handle(),std::stop_token{});
  CHECK(source->frame_count()!=8U);
  PredictionRunOutput options{.directory=directory}; options.saving.video_mode=contracts::PredictionVideoSaving::Samples; options.saving.video_samples=2;
  contracts::WorkflowOutputFacts facts; options.progress=[&](const auto& value) { facts=value; };
  detail::PredictionOutput output({execution},rfdetr::PredictSourceKind::VideoFile,options,context,{},91U);
  auto catalog=std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"frame"});
  output.Begin({.class_catalog=catalog,.class_domain=mmltk::backend::data::catalog::ClassReferenceDomain::Foreground});
  std::size_t processed=0;
  while (auto frame=source->Next()) {
   if (output.Wants(frame->index)) REQUIRE(output.Capture({.dataset_index=static_cast<std::int64_t>(frame->index)},
    {.chw=frame->chw,.width=frame->width,.height=frame->height,.device=0,.stream=stream.native_handle(),.custody=source,.timing=frame->timing},{}));
   ++processed;
  }
  output.Finish(true); CHECK(processed==8U); CHECK(facts.completed_samples==2U);
  unsigned saved=0;
  for (const auto& entry : std::filesystem::directory_iterator(directory/"samples")) { CHECK(entry.path().filename().string().starts_with("frame-")); ++saved; }
  CHECK(saved==2U);
 }
}

}
