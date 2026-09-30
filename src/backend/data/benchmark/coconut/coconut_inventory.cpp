#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory_io.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/pch_linux.h"
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::backend::data::detail::CompileTaskKind;

namespace {
using Cancellation = mmltk::common::concurrency::CancellationObservation;
[[noreturn]] void invalid(std::string_view detail) { throw CoconutInputError("COCONut: " + std::string(detail)); }
template <InventoryRecord T>
std::string store_inventory(const std::filesystem::path& path, const InventoryHeader& header, std::span<const T> records, Cancellation cancellation, detail::StorageReservationPool* storage = nullptr) {
 throw_if_benchmark_cancelled(cancellation);
 (void)mmltk::common::io::ensure_parent_directory(path);
 detail::StorageReservationPool destination(path, {}, storage);
 auto staging = detail::StagedArtifact::create(destination, path, 0, "COCONut inventory staging");
 InventoryOutput output(&staging, cancellation);
 const auto identity = encode_inventory(output, header, records, cancellation);
 staging.file().sync_data();
 staging.publish(path, cancellation);
 return identity;
}
}  // namespace
void CoconutInventorySeal::capture(const mmltk::common::io::FileHandle& file) {
 struct stat status{};
 if (::fstat(file.get(), &status) != 0) throw mmltk::common::io::errno_error("stat COCONut inventory");
 if (status.st_size < 32) throw CoconutInputError("COCONut: truncated inventory");
 const auto size = mmltk::common::math::checked_cast<std::size_t>(status.st_size, "COCONut inventory size overflow");
 if (bytes && bytes != size) throw std::logic_error("COCONut inventory custody changed extent");
 if (!bytes) bytes = size;
 void* address = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, file.get(), 0);
 if (address == MAP_FAILED) throw mmltk::common::io::errno_error("map COCONut inventory");
 mapping.adopt(address, bytes);
 device = status.st_dev;
 inode = status.st_ino;
}
std::uint64_t InventoryOutput::byte_size() const { return mmltk::common::math::checked_add<std::uint64_t>(offset_, 32U, "inventory extent overflow"); }
std::string InventoryOutput::finish() {
 flush();
 const auto digest = hash_.Finish();
 if (file_) {
  file_->resize(byte_size(), "COCONut inventory digest extent");
  file_->file().pwrite_all(digest.data(), digest.size(), offset_);
  file_->reconcile();
 }
 throw_if_benchmark_cancelled(cancellation_);
 return mmltk::common::io::sha256_hex(digest);
}
void InventoryOutput::append(std::span<const std::uint8_t> bytes) {
 while (!bytes.empty()) {
  const auto count = std::min(bytes.size(), buffer_.size() - used_);
  std::memcpy(buffer_.data() + used_, bytes.data(), count);
  used_ += count;
  bytes = bytes.subspan(count);
  if (used_ == buffer_.size()) flush();
 }
}
void InventoryOutput::flush() {
 throw_if_benchmark_cancelled(cancellation_);
 if (!used_) return;
 if (used_ > std::numeric_limits<std::size_t>::max() - offset_) invalid("inventory size overflow");
 hash_.Update(std::span(buffer_.data(), used_));
 if (file_) {
  file_->resize(offset_ + used_ + 32, "COCONut inventory extent");
  file_->file().pwrite_all(buffer_.data(), used_, offset_);
  file_->reconcile();
 }
 offset_ += used_;
 used_ = 0;
}
InventoryInput::InventoryInput(const std::filesystem::path& path, Cancellation cancellation) : seal_(std::make_shared<CoconutInventorySeal>()), cancellation_(cancellation) {
 const auto file = mmltk::common::io::FileHandle::open_readonly(path.string());
 seal_->capture(file);
 seal_->directory = path.parent_path();
 seal_->published_path = path;
 content_size_ = seal_->bytes - 32;
}
std::string InventoryInput::finish() {
 throw_if_benchmark_cancelled(cancellation_);
 if (loaded_ != content_size_ || cursor_ != available_) invalid("inventory has trailing records");
 mmltk::common::io::Sha256Digest expected{};
 std::memcpy(expected.data(), seal_->data().data() + content_size_, expected.size());
 const auto actual = hash_.Finish();
 if (actual != expected) invalid("inventory checksum mismatch");
 return mmltk::common::io::sha256_hex(actual);
}
void InventoryInput::read(std::span<std::uint8_t> output) {
 while (!output.empty()) {
  if (cursor_ == available_) {
   throw_if_benchmark_cancelled(cancellation_);
   available_ = std::min(std::size_t{65536}, content_size_ - loaded_);
   cursor_ = 0;
   if (!available_) invalid("truncated inventory record");
   hash_.Update(seal_->data().subspan(loaded_, available_));
   loaded_ += available_;
  }
  const auto count = std::min(output.size(), available_ - cursor_);
  std::memcpy(output.data(), seal_->data().data() + loaded_ - available_ + cursor_, count);
  cursor_ += count;
  output = output.subspan(count);
 }
}
void CoconutInventorySeal::publish(const std::filesystem::path& inventory_path, Cancellation cancellation, detail::StorageReservationPool* storage) {
 const auto destination_directory = mmltk::common::io::ensure_parent_directory(inventory_path);
 struct stat destination{};
 if (::stat(destination_directory.c_str(), &destination) != 0) throw mmltk::common::io::errno_error("stat COCONut inventory destination");
 struct stat current{};
 const bool published = published_path == inventory_path && ::stat(inventory_path.c_str(), &current) == 0 && current.st_dev == device && current.st_ino == inode && current.st_size >= 0 &&
                        static_cast<std::uint64_t>(current.st_size) == bytes;
 if (!published) {
  bool moved = false;
  if (published_path.empty() && destination.st_dev == device && std::filesystem::is_regular_file(staged.path())) {
   try {
    staged.publish(inventory_path, cancellation);
    moved = true;
   } catch (const std::filesystem::filesystem_error& error) {
    if (error.code() != std::errc::cross_device_link) throw;
   }
  }
  if (!moved) {
   detail::StorageReservationPool reservations(inventory_path, {}, storage);
   auto copy = detail::StagedArtifact::create(reservations, inventory_path, bytes, "COCONut sealed inventory copy");
   copy.preallocate(bytes);
   for (std::size_t offset = 0; offset < bytes;) {
    throw_if_benchmark_cancelled(cancellation);
    const auto count = std::min(std::size_t{65536}, bytes - offset);
    copy.file().pwrite_all(data().data() + offset, count, offset);
    offset += count;
   }
   copy.file().sync_data();
   // Retain the inode we are about to publish, so inode-number reuse cannot
   // make a later pathname stat authorize unrelated bytes. The copied bytes
   // are already sealed; this mapping transition performs no decode or hash.
   capture(copy.file());
   staged = {};
   copy.publish(inventory_path, cancellation);
  }
  published_path = inventory_path;
 }
}
void validate_inventory_header(const InventoryHeader& header, const InventoryHeader& expected) {
 if (header.magic != expected.magic || header.version != expected.version || header.cache_schema != expected.cache_schema || header.normalization != expected.normalization ||
     header.input_identity != expected.input_identity || header.edition != expected.edition || header.source != expected.source || header.shard != expected.shard ||
     header.component != expected.component)
  invalid("inventory identity mismatch");
}
void validate_coconut_physical_image(const CoconutPhysicalImage& image) {
 (void)coconut_namespace_name(image.source);
 if (image.archive_identity.empty() || image.member != canonical_benchmark_archive_member(image.member)) invalid("invalid physical inventory identity");
 if (std::filesystem::path(image.member).extension() != ".jpg") invalid("physical member is not JPEG: " + image.member);
 if (image.source == CoconutImageNamespace::Objects365V1 || image.source == CoconutImageNamespace::Objects365V2) {
  const auto name = parse_coconut_objects_member(image.member);
  if (name.source != image.source || name.id != image.image_id) invalid("contradictory Objects365 physical identity: " + image.member);
 } else {
  if (parse_coconut_coco_member(image.member) != image.image_id) invalid("contradictory COCO physical identity: " + image.member);
  const std::string_view directory = image.source == CoconutImageNamespace::CocoTrain ? "train2017/" : image.source == CoconutImageNamespace::CocoUnlabeled ? "unlabeled2017/" : "val2017/";
  if (!image.member.starts_with(directory)) invalid("COCO physical subset disagrees with archive member: " + image.member);
 }
}
std::vector<CoconutPhysicalImage> load_coconut_physical_inventory(const std::filesystem::path& cache_path, CoconutImageNamespace source, std::uint16_t shard, std::string_view archive_identity,
 Cancellation cancellation, const std::function<void(const CoconutPhysicalImage&)>& ready) {
 InventoryHeader expected;
 expected.source = source;
 expected.shard = shard;
 expected.input_identity = archive_identity;
 InventoryInput input(cache_path, cancellation);
 InventoryHeader admitted;
 input.value(admitted);
 validate_inventory_header(admitted, expected);
 if (admitted.count > input.maximum_records()) invalid("inventory count exceeds file extent");
 std::vector<CoconutPhysicalImage> images;
 if (!ready) images.reserve(mmltk::common::math::checked_cast<std::size_t>(admitted.count, "physical inventory count overflow"));
 std::optional<std::uint64_t> previous;
 for (std::uint64_t i = 0; i < admitted.count; ++i) {
  throw_if_benchmark_cancelled(cancellation);
  CoconutPhysicalImage image;
  input.value(image);
  validate_coconut_physical_image(image);
  if (image.source != source || image.shard != shard || image.archive_identity != archive_identity || (previous && image.image_id <= *previous)) invalid("invalid cached archive inventory");
  previous = image.image_id;
  if (ready)
   ready(image);
  else
   images.push_back(std::move(image));
 }
 (void)input.finish();
 return images;
}
void store_coconut_physical_inventory(const std::filesystem::path& path, std::span<const CoconutPhysicalImage> images, CoconutImageNamespace source, std::uint16_t shard,
 std::string_view archive_identity, Cancellation cancellation, detail::StorageReservationPool* storage) {
 InventoryHeader expected;
 expected.source = source;
 expected.shard = shard;
 expected.input_identity = archive_identity;
 expected.count = images.size();
 (void)store_inventory(path, expected, images, cancellation, storage);
}
}  // namespace mmltk::backend::data::benchmark_internal
