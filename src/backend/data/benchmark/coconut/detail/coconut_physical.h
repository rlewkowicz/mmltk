#pragma once  // backend.data private implementation boundary
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include "src/backend/data/detail/compile_resources.h"
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
struct AdmittedRecipeArchive {
 RecipeImageArchive origin;
 DownloadResult download;
};
struct BenchmarkCacheLayout;
class BenchmarkCompilePipeline;
struct CoconutPhysicalImage;
struct CoconutInventoryImage;
struct CoconutRecord;
struct CachedImageDirectory;
class NormalizedAnnotationReadView;
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
 struct Impl;

public:
 // Synchronous observations of independently completed images and the source cursor.
 using Progress = std::function<void(CoconutEdition, const AdmittedRecipeArchive&, std::uint64_t image_id, std::uint64_t entries)>;
 using Repair = std::function<void(AdmittedRecipeArchive&, std::string)>;
 CoconutPhysicalMembership(std::span<AdmittedRecipeArchive>, const CoconutRecipeCatalog&, bool explicit_catalog, const BenchmarkCacheLayout&, detail::StorageReservationPool&,
  BenchmarkCompilePipeline&, mmltk::common::concurrency::CancellationObservation = {}, Progress = {}, Repair = {});
 explicit CoconutPhysicalMembership(std::span<const CoconutPhysicalImage>, mmltk::common::concurrency::CancellationObservation = {});
 ~CoconutPhysicalMembership();
 CoconutPhysicalMembership(const CoconutPhysicalMembership&) = delete;
 CoconutPhysicalMembership& operator=(const CoconutPhysicalMembership&) = delete;
 // Copies retain one observation; independent observations have independent
 // custody. Releasing the last copy withdraws its callback without settling
 // another observer of the same identity.
 class Registration final {
 public:
  Registration() = default;
  explicit operator bool() const noexcept { return bool(state_); }
  [[nodiscard]] bool ready() const;
  void subscribe(std::function<void(std::exception_ptr)>) const;

 private:
  friend class CoconutPhysicalMembership;
  struct State;
  std::shared_ptr<State> state_;
 };
 struct Readiness {
  // Early identity includes an admitted encoded header. Terminal identity
  // also retains unreadable members for ordinary contextual rejection; neither
  // observation promises successful pixels or final membership.
  std::optional<CoconutPhysicalImage> image;
  std::uint64_t generation = 0;
  bool terminal = false;
  Registration changed;
 };
 // Snapshot and next-change subscription share the owner's generation lock;
 // subscribers run after that lock is released. Terminal missing/ambiguous
 // identities have no image; resolve supplies their contextual rejection.
 // terminal marks source/pixel settlement separately from recovery readiness.
 [[nodiscard]] Readiness readiness(CoconutEdition, const CoconutRecord&) const;
 // Annotation identity admission has no physical I/O or readiness wait. A
 // provisional identity supports normalization without granting eligibility.
 [[nodiscard]] CoconutPhysicalImage identify(CoconutEdition, const CoconutRecord&) const;
 [[nodiscard]] std::filesystem::path identity_root(CoconutImageNamespace) const;
 // One acquisition controller visits this source forward and publishes every
 // admitted image immediately. Annotation consumers never own its cursor.
 bool consume(AdmittedRecipeArchive&, const std::function<void(const CoconutPhysicalImage&, std::string_view)>& rejected = {}, bool cache_only = false);
 [[nodiscard]] CachedImageDirectory summary(const AdmittedRecipeArchive&) const;
 void admitted(AdmittedRecipeArchive&, DownloadResult, bool replacement = false);
 [[nodiscard]] bool can_retry(const AdmittedRecipeArchive&) const;
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
 [[nodiscard]] CoconutPhysicalImage resolve(CoconutEdition, const CoconutPhysicalImage&) const;
 [[nodiscard]] BenchmarkSourcePublication label_publication(CoconutEdition, const CoconutPhysicalImage&) const;
 [[nodiscard]] const CoconutPhysicalImage* find(CoconutImageNamespace, std::uint64_t) const noexcept;

private:
 std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::data::benchmark_internal
