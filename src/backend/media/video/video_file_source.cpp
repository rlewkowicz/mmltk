#include "video_file_source.h"
#include "video_frame_convert.h"
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
#include <libavutil/log.h>
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
import mmltk.common.logging.mmltk_logging;

namespace mmltk::backend::media::video {
namespace {
class DecoderContext final {
   public:
    explicit DecoderContext(CUcontext owner) {
        if (owner && cuCtxPushCurrent(owner) != CUDA_SUCCESS) throw std::runtime_error("video decoder context is unavailable");
        active_ = owner != nullptr;
    }
    ~DecoderContext() { if (active_) { CUcontext popped{}; static_cast<void>(cuCtxPopCurrent(&popped)); } }
   private:
    bool active_ = false;
};
void configure_video_logging() {
    static std::once_flag configured;
    std::call_once(configured, [] {
        av_log_set_level(AV_LOG_TRACE);
        av_log_set_callback([](void*, int level, const char* format, va_list arguments) {
            const auto severity = level <= AV_LOG_ERROR ? spdlog::level::err : level <= AV_LOG_WARNING ? spdlog::level::warn :
                level <= AV_LOG_INFO ? spdlog::level::info : level <= AV_LOG_DEBUG ? spdlog::level::debug : spdlog::level::trace;
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
        });
    });
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
}
struct VideoFileSource::State final {
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
    std::filesystem::path path;
    int device_id = 0;
    void Open(bool hardware);
    unsigned Rotation() const {
        std::size_t size = 0U;
        const auto* matrix = av_stream_get_side_data(format->streams[video_stream], AV_PKT_DATA_DISPLAYMATRIX, &size);
        if (const auto* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) { matrix = side->data; size = side->size; }
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
    ~State() {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    void Reserve(std::size_t pixels) {
        if (pixels <= capacity) return;
        cuda_check(cudaStreamSynchronize(stream));
        if (pixels > std::numeric_limits<std::size_t>::max() / (3U * sizeof(float))) throw std::runtime_error("video frame is too large");
        const auto release = [](float* address) noexcept { return cudaFree(address); };
        cuda_check(chw.RetryPending(release).failure);
        cuda_check(chw.AllocateCandidate([pixels](float*& address) noexcept { return cudaMalloc(reinterpret_cast<void**>(&address), pixels * 3U * sizeof(float)); }).failure);
        cuda_check(chw.PromoteCandidate(release).failure);
        capacity = pixels;
    }
    VideoFrame Convert() {
        if (frame->width <= 0 || frame->height <= 0 || frame->width > std::numeric_limits<int>::max() / 3)
            throw std::runtime_error("video frame has invalid dimensions");
        if (frame->colorspace == AVCOL_SPC_BT2020_CL || frame->colorspace == AVCOL_SPC_YCGCO ||
            frame->colorspace == AVCOL_SPC_SMPTE2085 || frame->colorspace == AVCOL_SPC_CHROMA_DERIVED_CL ||
            frame->colorspace == AVCOL_SPC_ICTCP)
            throw std::runtime_error("unsupported video color matrix");
        const auto width = static_cast<std::uint32_t>(frame->width);
        const auto height = static_cast<std::uint32_t>(frame->height);
        Reserve(static_cast<std::size_t>(width) * height);
        VideoColorConversion conversion;
        conversion.clockwise_quarters = Rotation();
        if (frame->format == AV_PIX_FMT_CUDA) {
            if (!frame->hw_frames_ctx || !frame->hw_frames_ctx->data) throw std::runtime_error("CUDA video frame lacks device custody");
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
                conversion.planes[channel] = {frame->data[component.plane], static_cast<std::size_t>(frame->linesize[component.plane]),
                    component.step, component.offset, component.shift, component.depth};
            }
            conversion.rgb = (description->flags & AV_PIX_FMT_FLAG_RGB) != 0;
            conversion.chroma_x = conversion.rgb ? 0 : description->log2_chroma_w;
            conversion.chroma_y = conversion.rgb ? 0 : description->log2_chroma_h;
            conversion.full_range = frame->color_range == AVCOL_RANGE_JPEG;
            switch (frame->colorspace) {
                case AVCOL_SPC_BT709: conversion.kr = 0.2126F; conversion.kb = 0.0722F; break;
                case AVCOL_SPC_BT2020_NCL: conversion.kr = 0.2627F; conversion.kb = 0.0593F; break;
                case AVCOL_SPC_FCC: conversion.kr = 0.30F; conversion.kb = 0.11F; break;
                case AVCOL_SPC_SMPTE240M: conversion.kr = 0.212F; conversion.kb = 0.087F; break;
                default: break;
            }
        } else {
            const auto bytes = static_cast<std::size_t>(width) * height * 3U;
            if (!pinned) pinned = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
            if (pinned->capacity_bytes() < bytes) pinned->ensure_bytes(bytes);
            if (rgb_capacity < bytes) {
                const auto release = [](std::uint8_t* address) noexcept { return cudaFree(address); };
                cuda_check(rgb.RetryPending(release).failure);
                cuda_check(rgb.AllocateCandidate([this](std::uint8_t*& address) noexcept { return cudaMalloc(reinterpret_cast<void**>(&address), pinned->capacity_bytes()); }).failure);
                cuda_check(rgb.PromoteCandidate(release).failure);
                rgb_capacity = pinned->capacity_bytes();
            }
            scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!scaler) throw std::runtime_error("video color converter unavailable");
            const auto color_space = frame->colorspace == AVCOL_SPC_UNSPECIFIED ? SWS_CS_DEFAULT : frame->colorspace;
            const auto* coefficients = sws_getCoefficients(color_space);
            check(sws_setColorspaceDetails(scaler, coefficients, frame->color_range == AVCOL_RANGE_JPEG,
                                          coefficients, 1, 0, 1 << 16, 1 << 16), "video color conversion");
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
        cuda_check(static_cast<cudaError_t>(convert_video_chw(conversion, width, height, chw.active(), reinterpret_cast<std::uintptr_t>(stream))));
        cuda_check(cudaEventRecord(source_read, stream));
        source_read_pending = true;
        VideoFrame result{chw.active(), conversion.clockwise_quarters % 2U ? height : width, conversion.clockwise_quarters % 2U ? width : height, index++, {}};
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            const auto seconds = frame->best_effort_timestamp * av_q2d(format->streams[video_stream]->time_base);
            if (std::isfinite(seconds)) result.presentation_seconds = seconds;
        }
        return result;
    }
};
struct VideoFileSource::Owner final {
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement{1U};
    mmltk::frameworks::gpu::TerminalCudaRetirementLease lease = Reserve();
    std::shared_ptr<State> state = std::make_shared<State>();
    mmltk::frameworks::gpu::TerminalCudaRetirementLease Reserve() {
        auto reserved = retirement.Reserve();
        if (!reserved) throw std::runtime_error("video retirement admission failed");
        return std::move(*reserved);
    }
    ~Owner() {
        cudaError_t failure = cudaSuccess;
        try {
            DecoderContext binding(state->context);
            if (state->stream) failure = cudaStreamSynchronize(state->stream);
            if (failure == cudaSuccess) failure = state->chw.ReleaseAll([](float* address) noexcept { return cudaFree(address); }).failure;
            if (failure == cudaSuccess) failure = state->rgb.ReleaseAll([](std::uint8_t* address) noexcept { return cudaFree(address); }).failure;
            if (failure == cudaSuccess && state->decoder_ready) failure = cudaEventDestroy(state->decoder_ready);
            if (failure == cudaSuccess && state->source_read) failure = cudaEventDestroy(state->source_read);
            if (failure == cudaSuccess && state->pinned && state->pinned->ReleaseSettled() != CUDA_SUCCESS) failure = cudaErrorUnknown;
            if (failure == cudaSuccess) state.reset();
        } catch (...) { failure = cudaErrorUnknown; }
        if (failure != cudaSuccess) std::move(lease).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(state)), failure);
    }
};
VideoFileSource::VideoFileSource(const std::filesystem::path& path, int device, std::uintptr_t stream, std::stop_token stop)
    : owner_(std::make_unique<Owner>()) {
    auto& state = *owner_->state;
    configure_video_logging();
    std::error_code path_error;
    if (!std::filesystem::is_regular_file(path, path_error)) throw std::invalid_argument("prediction video must be a local regular file");
    if (!stream || !state.frame || !state.packet) throw std::runtime_error("video decoder allocation failed");
    state.stop = stop;
    state.stream = reinterpret_cast<cudaStream_t>(stream);
    if (cuCtxGetCurrent(&state.context) != CUDA_SUCCESS || !state.context) throw std::runtime_error("video decoder requires an active CUDA context");
    state.path = path;
    state.device_id = device;
    try { state.Open(true); }
    catch (...) { if (stop.stop_requested()) return; state.Open(false); }
    cuda_check(cudaEventCreateWithFlags(&state.decoder_ready, cudaEventDisableTiming));
    cuda_check(cudaEventCreateWithFlags(&state.source_read, cudaEventDisableTiming));
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
    const auto inspected = avformat_find_stream_info(format, nullptr);
    if (stop.stop_requested()) return;
    check(inspected, "inspect prediction video");
    const AVCodec* decoder = nullptr;
    video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    check(video_stream, "select prediction video stream");
    auto* track = format->streams[video_stream];
    codec = avcodec_alloc_context3(decoder);
    if (!codec) throw std::bad_alloc();
    check(avcodec_parameters_to_context(codec, track->codecpar), "configure prediction decoder");
    hardware_attempt = false;
    if (hardware) for (int index = 0; const auto* config = avcodec_get_hw_config(decoder, index); ++index) {
        if (config->device_type != AV_HWDEVICE_TYPE_CUDA || !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
        const auto name = std::to_string(device_id);
        if (av_hwdevice_ctx_create(&codec->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, name.c_str(), nullptr, AV_CUDA_USE_CURRENT_CONTEXT) >= 0) {
            hardware_attempt = true;
        } else { av_buffer_unref(&codec->hw_device_ctx); }
        break;
    }
    codec->get_format = [](AVCodecContext* context, const AVPixelFormat* formats) {
        if (context->hw_device_ctx)
            for (auto* format = formats; *format != AV_PIX_FMT_NONE; ++format)
                if (*format == AV_PIX_FMT_CUDA) return *format;
        for (auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            const auto* descriptor = av_pix_fmt_desc_get(*format);
            if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) && sws_isSupportedInput(*format)) return *format;
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
    DecoderContext binding(state.context);
    // Only decoder inputs and pinned upload memory require host settlement.
    // CHW consumers and its next write remain ordered on the supplied stream.
    if (state.source_read_pending) cuda_check(cudaEventSynchronize(state.source_read));
    state.source_read_pending = false;
    av_frame_unref(state.frame);
    while (!state.stop.stop_requested()) {
        const auto received = avcodec_receive_frame(state.codec, state.frame);
        if (received == 0) {
            try { return state.Convert(); }
            catch (...) {
                if (!state.hardware_attempt || state.index != 0U || state.stop.stop_requested()) throw;
                cuda_check(cudaStreamSynchronize(state.stream));
                state.Open(false);
                continue;
            }
        }
        if (received < 0 && received != AVERROR(EAGAIN) && received != AVERROR_EOF && state.hardware_attempt && state.index == 0U) {
            state.Open(false);
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
            check(avcodec_send_packet(state.codec, nullptr), "drain prediction video");
        } else {
            check(read, "read prediction video");
            const auto submitted = avcodec_send_packet(state.codec, state.packet);
            if (submitted < 0 && state.hardware_attempt && state.index == 0U) { state.Open(false); continue; }
            check(submitted, "submit prediction video packet");
        }
    }
    return {};
}
double VideoFileSource::frames_per_second() const noexcept { return owner_->state->fps; }
std::uint64_t VideoFileSource::frame_count() const noexcept { return owner_->state->total; }
}
