#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <map>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
struct AdmittedRecipeArchive;
struct BenchmarkCacheLayout;
class BenchmarkCompilePipeline;
class StorageReservationPool;
struct CoconutPhysicalImage;
struct CoconutInventoryImage;
struct CoconutRecipeCatalog;
struct CoconutRecord;
class NormalizedAnnotationReadView;
struct CoconutPhysicalName {
 CoconutImageNamespace source;
 std::uint64_t id;
 std::string stem;
};
[[nodiscard]] CoconutPhysicalName parse_coconut_objects_member(std::string_view);
[[nodiscard]] std::uint64_t parse_coconut_coco_member(std::string_view);
void validate_coconut_physical_image(const CoconutPhysicalImage&);
[[nodiscard]] std::span<const CoconutImageNamespace> coconut_release_sources(CoconutEdition);
// One physical reader's future input. Lease/file controls remain separately
// charged retained handles; the decoder and encoded bytes are transient backing
// inside its producer's complete allowance. All dependent descriptors must be
// promised before that producer acquires any input.
class CoconutPhysicalInputRequirement final {
public:
 CoconutPhysicalInputRequirement() = default;
 [[nodiscard]] static CoconutPhysicalInputRequirement archive(std::uint64_t workspace_bytes);
 [[nodiscard]] BenchmarkResources lease_controls() const noexcept { return lease_; }
 [[nodiscard]] std::uint64_t workspace_bytes() const noexcept { return workspace_bytes_; }
 [[nodiscard]] std::size_t continuation_descriptors() const noexcept { return lease_.descriptors + lease_.continuation_descriptors; }

private:
 CoconutPhysicalInputRequirement(BenchmarkResources lease, std::uint64_t bytes) : lease_(lease), workspace_bytes_(bytes) {}
 BenchmarkResources lease_;
 std::uint64_t workspace_bytes_ = 0;
};
class CoconutPhysicalMembershipError final : public std::runtime_error {
public:
 CoconutPhysicalMembershipError(CoconutEdition edition, std::uint64_t image_id, std::string message) : std::runtime_error(std::move(message)), edition_(edition), image_id_(image_id) {}
 [[nodiscard]] CoconutEdition edition() const noexcept { return edition_; }
 [[nodiscard]] std::uint64_t image_id() const noexcept { return image_id_; }

private:
 CoconutEdition edition_;
 std::uint64_t image_id_;
};
class PhysicalArchiveFailure final : public std::runtime_error {
public:
 PhysicalArchiveFailure(AdmittedRecipeArchive& archive, std::string reason, std::exception_ptr fatal = {}) : std::runtime_error(std::move(reason)), archive_(&archive), fatal_(std::move(fatal)) {}
 [[nodiscard]] AdmittedRecipeArchive& archive() const noexcept { return *archive_; }
 [[nodiscard]] std::exception_ptr fatal() const noexcept { return fatal_; }

private:
 AdmittedRecipeArchive* archive_;
 std::exception_ptr fatal_;
};
// Full-backing dependencies are accumulated in the owning inventory traversal.
// Selected views retain this same summary, including unselected source images.
class CoconutPhysicalDependencies final {
public:
 void add(const CoconutPhysicalImage&);

private:
 friend class CoconutPhysicalMembership;
 std::map<std::uint16_t, std::string> shards_;
};
// Compile-local physical joins. Borrowed catalog/acquisition storage is stable
// until this owner and every importer retire. The compiler replaces an artifact
// only after withdrawal and settlement of its source publications/readers.
class CoconutPhysicalMembership final {
public:
 CoconutPhysicalMembership(std::span<AdmittedRecipeArchive>, const CoconutRecipeCatalog&, bool explicit_catalog, const BenchmarkCacheLayout&, StorageReservationPool&, BenchmarkCompilePipeline&,
  mmltk::common::concurrency::CancellationObservation = {});
 explicit CoconutPhysicalMembership(std::span<const CoconutPhysicalImage>, mmltk::common::concurrency::CancellationObservation = {});
 ~CoconutPhysicalMembership();
 CoconutPhysicalMembership(const CoconutPhysicalMembership&) = delete;
 CoconutPhysicalMembership& operator=(const CoconutPhysicalMembership&) = delete;
 [[nodiscard]] CoconutPhysicalInputRequirement input_requirement(CoconutEdition) const;
 // A sequence retires only the physical reader drawing on its own promise;
 // another concurrent sequence's reader remains live. Empty means all readers.
 void release_readers(CoconutEdition, const BenchmarkAllowance& producer = {}) const;
 [[nodiscard]] bool eligible(CoconutEdition, const AdmittedRecipeArchive&) const;
 [[nodiscard]] std::string dependency_identity(CoconutEdition, CoconutImageNamespace, const CoconutPhysicalDependencies& = {}, bool current = true) const;
 // Invalidates affected routes/joins and their cached input requirement before
 // the compiler mutates the admitted artifact; independent readers survive.
 void withdraw(const AdmittedRecipeArchive&);
 [[nodiscard]] CoconutPhysicalImage resolve(CoconutEdition, std::string_view identity, const CoconutRecord&, const BenchmarkAllowance&) const;
 [[nodiscard]] BenchmarkSourcePublication label_publication(CoconutEdition, const CoconutPhysicalImage&) const;
 [[nodiscard]] const CoconutPhysicalImage* find(CoconutImageNamespace, std::uint64_t) const noexcept;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::data::benchmark_internal
