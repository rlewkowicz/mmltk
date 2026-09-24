#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rendered_image_writer.h"
#include "detail/checked_png.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/common/concurrency/worker_pool.h"
#include <cuda_runtime.h>
#include <stb_image_write.h>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <exception>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
namespace mmltk::backend::imaging::raster {
namespace {
struct PngSink final {
 std::FILE* file;
 int error = 0;
 static void Write(void* context, void* bytes, int count) noexcept {
  auto& sink = *static_cast<PngSink*>(context);
  if (sink.error != 0 || count <= 0) return;
  errno = 0;
  const auto written = std::fwrite(bytes, 1U, static_cast<std::size_t>(count), sink.file);
  if (written != static_cast<std::size_t>(count) || std::ferror(sink.file)) sink.error = errno != 0 ? errno : EIO;
 }
};
[[noreturn]] void png_io_failure(const char* operation, const char* path, int error) {
 throw std::system_error(error, std::generic_category(), std::string(operation) + " rendered PNG: " + path);
}
// A PNG attempt owns one exclusive sibling and its open stream. No fixed
// staging entry is opened, followed, truncated, or removed.
class PngStagingFile final {
public:
 explicit PngStagingFile(const std::filesystem::path& destination) : path_(destination.string() + ".partial.XXXXXX") {
  const int descriptor = mkostemp(path_.data(), O_CLOEXEC);
  if (descriptor < 0) png_io_failure("create staging file for", destination.c_str(), errno);
  file_.reset(fdopen(descriptor, "wb"));
  if (!file_) {
   const int error = errno;
   close(descriptor);
   unlink(path_.c_str());
   png_io_failure("open staging stream for", destination.c_str(), error);
  }
 }
 ~PngStagingFile() {
  file_.reset();
  if (!path_.empty()) unlink(path_.c_str());
 }
 PngStagingFile(const PngStagingFile&) = delete;
 PngStagingFile& operator=(const PngStagingFile&) = delete;
 void Encode(const RenderedImageWriter::PngEncoder& encoder, const std::filesystem::path& destination,
             int width, int height, const void* pixels, int stride) {
  if (encoder) {
   // The optional encoder owns opening/closing this attempt's exclusive path.
   if (std::fclose(file_.release()) != 0) png_io_failure("close staging stream for", destination.c_str(), errno);
   if (encoder(path_.c_str(), width, height, 4, pixels, stride) == 0)
    throw std::runtime_error("failed to write rendered PNG: " + destination.string());
  } else {
   detail::write_png_stream(file_.release(), destination.c_str(), width, height, 4, pixels, stride);
  }
 }
 void Publish(const std::filesystem::path& destination) {
  std::filesystem::rename(path_, destination);
  path_.clear();
 }
private:
 std::string path_;
 std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr, &std::fclose};
};

}
void detail::validate_png_extent(std::size_t width, std::size_t height, std::size_t channels, std::size_t stride, const char* path) {
 constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
 const auto invalid = [&] { throw std::invalid_argument(std::string("unsupported rendered PNG extent: ") + path); };
 if (width == 0 || height == 0 || channels == 0 || channels > 4 || width > limit / channels || height > limit) invalid();
 const auto row = width * channels;
 if (stride == 0) stride = row;
 // stb accumulates absolute signed-byte filter scores in int and addresses
 // rows with signed stride * row_index. Bound both before any allocation.
 if (row > limit / 128U || stride < row || stride > limit || height > limit / stride || height > limit / (row + 1U)) invalid();
 const auto filtered = (row + 1U) * height;
 // Bundled fixed Huffman deflate emits at most nine bits per input byte
 // (match codes cost less), plus framing/padding. Reserve PNG's 57 bytes and
 // leave half the signed range for stb's doubling stretchy-buffer capacity.
 const auto compressed_bound = filtered + (filtered + 7U) / 8U + 64U;
 if (compressed_bound + 57U > (limit - 1U) / 2U) invalid();
}
int detail::write_png_stream(std::FILE* stream, const char* path, int width, int height, int channels, const void* pixels, int stride) {
 // Own the stream immediately, including exceptional encoder exit. The callback
 // cannot throw through stb: report only after it has released encoded storage.
 std::unique_ptr<std::FILE, decltype(&std::fclose)> file(stream, &std::fclose);
 validate_png_extent(width, height, channels, stride, path);
 PngSink sink{file.get()};
 const int encoded = stbi_write_png_to_func(&PngSink::Write, &sink, width, height, channels, pixels, stride);
 const char* failed_operation = "write";
 int error = sink.error;
 if (error == 0) {
  errno = 0;
  if (std::fflush(file.get()) != 0) {
   error = errno != 0 ? errno : EIO;
   failed_operation = "flush";
  }
 }
 errno = 0;
 const int closed = std::fclose(file.release());
 if (closed != 0 && error == 0) {
  error = errno != 0 ? errno : EIO;
  failed_operation = "close";
 }
 if (error != 0) png_io_failure(failed_operation, path, error);
 if (encoded == 0) throw std::runtime_error(std::string("failed to encode rendered PNG: ") + path);
 return encoded;
}
int detail::write_png_file(const char* path, int width, int height, int channels, const void* pixels, int stride) {
 validate_png_extent(width, height, channels, stride, path);
 auto* file = std::fopen(path, "wb");
 if (!file) png_io_failure("open", path, errno);
 return write_png_stream(file, path, width, height, channels, pixels, stride);
}
using mmltk::frameworks::gpu::ensure_cuda_ok;
struct RenderedImageWriter::Impl final {
 explicit Impl(mmltk::frameworks::gpu::DeviceContext owner, PngEncoder encoder) : context(std::move(owner)), stream(context), encode(std::move(encoder)) {
  context.Bind();
  pinned = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
 }
 mmltk::frameworks::gpu::DeviceContext context;
 mmltk::frameworks::gpu::ImageStream stream;
 std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> pinned;
 PngEncoder encode;
 mmltk::common::concurrency::WorkerPool pool{1, {}, "samplewrite"};
 std::future<std::filesystem::path> future;
};
RenderedImageWriter::RenderedImageWriter(mmltk::frameworks::gpu::DeviceContext context, PngEncoder encoder) : impl_(std::make_unique<Impl>(std::move(context),std::move(encoder))) {}
RenderedImageWriter::~RenderedImageWriter() { impl_->pool.wait_idle(); }
void RenderedImageWriter::Write(mmltk::frameworks::gpu::BorrowedImageProductReadView image, const std::filesystem::path& destination) {
 if (impl_->future.valid()) throw std::logic_error("previous rendered image write has not been settled");
 if (!image.valid() || !image.plane(0).UsesContext(impl_->context)) throw std::invalid_argument("rendered image context is invalid");
 const auto plane = image.plane(0).plane();
 const auto width = plane.descriptor.width, height = plane.descriptor.height;
 detail::validate_png_extent(width, height, 4U, 0U, destination.c_str());
 impl_->context.Bind();
 const auto pitch = static_cast<std::size_t>(width) * 4U;
 impl_->pinned->ensure_bytes(pitch * height);
 try {
  impl_->stream.Await(image);
  ensure_cuda_ok(cudaMemcpy2DAsync(impl_->pinned->data(), pitch, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, pitch, height, cudaMemcpyDeviceToHost, reinterpret_cast<cudaStream_t>(impl_->stream.native_handle())), "copy rendered image for PNG");
  impl_->stream.Synchronize();
 } catch (...) { impl_->stream.RethrowAfterSettlement(std::current_exception()); }
 // No borrowed source or CUDA API reaches the file worker.
 impl_->future = impl_->pool.enqueue([owner = impl_.get(), destination, width, height, pitch] {
  std::filesystem::create_directories(destination.parent_path());
  PngStagingFile staging(destination);
  staging.Encode(owner->encode, destination, static_cast<int>(width), static_cast<int>(height), owner->pinned->data(), static_cast<int>(pitch));
  staging.Publish(destination);
  return destination;
 });
}
std::filesystem::path RenderedImageWriter::Flush() { return impl_->future.valid() ? impl_->future.get() : std::filesystem::path{}; }
}  // namespace mmltk::backend::imaging::raster
