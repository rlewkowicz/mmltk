#include "video_file_source.h"
#include "video_frame_convert.h"
#include "src/common/system/numa_memory.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include <cuda_runtime_api.h>
#include <cuda.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <spdlog/spdlog.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
import mmltk.common.logging.mmltk_logging;
namespace mmltk::backend::media::video {
namespace {
void video_log(void*, int level, const char* format, va_list arguments) {
 // FFmpeg's optional AV_LOG_C decoration is outside its low severity byte.
 const auto message_level = level >= 0 ? level & 0xff : level;
 // av_vlog dispatches custom callbacks regardless of av_log_level.
 if (message_level > av_log_get_level()) return;
 const auto severity = message_level <= AV_LOG_FATAL      ? spdlog::level::critical
                       : message_level <= AV_LOG_ERROR    ? spdlog::level::err
                        : message_level <= AV_LOG_WARNING ? spdlog::level::warn
                         : message_level <= AV_LOG_INFO   ? spdlog::level::info
                          : message_level <= AV_LOG_DEBUG ? spdlog::level::debug
                                                          : spdlog::level::trace;
 try {
  mmltk::common::logging::log_if_enabled("video", severity, [&](auto& logger) {
   char text[2048]{};
   va_list copy;
   va_copy(copy, arguments);
   std::vsnprintf(text, sizeof(text), format, copy);
   va_end(copy);
   logger.log(severity, "{}", text);
  });
 } catch (...) {}
}
void check(int result, const char* operation) {
 if (result < 0) {
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, text, sizeof(text));
  throw std::runtime_error(std::string(operation) + ": " + text);
 }
}
void cuda_check(cudaError_t result) {
 if (result != cudaSuccess) throw std::runtime_error(cudaGetErrorString(result));
}
}  // namespace
void VideoFileSource::ConfigureLogging(LogCallback callback) {
 const auto selected = mmltk::common::logging::level();
 int admission = AV_LOG_QUIET;
 switch (selected) {
  case spdlog::level::trace: admission = AV_LOG_TRACE; break;
  case spdlog::level::debug: admission = AV_LOG_DEBUG; break;
  case spdlog::level::info: admission = AV_LOG_INFO; break;
  case spdlog::level::warn: admission = AV_LOG_WARNING; break;
  case spdlog::level::err: admission = AV_LOG_ERROR; break;
  case spdlog::level::critical: admission = AV_LOG_FATAL; break;
  default: break;
 }
 av_log_set_level(admission);
 av_log_set_callback(admission == AV_LOG_QUIET ? nullptr : callback ? callback : &video_log);
}
VideoFileSource::StorageLimits::StorageLimits(VideoFrameCapacity requested) : maximum_pixels(requested.maximum_pixels) {
 constexpr auto maximum = std::numeric_limits<std::size_t>::max();
 if (maximum_pixels == 0U || maximum_pixels > maximum / (3U * sizeof(float)) ||
     maximum_pixels > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
  throw std::invalid_argument("video frame capacity exceeds supported storage");
 chw_bytes = maximum_pixels * 3U * sizeof(float);
 rgb_bytes = maximum_pixels * 3U;
 const auto page = mmltk::common::system::host_page_size();
 if (rgb_bytes > maximum - (page - 1U)) throw std::invalid_argument("video frame capacity exceeds page-rounded storage");
 pinned_bytes = mmltk::common::system::page_rounded_bytes(rgb_bytes);
 // Each high-water owner permits at most an incumbent and one replacement.
 // Device RGB uses the page-rounded extent of its pinned transfer allocation.
 if (pinned_bytes > maximum / 4U || chw_bytes > (maximum - 4U * pinned_bytes) / 2U)
  throw std::invalid_argument("video frame capacity exceeds replacement storage");
 owned_bytes = 2U * chw_bytes + 4U * pinned_bytes;
}
std::size_t VideoFileSource::StorageLimits::Pixels(int width, int height) const {
 if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 3 ||
     static_cast<std::size_t>(width) > maximum_pixels / static_cast<std::size_t>(height))
  throw std::invalid_argument("video frame dimensions exceed supported capacity");
 return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}
