#include "src/backend/data/compiler/detail/dataset_compiler_internal.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
import mmltk.common.logging.profile_utils;
using mmltk::backend::data::detail::CompileChange;
namespace mmltk::backend::data::compiler_internal {
namespace {
using common::io::MappedFile;
using detail::CompileAllowance;
using detail::CompileExecution;
using detail::CompileTaskKind;
using detail::CompileWorkClass;
enum class ReadyState { Ready, Active, Complete };
struct ImageWork {
 explicit ImageWork(CompileExecution& execution) : label_workspace(execution) {}
 std::uint32_t index = 0;
 bool occupied = false, geometry_ready = false, header_complete = false, labels_complete = false, header_reported = false, pixel_reported = false, label_reported = false;
 ReadyState opened = ReadyState::Ready, header = ReadyState::Ready, prepared = ReadyState::Ready, pixels = ReadyState::Ready, labels = ReadyState::Ready, output = ReadyState::Ready;
 // Each mapping retains the opened generation. No whole input cache is pinned.
 // Descriptor custody covers both maps until their consumers have retired.
 CompileAllowance descriptors, label_custody;
 MappedFile encoded, annotations;
 std::optional<detail::CompilePixelWorkspacePlan> plan;
 std::size_t scan_cursor = 0, line_bytes = 0, maximum_line = 0, encoded_extent = 0;
 DirectoryLabels label_workspace;
 detail::CompiledLabelWorkspace::OutputLease payload;
 ImageEntry entry{};
};
void observe(const CompilerConfig& config, CompileTaskKind kind, std::uint32_t image, bool complete) {
 if (config.observation && config.observation->task) config.observation->task(config.observation->context, kind, image, complete);
}
bool inspect_input(ImageWork& image, const CompilerConfig& config) {
 if (!image.plan) {
  detail::CompileImageDecoder decoder;
  detail::CompileImageHeader header;
  try {
   header = decoder.read_header({image.encoded.data(), image.encoded.size()}, detail::CompileImagePolicy::Directory);
  } catch (const detail::CompileImageError& error) {
   throw std::runtime_error("failed to read image dimensions for " + image_path(std::filesystem::path(config.source_dir) / config.split, image.index).string() + ": " + error.what());
  }
  // Planning is immutable and complete before either consumer is published.
  image.plan.emplace(detail::compile_pixel_workspace_plan(header, {config.target_width, config.target_height, config.resize_mode, config.perceptual_downscale}));
  image.encoded_extent = common::math::checked_add(image.encoded.size(), std::size_t{63}, "encoded image workspace overflow") / 64 * 64;
 }
 // Only raw record extents are measured here, in bounded header continuations.
 // Labels, masks, semantics and the final output extent are each projected once.
 const auto end = image.scan_cursor + std::min<std::size_t>(1U << 20, image.annotations.size() - image.scan_cursor);
 while (image.scan_cursor < end) {
  const auto* begin = image.annotations.data() + image.scan_cursor;
  const auto* newline = static_cast<const std::uint8_t*>(std::memchr(begin, '\n', end - image.scan_cursor));
  const auto bytes = newline ? static_cast<std::size_t>(newline - begin) : end - image.scan_cursor;
  image.line_bytes = common::math::checked_add(image.line_bytes, bytes, "annotation line size overflow");
  image.scan_cursor += bytes;
  if (newline) {
   image.maximum_line = std::max(image.maximum_line, image.line_bytes);
   image.line_bytes = 0;
   ++image.scan_cursor;
  }
 }
 if (image.scan_cursor != image.annotations.size()) return false;
 image.maximum_line = std::max(image.maximum_line, image.line_bytes);
 return true;
}
}  // namespace
LabelBlocks compile_directory_images(const std::filesystem::path& split, std::uint32_t count, const CompilerConfig& config, const catalog::ClassCatalog& catalog, std::uint8_t category_base,
 detail::CompiledSlotStorage& storage, CompileExecution& execution, detail::FileWorkQueue& files, ProgressCounter* label_progress, ProgressCounter* pixel_progress,
 common::concurrency::CancellationObservation cancellation) {
 LabelBlocks result;
 result.index.resize(count);
 // A bounded source window replaces static ranges and group joins. Slot identity
 // always remains its numbered input identity, independent of ready-task order.
 const auto capacity = std::min<std::size_t>(count, std::max<std::size_t>(2, execution.workers() * 2));
 std::vector<std::unique_ptr<ImageWork>> images;
 images.reserve(capacity);
 for (std::size_t i = 0; i < capacity; ++i) images.push_back(std::make_unique<ImageWork>(execution));
 std::vector<std::unique_ptr<detail::CompilePixelWorkspace>> pixels;
 for (std::size_t i = 0; i < execution.workers(); ++i) pixels.push_back(std::make_unique<detail::CompilePixelWorkspace>());
 auto extraction = execution.open_class(CompileWorkClass::Extraction);
 auto annotations = execution.open_class(CompileWorkClass::Annotations);
 auto pixel_producer = execution.open_class(CompileWorkClass::Pixels);
 std::mutex mutex;
 std::condition_variable changed;
 std::uint64_t revision = 0;
 std::size_t active = 0, headers_left = count, labels_left = count, pixels_left = count, settled = 0;
 std::uint32_t next = 0;
 std::exception_ptr failure;
 std::vector<CompileDiagnostic> diagnostics;
 CompileExecution::Readiness readiness(execution, CompileChange::Resources | CompileChange::Records, [&](CompileExecution::Readiness&, CompileExecution::Readiness::Changes) {
  const std::lock_guard lock(mutex);
  ++revision;
  changed.notify_all();
 });
 const auto reject = [&](std::exception_ptr error) {
  execution.fail(error);
  if (!failure) failure = execution.interruption();
 };
 // Accepted callbacks are the only writer of ready/completion facts. They run
 // after the CPU/file grant is returned and preserve custody until physical exit.
 const auto complete = [&](ImageWork& image, ReadyState& state, CompileTaskKind kind, std::exception_ptr error) noexcept {
  try {
   if (!error) observe(config, kind, image.index, true);
  } catch (...) { error = std::current_exception(); }
  const std::lock_guard lock(mutex);
  if (error) reject(error);
  state = ReadyState::Complete;
  --active;
  ++revision;
  changed.notify_all();
 };
 std::unique_lock lock(mutex);
 try {
  while (settled < count && !failure) {
   if (const auto interrupted = execution.interruption()) {
    failure = interrupted;
    break;
   }
   const auto observed = revision;
   bool made_ready = false;
   for (auto& pointer : images) {
    auto& image = *pointer;
    if (!image.occupied && next < count) {
     image.index = next++;
     image.occupied = true;
     made_ready = true;
    }
    if (!image.occupied) continue;
    if (image.opened == ReadyState::Ready) {
     CompileExecution::Readiness::Attempt attempt(&readiness);
     if (!image.descriptors) {
      auto allowance = execution.try_reserve(detail::CompileResources::handles(2));
      if (!allowance) continue;
      image.descriptors = std::move(*allowance);
     }
     auto task = files.try_submit({.completed = [&complete, &image](std::exception_ptr error) { complete(image, image.opened, CompileTaskKind::Archive, error); }, .work = [&image, &split, &config] {
      observe(config, CompileTaskKind::Archive, image.index, false);
      try {
       image.encoded = MappedFile::open_readonly(image_path(split, image.index).string());
      } catch (const std::exception& error) { throw std::runtime_error("failed to read image dimensions for " + image_path(split, image.index).string() + ": " + error.what()); }
      try {
       image.annotations = MappedFile::open_readonly(annotation_path(split, image.index).string());
      } catch (const std::exception& error) { throw std::runtime_error("failed to open annotation file: " + annotation_path(split, image.index).string() + ": " + error.what()); }
     }, .service_class = CompileWorkClass::Extraction});
     if (task) {
      image.opened = ReadyState::Active;
      ++active;
      attempt.commit();
     }
     continue;
    }
    if (image.opened != ReadyState::Complete) continue;
    if (image.header == ReadyState::Complete) {
     image.geometry_ready = true;
     if (!image.header_complete)
      image.header = ReadyState::Ready;
     else if (!image.header_reported) {
      image.header_reported = true;
      if (!--headers_left) extraction.close();
     }
    }
    if (image.header == ReadyState::Ready) {
     auto task = execution.try_submit({.kind = CompileTaskKind::Header, .work = [&image, &config](std::size_t, const CompileAllowance&) {
      observe(config, CompileTaskKind::Header, image.index, false);
      image.header_complete = inspect_input(image, config);
     }, .retain_workspace = false, .completed = [&complete, &image](std::exception_ptr error) { complete(image, image.header, CompileTaskKind::Header, error); }});
     if (task) {
      image.header = ReadyState::Active;
      ++active;
     }
    }
    if (!image.geometry_ready) continue;
    // A separate prepared extent prevents mapped allocation under a CPU or facts
    // lock. Labels are independently ready from the very same admitted geometry.
    if (image.prepared == ReadyState::Ready) {
     auto task =
      files.try_submit({.completed = [&complete, &image](std::exception_ptr error) { complete(image, image.prepared, CompileTaskKind::Output, error); }, .work = [&image, &storage, &config] {
      observe(config, CompileTaskKind::Output, image.index, false);
      storage.prepare_pixels(image.index);
     }});
     if (task) {
      image.prepared = ReadyState::Active;
      ++active;
     }
    }
    if (image.prepared == ReadyState::Complete && image.pixels == ReadyState::Ready) {
     auto demand = image.plan->resources();
     demand.bytes = common::math::checked_add(demand.bytes, image.encoded_extent, "encoded pixel workspace overflow");
     auto task =
      execution.try_submit({.kind = CompileTaskKind::Pixels, .resources = demand, .work = [&image, &storage, &config, &pixels, pixel_progress](std::size_t lane, const CompileAllowance& allowance) {
      observe(config, CompileTaskKind::Pixels, image.index, false);
      ProgressBatch progress(pixel_progress);
      auto memory = allowance.workspace();
      auto encoded = std::span(reinterpret_cast<std::uint8_t*>(memory.data()), image.encoded.size());
      // The opened immutable source remains sufficient custody while this one
      // envelope waits for exceptional admission; no normal input lease is held.
      std::memcpy(encoded.data(), image.encoded.data(), encoded.size());
      auto destination = storage.pixels(image.index);
      try {
       detail::CompilePixelObserver observer;
       if (config.observation && config.observation->pixel)
        observer = [&](const detail::CompilePixelObservation& value) { config.observation->pixel(config.observation->context, image.index, value); };
       pixels[lane]->write(*image.plan, encoded, image.plan->header(), memory.subspan(image.encoded_extent), {destination.data(), image.plan->destination()}, +[] {}, observer);
      } catch (const detail::CompileImageError& error) {
       throw std::runtime_error("failed to load image file: " + image_path(std::filesystem::path(config.source_dir) / config.split, image.index).string() + ": " + error.what());
      }
      progress.increment();
     }, .finite_consumer = true, .retain_workspace = false, .completed = [&complete, &image](std::exception_ptr error) { complete(image, image.pixels, CompileTaskKind::Pixels, error); }});
     if (task) {
      image.pixels = ReadyState::Active;
      ++active;
     }
    }
    if (image.labels == ReadyState::Complete && !image.labels_complete) image.labels = ReadyState::Ready;
    if (image.header_reported && image.labels == ReadyState::Ready) {
     const auto demand = DirectoryLabels::resources(image.plan->header().width, image.maximum_line, config.max_label_bytes);
     auto task = execution.try_submit(
      {.kind = CompileTaskKind::Labels, .resources = demand, .work = [&image, &config, &catalog, category_base, &split, cancellation, label_progress](std::size_t, const CompileAllowance& allowance) {
      observe(config, CompileTaskKind::Labels, image.index, false);
      ProgressBatch progress(label_progress);
      if (!image.label_custody) {
       image.label_workspace.begin(image.index, allowance, {image.annotations.data(), image.annotations.size()}, image.maximum_line, image.plan->header(), config, catalog, category_base,
        annotation_path(split, image.index), image_path(split, image.index));
       image.label_custody = allowance;
      }
      image.labels_complete = image.label_workspace.step(cancellation);
      if (image.labels_complete) {
       image.entry = image.label_workspace.entry();
       image.payload = image.label_workspace.release_output();
      }
     }, .allowance = image.label_custody, .finite_consumer = true, .retain_workspace = false, .completed = [&complete, &image](std::exception_ptr error) {
      complete(image, image.labels, CompileTaskKind::Labels, error);
     }});
     if (task) {
      image.labels = ReadyState::Active;
      ++active;
     }
    }
    if (image.labels == ReadyState::Complete && image.labels_complete && image.output == ReadyState::Ready) {
     auto task =
      files.try_submit({.completed = [&complete, &image](std::exception_ptr error) { complete(image, image.output, CompileTaskKind::CacheWrite, error); }, .work = [&image, &storage, &config] {
      observe(config, CompileTaskKind::CacheWrite, image.index, false);
      storage.labels(image.index).write(image.entry, std::move(image.payload));
     }});
     if (task) {
      image.output = ReadyState::Active;
      ++active;
      image.label_workspace.take_diagnostics(diagnostics);
      image.label_workspace.reset();
      image.label_custody = {};
     }
    }
    if (image.pixels == ReadyState::Complete && !image.pixel_reported) {
     image.pixel_reported = true;
     if (!--pixels_left) pixel_producer.close();
    }
    if (image.output == ReadyState::Complete && !image.label_reported) {
     image.label_reported = true;
     if (label_progress) label_progress->add_completed(1);
     if (!--labels_left) annotations.close();
    }
    if (image.pixels == ReadyState::Complete && image.output == ReadyState::Complete) {
     result.index[image.index] = image.entry;
     result.max_instances_per_image = std::max<std::uint32_t>(result.max_instances_per_image, image.entry.num_instances);
     ++settled;
     // All accepted operations have returned before this reusable context loses
     // source mappings, descriptors, payload leases and the immutable plan.
     image.encoded = {};
     image.annotations = {};
     image.descriptors = {};
     image.plan.reset();
     image.scan_cursor = 0;
     image.line_bytes = 0;
     image.maximum_line = 0;
     image.encoded_extent = 0;
     image.geometry_ready = false;
     image.header_complete = false;
     image.labels_complete = false;
     image.header_reported = false;
     image.pixel_reported = false;
     image.label_reported = false;
     image.entry = {};
     image.payload = {};
     image.opened = image.header = image.prepared = image.pixels = image.labels = image.output = ReadyState::Ready;
     image.occupied = false;
     made_ready = true;
    }
   }
   if (settled == count) break;
   if (!made_ready) changed.wait(lock, [&] { return revision != observed || bool(failure); });
  }
 } catch (...) { reject(std::current_exception()); }
 if (failure) {
  lock.unlock();
  files.cancel();
  lock.lock();
  // Rejected/ready continuations settle locally. Accepted operations alone own
  // physical completion; cancellation never returns their storage early.
  changed.wait(lock, [&] { return active == 0; });
 }
 lock.unlock();
 files.join();
 try {
  execution.join();
 } catch (...) {
  if (!failure) failure = std::current_exception();
 }
 if (failure) std::rethrow_exception(failure);
 // Preserve numbered-file/record diagnostic order independently of scheduling.
 std::ranges::sort(diagnostics, [](const auto& left, const auto& right) {
  if (left.annotation_path.size() != right.annotation_path.size()) return left.annotation_path.size() < right.annotation_path.size();
  if (left.annotation_path != right.annotation_path) return left.annotation_path < right.annotation_path;
  return left.line < right.line;
 });
 if (config.diagnostics)
  for (auto& diagnostic : diagnostics) try {
    config.diagnostics->push_back(std::move(diagnostic));
   } catch (...) {}
 std::size_t labels = 0, runs = 0;
 for (const auto& entry : result.index) {
  labels += entry.num_instances;
  runs += entry.num_runs;
 }
 mmltk::common::logging::profile_set_value("compiler.labels.total_labels", labels);
 mmltk::common::logging::profile_set_value("compiler.labels.total_rle_pairs", runs);
 mmltk::common::logging::profile_set_value("compiler.labels.max_instances_per_image", result.max_instances_per_image);
 return result;
}
}  // namespace mmltk::backend::data::compiler_internal
