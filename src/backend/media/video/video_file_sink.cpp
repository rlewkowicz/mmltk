#include "video_file_sink.h"
#include "video_media_detail.h"
#include "video_frame_convert.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/cuda_error.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <exception>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
namespace mmltk::backend::media::video {
namespace {
std::int64_t shifted_time(std::int64_t value, AVRational base, std::int64_t origin, AVRational output) {
 if (base.num <= 0 || base.den <= 0 || output.num <= 0 || output.den <= 0) throw std::invalid_argument("video has an invalid rational time base");
 const auto converted = av_rescale_q(value, base, output);
 const auto offset = av_rescale_q(origin, AV_TIME_BASE_Q, output);
 if (converted == std::numeric_limits<std::int64_t>::min() || converted == std::numeric_limits<std::int64_t>::max() || (offset > 0 && converted <= std::numeric_limits<std::int64_t>::min() + offset) ||
     (offset < 0 && converted >= std::numeric_limits<std::int64_t>::max() + offset))
  throw std::overflow_error("video timestamp exceeds representable output timing");
 return converted - offset;
}
struct Options final {
 AVDictionary* value = nullptr;
 ~Options() { av_dict_free(&value); }
 void Set(const char* name, const char* text) { require_media(av_dict_set(&value, name, text, 0), "configure video output"); }
};
}  // namespace
struct VideoFileSink::State final {
 std::filesystem::path partial, complete;
 int descriptor = -1;
 FileWrite file_write;
 std::shared_ptr<const VideoMediaInfo::State> source;
 AVFormatContext* format = nullptr;
 AVCodecContext* encoder = nullptr;
 AVFrame* frame = av_frame_alloc();
 AVPacket* packet = av_packet_alloc();
 SwsContext* scaler = nullptr;
 std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> pinned;
 unsigned width = 0, height = 0;
 AVRational rate{};
 bool hardware = false, closed = false;
 std::optional<std::int64_t> last_pts, last_key;
 std::vector<std::optional<std::int64_t>> last_audio;
 std::deque<std::pair<std::int64_t, std::int64_t>> pending_frames;
 ~State() {
  // The partial path is deliberately never removed, including failed creation.
  if (format && format->pb) {
   avio_flush(format->pb);
   av_freep(&format->pb->buffer);
   avio_context_free(&format->pb);
  }
  if (descriptor >= 0) ::close(descriptor);
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&encoder);
  sws_freeContext(scaler);
  avformat_free_context(format);
 }
 static int WriteBytes(void* opaque, std::uint8_t* data, int size) noexcept {
  auto& self = *static_cast<State*>(opaque);
  int written = 0;
  try {
   while (written < size) {
    const auto result =
     self.file_write ? self.file_write(self.descriptor, data + written, static_cast<std::size_t>(size - written)) : ::write(self.descriptor, data + written, static_cast<std::size_t>(size - written));
    if (result < 0) {
     if (errno == EINTR) continue;
     return AVERROR(errno ? errno : EIO);
    }
    if (result == 0 || result > size - written) return AVERROR(EIO);
    written += static_cast<int>(result);
   }
   return written;
  } catch (...) { return AVERROR(EIO); }
 }
 void CloseFile() {
  int failure = 0;
  if (format && format->pb) {
   avio_flush(format->pb);
   failure = format->pb->error;
   av_freep(&format->pb->buffer);
   avio_context_free(&format->pb);
  }
  if (descriptor >= 0) {
   const auto file = descriptor;
   descriptor = -1;
   if (::close(file) < 0 && failure >= 0) failure = AVERROR(errno);
  }
  closed = true;
  require_media(failure, "close prediction Matroska output");
 }
 void Encoder(bool nvenc, int device) {
  avcodec_free_context(&encoder);
  av_frame_unref(frame);
  const auto* codec = avcodec_find_encoder_by_name(nvenc ? "h264_nvenc" : "libx264");
  if (!codec) throw std::runtime_error("H.264 encoder is unavailable");
  encoder = avcodec_alloc_context3(codec);
  if (!encoder) throw std::bad_alloc();
  encoder->width = static_cast<int>((width + 1U) & ~1U);
  encoder->height = static_cast<int>((height + 1U) & ~1U);
  encoder->time_base = source->time_base;
  if (encoder->time_base.num <= 0 || encoder->time_base.den <= 0) {
   if (rate.num <= 0 || rate.den <= 0) throw std::runtime_error("video output has unusable timing");
   encoder->time_base = av_inv_q(rate);
  }
  encoder->framerate = rate.num > 0 && rate.den > 0 ? rate : AVRational{0, 1};
  encoder->max_b_frames = 0;
  encoder->gop_size = std::numeric_limits<int>::max();
  encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  encoder->color_range = AVCOL_RANGE_MPEG;
  encoder->colorspace = AVCOL_SPC_BT709;
  encoder->pix_fmt = nvenc ? AV_PIX_FMT_CUDA : AV_PIX_FMT_YUV420P;
  Options options;
  options.Set("preset", nvenc ? "p4" : "veryfast");
  if (nvenc) {
   options.Set("rc", "vbr");
   options.Set("cq", "20");
   options.Set("rc-lookahead", "0");
   options.Set("zerolatency", "1");
   options.Set("delay", "0");
   options.Set("surfaces", "4");
   const auto name = std::to_string(device);
   require_media(av_hwdevice_ctx_create(&encoder->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, name.c_str(), nullptr, AV_CUDA_USE_CURRENT_CONTEXT), "initialize NVENC CUDA device");
   encoder->hw_frames_ctx = av_hwframe_ctx_alloc(encoder->hw_device_ctx);
   if (!encoder->hw_frames_ctx) throw std::bad_alloc();
   auto* frames = reinterpret_cast<AVHWFramesContext*>(encoder->hw_frames_ctx->data);
   frames->format = AV_PIX_FMT_CUDA;
   frames->sw_format = AV_PIX_FMT_NV12;
   frames->width = encoder->width;
   frames->height = encoder->height;
   frames->initial_pool_size = 4;
   require_media(av_hwframe_ctx_init(encoder->hw_frames_ctx), "initialize NVENC surfaces");
  } else {
   options.Set("crf", "20");
   options.Set("tune", "zerolatency");
  }
  require_media(avcodec_open2(encoder, codec, &options.value), "initialize H.264 encoder");
  hardware = nvenc;
  if (!hardware) {
   frame->format = encoder->pix_fmt;
   frame->width = encoder->width;
   frame->height = encoder->height;
   require_media(av_frame_get_buffer(frame, 32), "allocate software video surface");
   pinned = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
   pinned->ensure_bytes(static_cast<std::size_t>(encoder->width) * encoder->height * 4U);
   scaler = sws_getContext(encoder->width, encoder->height, AV_PIX_FMT_RGBA, encoder->width, encoder->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
   if (!scaler) throw std::runtime_error("software video conversion is unavailable");
   const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
   require_media(sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16), "configure video output colors");
  }
 }
 void FlushIo() {
  avio_flush(format->pb);
  require_media(format->pb->error, "flush prediction video");
 }
 void Mux(AVPacket* value, AVRational time_base, int track) {
  av_packet_rescale_ts(value, time_base, format->streams[track]->time_base);
  value->stream_index = track;
  // Immediate draining bounds the mux queue independently of absent/sparse
  // audio streams. The native demux/decoder retains source packet order.
  require_media(av_interleaved_write_frame(format, value), "mux prediction video");
  require_media(av_interleaved_write_frame(format, nullptr), "drain prediction interleaving");
  FlushIo();
 }
 void Drain() {
  for (;;) {
   const auto result = avcodec_receive_packet(encoder, packet);
   if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
   require_media(result, "receive H.264 packet");
   if (pending_frames.empty() || packet->pts != pending_frames.front().first) throw std::runtime_error("H.264 encoder changed frame order");
   packet->duration = pending_frames.front().second;
   pending_frames.pop_front();
   Mux(packet, encoder->time_base, 0);
   av_packet_unref(packet);
  }
 }
 void Finish() {
  std::exception_ptr failure;
  try {
   require_media(avcodec_send_frame(encoder, nullptr), "drain annotated video encoder");
   Drain();
   if (!pending_frames.empty()) throw std::runtime_error("H.264 encoder did not drain every annotated frame");
   // An orderly stop also settles the last cluster and buffered audio. Only
   // Complete publishes the finished filename; a crash still needs no trailer.
   require_media(av_write_trailer(format), "finish prediction Matroska output");
   FlushIo();
  } catch (...) { failure = std::current_exception(); }
  try {
   CloseFile();
  } catch (...) {
   if (!failure) failure = std::current_exception();
  }
  if (failure) std::rethrow_exception(failure);
 }
};
VideoFileSink::VideoFileSink(const std::filesystem::path& partial, const std::filesystem::path& completed, const VideoMediaInfo& info, int device, bool software_only, FileWrite file_write)
    : state_(std::make_unique<State>()) {
 auto& s = *state_;
 s.file_write = std::move(file_write);
 if (!info.state_ || !info.width || !info.height || info.width >= static_cast<unsigned>(std::numeric_limits<int>::max() / 4) ||
     info.height >= static_cast<unsigned>(std::numeric_limits<int>::max() / 4))
  throw std::invalid_argument("video output source geometry is unavailable or excessive");
 if (!s.frame || !s.packet) throw std::bad_alloc();
 s.width = info.width;
 s.height = info.height;
 s.rate = {info.rate_numerator, info.rate_denominator};
 s.source = info.state_;
 s.partial = partial;
 s.complete = completed;
 if (s.partial.empty() || s.complete.empty() || s.partial == s.complete) throw std::invalid_argument("video output paths must be distinct and nonempty");
 if (std::filesystem::exists(s.partial) || std::filesystem::exists(s.complete)) throw std::runtime_error("prediction video output already exists");
 require_media(avformat_alloc_output_context2(&s.format, nullptr, "matroska", s.partial.c_str()), "allocate Matroska output");
 if (!s.format) throw std::bad_alloc();
 for (const auto& track : s.source->audio)
  if (avformat_query_codec(s.format->oformat, track->parameters->codec_id, FF_COMPLIANCE_NORMAL) <= 0) throw std::invalid_argument("source audio is unsupported by Matroska");
 if (!software_only) {
  try {
   s.Encoder(true, device);
  } catch (...) { s.Encoder(false, device); }
 } else
  s.Encoder(false, device);
 auto* video = avformat_new_stream(s.format, nullptr);
 if (!video) throw std::bad_alloc();
 video->time_base = s.encoder->time_base;
 video->avg_frame_rate = s.encoder->framerate;
 require_media(avcodec_parameters_from_context(video->codecpar, s.encoder), "declare H.264 stream");
 for (const auto& input : s.source->audio) {
  auto* track = avformat_new_stream(s.format, nullptr);
  if (!track) throw std::bad_alloc();
  require_media(avcodec_parameters_copy(track->codecpar, input->parameters), "declare audio stream");
  track->codecpar->codec_tag = 0;
  track->time_base = input->time_base;
  track->disposition = input->disposition;
  require_media(av_dict_copy(&track->metadata, input->metadata, 0), "declare audio metadata");
 }
 s.last_audio.resize(s.source->audio.size());
 s.format->flags |= AVFMT_FLAG_FLUSH_PACKETS;
 s.format->max_interleave_delta = 1000000;
 s.format->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
 s.descriptor = ::open(s.partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
 if (s.descriptor < 0) require_media(AVERROR(errno), "open prediction partial video");
 auto* buffer = static_cast<unsigned char*>(av_malloc(32768U));
 if (!buffer) throw std::bad_alloc();
 s.format->pb = avio_alloc_context(buffer, 32768, 1, &s, nullptr, &State::WriteBytes, nullptr);
 if (!s.format->pb) {
  av_free(buffer);
  throw std::bad_alloc();
 }
 s.format->flags |= AVFMT_FLAG_CUSTOM_IO;
 Options mux;
 mux.Set("live", "1");
 mux.Set("cluster_time_limit", "1000");
 mux.Set("cluster_size_limit", "2097152");
 mux.Set("flush_packets", "1");
 require_media(avformat_write_header(s.format, &mux.value), "write Matroska header");
 s.FlushIo();
}
VideoFileSink::~VideoFileSink() = default;
void VideoFileSink::Write(mmltk::frameworks::gpu::ImagePlaneView image, VideoTiming timing, std::uintptr_t stream) {
 auto& s = *state_;
 if (s.closed || !image.valid() || image.descriptor.width != s.width || image.descriptor.height != s.height) throw std::invalid_argument("prediction video frame geometry changed");
 AVRational base{timing.time_base_numerator, timing.time_base_denominator};
 std::int64_t pts;
 if (timing.pts && base.num > 0 && base.den > 0)
  pts = shifted_time(*timing.pts, base, s.source->origin_us, s.encoder->time_base);
 else {
  if (s.rate.num <= 0 || s.rate.den <= 0) throw std::runtime_error("video output has unusable timing: no timestamp or frame rate");
  const auto step = av_rescale_q(1, av_inv_q(s.rate), s.encoder->time_base);
  if (step <= 0 || (s.last_pts && *s.last_pts > std::numeric_limits<std::int64_t>::max() - step)) throw std::runtime_error("video frame rate cannot supply valid timestamp progression");
  pts = s.last_pts ? *s.last_pts + step : 0;
 }
 if (s.last_pts && pts <= *s.last_pts) throw std::runtime_error("video output timestamps do not progress");
 if (s.hardware) {
  av_frame_unref(s.frame);
  require_media(av_hwframe_get_buffer(s.encoder->hw_frames_ctx, s.frame, 0), "acquire NVENC surface");
  mmltk::frameworks::gpu::ensure_cuda_ok(static_cast<cudaError_t>(convert_video_nv12(reinterpret_cast<const std::uint8_t*>(image.data), image.descriptor.pitch_bytes, s.width, s.height,
                                          s.frame->data[0], s.frame->linesize[0], s.frame->data[1], s.frame->linesize[1], reinterpret_cast<cudaStream_t>(stream))),
   "convert annotated video to NV12");
  mmltk::frameworks::gpu::ensure_cuda_ok(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream)), "settle annotated video conversion");
 } else {
  const auto pitch = static_cast<std::size_t>(s.encoder->width) * 4U;
  auto* rgba = static_cast<std::uint8_t*>(s.pinned->data());
  mmltk::frameworks::gpu::ensure_cuda_ok(
   cudaMemcpy2DAsync(rgba, pitch, reinterpret_cast<const void*>(image.data), image.descriptor.pitch_bytes, s.width * 4U, s.height, cudaMemcpyDeviceToHost, reinterpret_cast<cudaStream_t>(stream)),
   "read annotated video frame");
  mmltk::frameworks::gpu::ensure_cuda_ok(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream)), "settle annotated video readback");
  if (s.width % 2U)
   for (unsigned y = 0; y < s.height; ++y) std::memcpy(rgba + y * pitch + s.width * 4U, rgba + y * pitch + (s.width - 1U) * 4U, 4U);
  if (s.height % 2U) std::memcpy(rgba + s.height * pitch, rgba + (s.height - 1U) * pitch, pitch);
  require_media(av_frame_make_writable(s.frame), "reuse software video frame");
  const std::uint8_t* planes[]{rgba, nullptr, nullptr, nullptr};
  const int strides[]{static_cast<int>(pitch), 0, 0, 0};
  if (sws_scale(s.scaler, planes, strides, 0, s.encoder->height, s.frame->data, s.frame->linesize) != s.encoder->height) throw std::runtime_error("incomplete annotated video conversion");
 }
 s.frame->pts = pts;
 s.frame->duration = timing.duration > 0 && base.num > 0 && base.den > 0 ? av_rescale_q(timing.duration, base, s.encoder->time_base)
                     : s.rate.num > 0 && s.rate.den > 0                  ? av_rescale_q(1, av_inv_q(s.rate), s.encoder->time_base)
                                                                         : 0;
 const auto key_interval = (static_cast<std::uint64_t>(s.encoder->time_base.den) + s.encoder->time_base.num - 1U) / s.encoder->time_base.num;
 const bool key = !s.last_key || static_cast<std::uint64_t>(pts) - static_cast<std::uint64_t>(*s.last_key) >= key_interval;
 s.frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
 if (s.pending_frames.size() >= 4U) throw std::runtime_error("H.264 encoder exceeded bounded frame delay");
 s.pending_frames.emplace_back(pts, s.frame->duration);
 require_media(avcodec_send_frame(s.encoder, s.frame), "encode annotated video frame");
 s.Drain();
 s.last_pts = pts;
 if (key) s.last_key = pts;
}
void VideoFileSink::Audio(const VideoAudioPacket& audio) {
 auto& s = *state_;
 if (s.closed || !audio.state_ || !audio.state_->packet) throw std::invalid_argument("invalid video audio packet");
 const auto* input = audio.state_->packet;
 if (input->size < 0 || input->size > 16 * 1024 * 1024) throw std::runtime_error("audio packet exceeds bounded output capacity");
 for (std::size_t index = 0; index < s.source->audio.size(); ++index) {
  const auto& track = *s.source->audio[index];
  if (track.source != input->stream_index) continue;
  if (input->dts == AV_NOPTS_VALUE || input->pts == AV_NOPTS_VALUE) throw std::runtime_error("source audio has unusable timing");
  if (s.last_audio[index] && input->dts < *s.last_audio[index]) throw std::runtime_error("source audio timestamps regress");
  require_media(av_packet_ref(s.packet, input), "retain output audio");
  s.packet->pts = shifted_time(s.packet->pts, track.time_base, s.source->origin_us, track.time_base);
  s.packet->dts = shifted_time(s.packet->dts, track.time_base, s.source->origin_us, track.time_base);
  s.Mux(s.packet, track.time_base, static_cast<int>(index + 1U));
  av_packet_unref(s.packet);
  s.last_audio[index] = input->dts;
  return;
 }
 throw std::runtime_error("audio track changed after output admission");
}
void VideoFileSink::ClosePartial() {
 if (!state_->closed) state_->Finish();
}
void VideoFileSink::Complete() {
 auto& s = *state_;
 if (s.closed) throw std::logic_error("video output already closed");
 s.Finish();
 std::filesystem::rename(s.partial, s.complete);
}
}  // namespace mmltk::backend::media::video
