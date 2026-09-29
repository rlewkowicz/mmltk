#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <map>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
struct AdmittedRecipeArchive;
struct DownloadResult;
struct BenchmarkCacheLayout;
class BenchmarkCompilePipeline;
class StorageReservationPool;
struct CoconutPhysicalImage;
struct CoconutInventoryImage;
struct CoconutRecipeCatalog;
struct CoconutRecord;
struct CachedImageDirectory;
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
class CoconutPhysicalMembershipError final : public std::runtime_error {
public:
 CoconutPhysicalMembershipError(CoconutEdition edition, std::uint64_t image_id, std::string message) : std::runtime_error(std::move(message)), edition_(edition), image_id_(image_id) {}
 [[nodiscard]] CoconutEdition edition() const noexcept { return edition_; }
 [[nodiscard]] std::uint64_t image_id() const noexcept { return image_id_; }

private:
 CoconutEdition edition_;
 std::uint64_t image_id_;
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
 // Synchronous observations of independently completed images and the source cursor.
 using Progress = std::function<void(CoconutEdition, const AdmittedRecipeArchive&, std::uint64_t image_id, std::uint64_t entries)>;
 CoconutPhysicalMembership(std::span<AdmittedRecipeArchive>, const CoconutRecipeCatalog&, bool explicit_catalog, const BenchmarkCacheLayout&, StorageReservationPool&, BenchmarkCompilePipeline&,
  mmltk::common::concurrency::CancellationObservation = {}, Progress = {});
 explicit CoconutPhysicalMembership(std::span<const CoconutPhysicalImage>, mmltk::common::concurrency::CancellationObservation = {});
 ~CoconutPhysicalMembership();
 CoconutPhysicalMembership(const CoconutPhysicalMembership&) = delete;
 CoconutPhysicalMembership& operator=(const CoconutPhysicalMembership&) = delete;
 // Annotation identity admission has no physical I/O or readiness wait.
 [[nodiscard]] CoconutPhysicalImage identify(CoconutEdition, const CoconutRecord&) const;
 [[nodiscard]] std::filesystem::path identity_root(CoconutImageNamespace) const;
 // One acquisition controller visits this source forward and publishes every
 // admitted image immediately. Annotation consumers never own its cursor.
 bool consume(AdmittedRecipeArchive&, const std::function<void(const CoconutPhysicalImage&, std::string_view)>& rejected = {}, bool cache_only = false, bool accept_partial = false);
 [[nodiscard]] CachedImageDirectory summary(const AdmittedRecipeArchive&) const;
 void admitted(AdmittedRecipeArchive&, DownloadResult, bool replacement = false);
 void wait_sources(std::optional<CoconutEdition> = {}) const;
 [[nodiscard]] bool discovery_complete() const;
 void unavailable(AdmittedRecipeArchive&);
 [[nodiscard]] bool eligible(CoconutEdition, const AdmittedRecipeArchive&) const;
 [[nodiscard]] std::string dependency_identity(CoconutEdition, CoconutImageNamespace, const CoconutPhysicalDependencies& = {}, bool current = true) const;
 // Invalidates affected routes and joins before
 // the compiler mutates the admitted artifact; independent readers survive.
 void withdraw(const AdmittedRecipeArchive&);
 // Settle identity against independently published source inventories.
 [[nodiscard]] CoconutPhysicalImage resolve(CoconutEdition, const CoconutRecord&) const;
 [[nodiscard]] BenchmarkSourcePublication label_publication(CoconutEdition, const CoconutPhysicalImage&) const;
 [[nodiscard]] const CoconutPhysicalImage* find(CoconutImageNamespace, std::uint64_t) const noexcept;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::data::benchmark_internal