void VideoFileSource::StorageLimits::Declared(int width, int height) const {
 // Zero means not yet discovered. Known axes still impose a lower bound.
 static_cast<void>(Pixels(width == 0 ? 1 : width, height == 0 ? 1 : height));
}
struct VideoFileSource::State final {
 explicit State(VideoFrameCapacity requested_capacity) : limits(requested_capacity) {}
 const StorageLimits limits;
 AVFormatContext* format = nullptr;
 AVCodecContext* codec = nullptr;
 AVFrame* frame = av_frame_alloc();
 AVPacket* packet = av_packet_alloc();
 SwsContext* scaler = nullptr;
 std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> pinned;
 std::stop_token stop;
 cudaStream_t stream = nullptr;
 CUcontext context = nullptr;
 cudaEvent_t decoder_ready = nullptr;
 cudaEvent_t source_read = nullptr;
 bool source_read_pending = false;
 mmltk::frameworks::gpu::CudaHighWaterAllocation<std::uint8_t*> rgb;
 mmltk::frameworks::gpu::CudaHighWaterAllocation<float*> chw;
 std::size_t capacity = 0U;
 std::size_t rgb_capacity = 0U;
 int video_stream = -1;
 double fps = 0.0;
 std::uint64_t total = 0U;
 std::uint64_t index = 0U;
 bool draining = false;
 bool hardware_attempt = false;
 bool failed = false;
 std::filesystem::path path;
 int device_id = 0;
 void Open(bool hardware);
 unsigned Rotation() const {
  const auto* parameters = format->streams[video_stream]->codecpar;
  const auto* stream_side = av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  std::size_t size = stream_side ? stream_side->size : 0U;
  const auto* matrix = stream_side ? stream_side->data : nullptr;
  if (const auto* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
   matrix = side->data;
   size = side->size;
  }
  if (!matrix) return 0U;
  if (size < 9U * sizeof(std::int32_t)) throw std::runtime_error("invalid video display matrix");
  const auto* transform = reinterpret_cast<const std::int32_t*>(matrix);
  const auto determinant = static_cast<double>(transform[0]) * transform[4] - static_cast<double>(transform[1]) * transform[3];
  if (determinant <= 0.0) throw std::runtime_error("unsupported reflected video display matrix");
  const auto degrees = -av_display_rotation_get(transform);
  if (!std::isfinite(degrees)) throw std::runtime_error("invalid video display rotation");
  const auto quarter = std::lround(degrees / 90.0);
  if (std::abs(degrees - static_cast<double>(quarter) * 90.0) > 0.01) throw std::runtime_error("unsupported video display rotation");
  return static_cast<unsigned>((quarter % 4 + 4) % 4);
 }
 void ReleaseDecoder() noexcept {
  sws_freeContext(scaler);
  scaler = nullptr;
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
 }
 ~State() { ReleaseDecoder(); }
 void Reserve(std::size_t pixels) {
  if (pixels <= capacity) return;
  cuda_check(cudaStreamSynchronize(stream));
  const auto release = [](float* address) noexcept { return cudaFree(address); };
  cuda_check(chw.RetryPending(release).failure);
  cuda_check(
   chw.AllocateCandidate([pixels](float*& address) noexcept { return cudaMalloc(reinterpret_cast<void**>(&address), pixels * 3U * sizeof(float)); }).failure);
  cuda_check(chw.PromoteCandidate(release).failure);
  capacity = pixels;
 }
 void ValidateFrameLayout() const {
  auto pixel_format = static_cast<AVPixelFormat>(frame->format);
  if (pixel_format == AV_PIX_FMT_CUDA) {
   if (!frame->hw_frames_ctx || !frame->hw_frames_ctx->data) throw std::runtime_error("CUDA video frame lacks device custody");
   pixel_format = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data)->sw_format;
  }
  int rows[4]{};
  check(av_image_fill_linesizes(rows, pixel_format, frame->width), "validate video row extents");
  for (unsigned plane = 0U; plane < 4U; ++plane) {
   const auto stride = static_cast<std::int64_t>(frame->linesize[plane]);
   if (rows[plane] > 0 && (!frame->data[plane] || std::abs(stride) < rows[plane] || (frame->format == AV_PIX_FMT_CUDA && stride < 0)))
    throw std::invalid_argument("video frame row stride is too small");
  }
 }
 VideoFrame Convert() {
  const auto pixels = limits.Pixels(frame->width, frame->height);
  const auto quarters = Rotation();
  if (quarters % 2 != 0) static_cast<void>(limits.Pixels(frame->height, frame->width));
  ValidateFrameLayout();
  if (frame->colorspace == AVCOL_SPC_BT2020_CL || frame->colorspace == AVCOL_SPC_YCGCO || frame->colorspace == AVCOL_SPC_SMPTE2085 ||
      frame->colorspace == AVCOL_SPC_CHROMA_DERIVED_CL || frame->colorspace == AVCOL_SPC_ICTCP)
   throw std::runtime_error("unsupported video color matrix");
  const auto width = static_cast<std::uint32_t>(frame->width);
  const auto height = static_cast<std::uint32_t>(frame->height);
  Reserve(pixels);
  VideoColorConversion conversion;
  conversion.clockwise_quarters = quarters;
  if (frame->format == AV_PIX_FMT_CUDA) {
   const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
   const auto* device = reinterpret_cast<const AVCUDADeviceContext*>(frames->device_ctx->hwctx);
   cuda_check(cudaEventRecord(decoder_ready, reinterpret_cast<cudaStream_t>(device->stream)));
   cuda_check(cudaStreamWaitEvent(stream, decoder_ready, 0U));
   const auto* description = av_pix_fmt_desc_get(frames->sw_format);
   if (!description || description->nb_components < 3 || (description->flags & (AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_BITSTREAM)))
    throw std::runtime_error("unsupported CUDA video pixel format");
   for (unsigned channel = 0U; channel < 3U; ++channel) {
    const auto& component = description->comp[channel];
    if (component.depth < 8 || component.depth > 16 || frame->linesize[component.plane] <= 0)
     throw std::runtime_error("unsupported CUDA video component layout");
    conversion.planes[channel] = {frame->data[component.plane],
                                  static_cast<std::size_t>(frame->linesize[component.plane]),
                                  component.step,
                                  component.offset,
                                  component.shift,
                                  component.depth};
   }
   conversion.rgb = (description->flags & AV_PIX_FMT_FLAG_RGB) != 0;
   conversion.chroma_x = conversion.rgb ? 0 : description->log2_chroma_w;
   conversion.chroma_y = conversion.rgb ? 0 : description->log2_chroma_h;
   conversion.full_range = frame->color_range == AVCOL_RANGE_JPEG;
   switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
     conversion.kr = 0.2126F;
     conversion.kb = 0.0722F;
     break;
    case AVCOL_SPC_BT2020_NCL:
     conversion.kr = 0.2627F;
     conversion.kb = 0.0593F;
     break;
    case AVCOL_SPC_FCC:
     conversion.kr = 0.30F;
     conversion.kb = 0.11F;
     break;
    case AVCOL_SPC_SMPTE240M:
     conversion.kr = 0.212F;
     conversion.kb = 0.087F;
     break;
    default: break;
   }
  } else {
   const auto bytes = pixels * 3U;
   if (!pinned) pinned = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
   if (pinned->capacity_bytes() < bytes) pinned->ensure_bytes(bytes);
   if (rgb_capacity < bytes) {
    const auto release = [](std::uint8_t* address) noexcept { return cudaFree(address); };
    cuda_check(rgb.RetryPending(release).failure);
    cuda_check(
     rgb.AllocateCandidate([this](std::uint8_t*& address) noexcept { return cudaMalloc(reinterpret_cast<void**>(&address), pinned->capacity_bytes()); })
      .failure);
    cuda_check(rgb.PromoteCandidate(release).failure);
    rgb_capacity = pinned->capacity_bytes();
   }
   scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
   if (!scaler) throw std::runtime_error("video color converter unavailable");
   const auto color_space = frame->colorspace == AVCOL_SPC_UNSPECIFIED ? SWS_CS_DEFAULT : frame->colorspace;
   const auto* coefficients = sws_getCoefficients(color_space);
   check(sws_setColorspaceDetails(scaler, coefficients, frame->color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0, 1 << 16, 1 << 16),
         "video color conversion");
   std::uint8_t* destination[]{static_cast<std::uint8_t*>(pinned->data()), nullptr, nullptr, nullptr};
   int strides[]{frame->width * 3, 0, 0, 0};
   if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination, strides) != frame->height)
    throw std::runtime_error("video conversion returned incomplete pixels");
   cuda_check(cudaMemcpyAsync(rgb.active(), pinned->data(), bytes, cudaMemcpyHostToDevice, stream));
   conversion.rgb = true;
   conversion.chroma_x = 0;
   conversion.chroma_y = 0;
   for (int channel = 0; channel < 3; ++channel) conversion.planes[channel] = {rgb.active(), width * 3U, 3, channel, 0, 8};
  }
  cuda_check(static_cast<cudaError_t>(convert_video_chw(conversion, width, height, chw.active(), stream)));
  cuda_check(cudaEventRecord(source_read, stream));
  source_read_pending = true;
  const auto* track = format->streams[video_stream];
  if ((track->avg_frame_rate.num <= 0 || track->avg_frame_rate.den <= 0) && codec->framerate.num > 0 && codec->framerate.den > 0)
   fps = av_q2d(codec->framerate);
  VideoFrame result{chw.active(), conversion.clockwise_quarters % 2U ? height : width, conversion.clockwise_quarters % 2U ? width : height, index++, {}};
  if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
   const auto seconds = static_cast<double>(frame->best_effort_timestamp) * av_q2d(format->streams[video_stream]->time_base);
   if (std::isfinite(seconds)) result.presentation_seconds = seconds;
  }
  return result;
 }
};
struct VideoFileSource::Owner final {
 Owner(VideoFrameCapacity capacity, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> authority, decltype(&cudaStreamSynchronize) settlement,
       mmltk::frameworks::gpu::CudaContextApi api)
     : retirement(authority ? std::move(authority) : std::make_shared<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>(1U)),
       settle(settlement),
       context_api(api),
       state(std::make_shared<State>(capacity)) {
  if (!settle || !context_api.get || !context_api.set) throw std::invalid_argument("video settlement operation is unavailable");
 }
 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement;
 decltype(&cudaStreamSynchronize) settle;
 mmltk::frameworks::gpu::CudaContextApi context_api;
 bool terminal = false;
 void Retain() noexcept {
  if (std::exchange(terminal, true)) return;
  auto retained = state;
  std::move(lease).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(retained)), cudaErrorUnknown);
 }
 template <class Operation>
 decltype(auto) Run(Operation&& operation) {
  if (terminal) throw mmltk::frameworks::gpu::CudaContextFailure(true);
  mmltk::frameworks::gpu::CudaContextScope scope({this, [](void* owner) noexcept { static_cast<Owner*>(owner)->Retain(); }}, context_api);
  return scope.Run([&]() -> decltype(auto) {
   scope.Select(state->context);
   return std::forward<Operation>(operation)();
  });
 }
 mmltk::frameworks::gpu::TerminalCudaRetirementLease lease = Reserve();
 std::shared_ptr<State> state;
 mmltk::frameworks::gpu::TerminalCudaRetirementLease Reserve() {
  auto reserved = retirement->Reserve();
  if (!reserved) throw std::runtime_error("video retirement admission failed");
  return std::move(*reserved);
 }
 ~Owner() {
  if (terminal) return;
  // Invalid-input construction never acquired GPU state.
  if (!state->context) return;
  try {
   Run([&] {
    if (state->stream) cuda_check(settle(state->stream));
    cuda_check(state->chw.ReleaseAll([](float* address) noexcept { return cudaFree(address); }).failure);
    cuda_check(state->rgb.ReleaseAll([](std::uint8_t* address) noexcept { return cudaFree(address); }).failure);
    if (state->decoder_ready) {
     cuda_check(cudaEventDestroy(state->decoder_ready));
     state->decoder_ready = nullptr;
    }
    if (state->source_read) {
     cuda_check(cudaEventDestroy(state->source_read));
     state->source_read = nullptr;
    }
    if (state->pinned && state->pinned->ReleaseSettled() != CUDA_SUCCESS) throw std::runtime_error("video pinned release failed");
    state->pinned.reset();
    state->ReleaseDecoder();
   });
  } catch (const mmltk::frameworks::gpu::CudaContextFailure& error) {
   if (error.terminal()) Retain();
  } catch (...) { Retain(); }
 }
};
VideoFileSource::VideoFileSource(const std::filesystem::path& path, VideoFrameCapacity capacity, int device, std::uintptr_t stream, std::stop_token stop,
                                 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement, decltype(&cudaStreamSynchronize) settle)
    : VideoFileSource(path, capacity, device, stream, stop, std::move(retirement), settle, mmltk::frameworks::gpu::CudaContextApi{}) {}
VideoFileSource::VideoFileSource(const std::filesystem::path& path, VideoFrameCapacity capacity, int device, std::uintptr_t stream, std::stop_token stop,
                                 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement, decltype(&cudaStreamSynchronize) settle,
                                 mmltk::frameworks::gpu::CudaContextApi context_api)
    : owner_(std::make_unique<Owner>(capacity, std::move(retirement), settle, context_api)) {
 auto& state = *owner_->state;
 std::error_code path_error;
 if (!std::filesystem::is_regular_file(path, path_error)) throw std::invalid_argument("prediction video must be a local regular file");
 if (!stream || !state.frame || !state.packet) throw std::runtime_error("video decoder allocation failed");
 state.stop = stop;
 state.stream = reinterpret_cast<cudaStream_t>(stream);
 if (owner_->context_api.get(owner_->context_api.context, &state.context) != CUDA_SUCCESS) {
  owner_->Retain();
  throw mmltk::frameworks::gpu::CudaContextFailure(true);
 }
 if (!state.context) throw std::runtime_error("video decoder requires an active CUDA context");
 // Production selects logging at startup. Publish both non-atomic FFmpeg
 // globals once before any source opens a codec; subsequent sources only
 // synchronize with that initialization and never rewrite active state.
 static std::once_flag logging_configured;
 std::call_once(logging_configured, [] { ConfigureLogging(); });
 state.path = path;
 state.device_id = device;
 try {
  owner_->Run([&] { state.Open(true); });
 } catch (const mmltk::frameworks::gpu::CudaContextFailure&) { throw; } catch (...) {
  if (stop.stop_requested()) return;
  owner_->Run([&] { state.Open(false); });
 }
 owner_->Run([&] {
  cuda_check(cudaEventCreateWithFlags(&state.decoder_ready, cudaEventDisableTiming));
  cuda_check(cudaEventCreateWithFlags(&state.source_read, cudaEventDisableTiming));
 });
}
void VideoFileSource::State::Open(bool hardware) {
 if (source_read_pending) cuda_check(cudaEventSynchronize(source_read));
 source_read_pending = false;
 av_frame_unref(frame);
 av_packet_unref(packet);
 avcodec_free_context(&codec);
 avformat_close_input(&format);
 draining = false;
 format = avformat_alloc_context();
 if (!format) throw std::bad_alloc();
 format->interrupt_callback = {[](void* opaque) { return static_cast<State*>(opaque)->stop.stop_requested() ? 1 : 0; }, this};
 AVDictionary* options = nullptr;
 av_dict_set(&options, "protocol_whitelist", "file", 0);
 const auto opened = avformat_open_input(&format, path.c_str(), nullptr, &options);
 av_dict_free(&options);
 if (stop.stop_requested()) return;
 check(opened, "open prediction video");
 const auto log_tracks = [&](const char* phase) {
  mmltk::common::logging::log_if_enabled("video", spdlog::level::debug, [&](auto& logger) {
   for (unsigned stream_index = 0; stream_index < format->nb_streams; ++stream_index) {
    const auto* track = format->streams[stream_index];
    const auto* parameters = track->codecpar;
    const auto* matrix = av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    const bool has_matrix = matrix && matrix->size == 9U * sizeof(std::int32_t);
    logger.debug("video source phase={} hardware={} stream={} codec={} width={} height={} frames={} side_data={} display_matrix={} rotation={}", phase,
                 hardware, stream_index, avcodec_get_name(parameters->codec_id), parameters->width, parameters->height, track->nb_frames,
                 parameters->nb_coded_side_data, has_matrix, has_matrix ? av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix->data)) : 0.0);
   }
  });
 };
 log_tracks("opened");
 struct ProbeOptions final {
  std::vector<AVDictionary*> streams;
  std::vector<AVPacket*> declarations;
  ~ProbeOptions() {
   for (auto*& options : streams) av_dict_free(&options);
   for (auto*& declaration : declarations) av_packet_free(&declaration);
  }
 } probe{std::vector<AVDictionary*>(format->nb_streams, nullptr), std::vector<AVPacket*>(format->nb_streams, nullptr)};
 for (unsigned stream_index = 0; stream_index < format->nb_streams; ++stream_index) {
  const auto* parameters = format->streams[stream_index]->codecpar;
  if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) limits.Declared(parameters->width, parameters->height);
  if (parameters->nb_coded_side_data > 0) {
   auto*& declaration = probe.declarations[stream_index];
   declaration = av_packet_alloc();
   if (!declaration) throw std::bad_alloc();
   AVPacket borrowed{};
   borrowed.side_data = parameters->coded_side_data;
   borrowed.side_data_elems = parameters->nb_coded_side_data;
   check(av_packet_copy_props(declaration, &borrowed), "retain prediction video metadata");
  }
  check(av_dict_set_int(&probe.streams[stream_index], "max_pixels", static_cast<std::int64_t>(limits.maximum_pixels), 0), "bound prediction probe decoder");
 }
 // New tracks discovered during find_stream_info have no per-stream option
 // slot. Disable its internal decoders for every track, retaining demuxing,
 // parsers, timing/extradata discovery, and the existing buffered packets.
 // Only the selected codec below may allocate decoded frames, under its limit.
 check(av_opt_set(format, "codec_whitelist", "", 0), "bound prediction stream discovery");
 const auto inspected = avformat_find_stream_info(format, probe.streams.data());
 log_tracks("inspected");
 if (stop.stop_requested()) return;
 check(inspected, "inspect prediction video");
 if (format->pb && format->pb->error < 0 && format->pb->error != AVERROR_EOF) check(format->pb->error, "read prediction video metadata");
 // Failed probe-codec admission may clear container side data before FFmpeg
 // republishes codec parameters. Restore missing declarations without replacing
 // discovered facts or enabling unbounded probe decoding.
 for (unsigned stream_index = 0; stream_index < probe.declarations.size(); ++stream_index) {
  auto* declaration = probe.declarations[stream_index];
  if (!declaration) continue;
  auto* parameters = format->streams[stream_index]->codecpar;
  for (int item = 0; item < declaration->side_data_elems; ++item) {
   auto& data = declaration->side_data[item];
   if (av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data, data.type)) continue;
   if (!av_packet_side_data_add(&parameters->coded_side_data, &parameters->nb_coded_side_data, data.type, data.data, data.size, 0)) throw std::bad_alloc();
   data.data = nullptr;
   data.size = 0U;
  }
 }
 log_tracks("ready");
 const AVCodec* decoder = nullptr;
 video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
 check(video_stream, "select prediction video stream");
 auto* track = format->streams[video_stream];
 limits.Declared(track->codecpar->width, track->codecpar->height);
 codec = avcodec_alloc_context3(decoder);
 if (!codec) throw std::bad_alloc();
 check(avcodec_parameters_to_context(codec, track->codecpar), "configure prediction decoder");
 codec->max_pixels = static_cast<std::int64_t>(limits.maximum_pixels);
 hardware_attempt = false;
 if (hardware)
  for (int config_index = 0; const auto* config = avcodec_get_hw_config(decoder, config_index); ++config_index) {
   if (config->device_type != AV_HWDEVICE_TYPE_CUDA || !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
   const auto name = std::to_string(device_id);
   if (av_hwdevice_ctx_create(&codec->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, name.c_str(), nullptr, AV_CUDA_USE_CURRENT_CONTEXT) >= 0) {
    hardware_attempt = true;
   } else {
    av_buffer_unref(&codec->hw_device_ctx);
   }
   break;
  }
 codec->get_format = [](AVCodecContext* decoder_context, const AVPixelFormat* formats) {
  if (decoder_context->hw_device_ctx)
   for (auto* candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate)
    if (*candidate == AV_PIX_FMT_CUDA) return *candidate;
  for (auto* candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
   const auto* descriptor = av_pix_fmt_desc_get(*candidate);
   if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) && sws_isSupportedInput(*candidate)) return *candidate;
  }
  return AV_PIX_FMT_NONE;
 };
 check(avcodec_open2(codec, decoder, nullptr), "open prediction decoder");
 fps = av_q2d(track->avg_frame_rate);
 if (!std::isfinite(fps) || fps <= 0.0) fps = av_q2d(track->r_frame_rate);
 if (!std::isfinite(fps) || fps <= 0.0) fps = 0.0;
 total = track->nb_frames > 0 ? static_cast<std::uint64_t>(track->nb_frames) : 0U;
}
VideoFileSource::~VideoFileSource() = default;
std::optional<VideoFrame> VideoFileSource::Next() {
 auto& state = *owner_->state;
 if (state.failed && !owner_->terminal) throw std::runtime_error("video source failed; create a new source to retry");
 bool entered = false;
 try {
  return owner_->Run([&]() -> std::optional<VideoFrame> {
   entered = true;
   // Only decoder inputs and pinned upload memory require host settlement.
   // CHW consumers and its next write remain ordered on the supplied stream.
   if (state.source_read_pending) cuda_check(cudaEventSynchronize(state.source_read));
   state.source_read_pending = false;
   av_frame_unref(state.frame);
   while (!state.stop.stop_requested()) {
    const auto received = owner_->Run([&] { return avcodec_receive_frame(state.codec, state.frame); });
    if (received == 0) {
     try {
      return owner_->Run([&] { return state.Convert(); });
     } catch (const mmltk::frameworks::gpu::CudaContextFailure&) { throw; } catch (...) {
      if (!state.hardware_attempt || state.index != 0U || state.stop.stop_requested()) throw;
      cuda_check(cudaStreamSynchronize(state.stream));
      owner_->Run([&] { state.Open(false); });
      continue;
     }
    }
    if (received < 0 && received != AVERROR(EAGAIN) && received != AVERROR_EOF && state.hardware_attempt && state.index == 0U) {
     owner_->Run([&] { state.Open(false); });
     continue;
    }
    if (received == AVERROR_EOF) return {};
    check(received == AVERROR(EAGAIN) ? 0 : received, "decode prediction video");
    if (state.draining) throw std::runtime_error("video decoder requested input after draining");
    int read;
    do {
     av_packet_unref(state.packet);
     read = av_read_frame(state.format, state.packet);
    } while (read >= 0 && state.packet->stream_index != state.video_stream && !state.stop.stop_requested());
    if (state.stop.stop_requested()) return {};
    if (read == AVERROR_EOF) {
     state.draining = true;
     check(owner_->Run([&] { return avcodec_send_packet(state.codec, nullptr); }), "drain prediction video");
    } else {
     check(read, "read prediction video");
     const auto submitted = owner_->Run([&] { return avcodec_send_packet(state.codec, state.packet); });
     if (submitted < 0 && state.hardware_attempt && state.index == 0U) {
      owner_->Run([&] { state.Open(false); });
      continue;
     }
     check(submitted, "submit prediction video packet");
    }
   }
   return {};
  });
 } catch (...) {
  // A context failure before entry admitted no decoder/storage work.
  // Escaping source failures forbid repeated candidate allocation; this
  // ordinary failed state does not imply unproved CUDA custody.
  if (entered) state.failed = true;
  throw;
 }
}
double VideoFileSource::frames_per_second() const noexcept { return owner_->state->fps; }
std::uint64_t VideoFileSource::frame_count() const noexcept { return owner_->state->total; }
}  // namespace mmltk::backend::media::video
