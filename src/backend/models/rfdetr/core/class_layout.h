#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/common/io/file_digest.h"

namespace mmltk::backend::models::rfdetr {
// Session reuse compares named file identities at admission, never in a batch.
struct ClassArtifactSnapshot final {
    mmltk::common::io::FileSnapshot artifact;
    std::filesystem::path descriptor_path;
    std::optional<mmltk::common::io::FileSnapshot> companion, descriptor;
    auto operator<=>(const ClassArtifactSnapshot&) const = default;
    [[nodiscard]] static ClassArtifactSnapshot Read(const std::filesystem::path&, const std::filesystem::path& = {});
};
// Immutable admission result. Slots remain in physical order, retaining backend
// top-k ties and query identity independently of the foreground catalog order.
class ResolvedClassLayout final {
 public:
    explicit ResolvedClassLayout(ModelClassLayout record);
    [[nodiscard]] const ModelClassLayout& record() const noexcept { return record_; }
    [[nodiscard]] const std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>& catalog() const noexcept { return catalog_; }
    [[nodiscard]] bool semantic() const noexcept { return semantic_; }
    [[nodiscard]] std::size_t output_width() const noexcept { return record_.slots.size(); }
    [[nodiscard]] std::span<const std::int64_t> eligible_slots() const noexcept { return eligible_slots_; }
    [[nodiscard]] std::span<const std::int64_t> class_references() const noexcept { return class_references_; }
    [[nodiscard]] bool prefix_identity() const noexcept { return prefix_identity_; }
    [[nodiscard]] mmltk::backend::data::catalog::ClassReferenceDomain domain() const noexcept;
    [[nodiscard]] ModelClassLayoutSummary summary() const;
    void require_execution(bool require_semantic = false) const;
 private:
    ModelClassLayout record_;
    std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog_;
    std::vector<std::int64_t> eligible_slots_, class_references_;
    bool semantic_ = true;
    bool prefix_identity_ = true;
};
[[nodiscard]] ModelClassLayout native_training_class_layout(const mmltk::backend::data::catalog::ClassCatalog& catalog);
[[nodiscard]] ModelClassLayout unresolved_class_layout(std::size_t output_width);
[[nodiscard]] ModelClassLayout coco_class_layout(ClassLayoutProvenance provenance);
[[nodiscard]] std::string encode_class_layout(const ModelClassLayout& layout);
[[nodiscard]] ModelClassLayout decode_class_layout(std::string_view text);
[[nodiscard]] std::string encode_class_descriptor(const ModelClassDescriptor& descriptor);
[[nodiscard]] ModelClassDescriptor read_class_descriptor(const std::filesystem::path& path);
[[nodiscard]] ModelClassDescriptor decode_class_descriptor(std::string_view text);
[[nodiscard]] std::vector<ModelClassDescriptor> read_artifact_class_descriptors(const std::filesystem::path& artifact,
    const mmltk::common::io::FileDigests& admitted_file, const std::filesystem::path& explicit_descriptor = {});
[[nodiscard]] std::vector<RfdetrNamedOutputRole> class_descriptor_output_roles(std::span<const ModelClassDescriptor> descriptors);
[[nodiscard]] ModelClassLayout admit_artifact_class_layout(std::size_t output_width,
    const std::optional<ModelClassLayout>& embedded, std::span<const ModelClassDescriptor> descriptors);
[[nodiscard]] ModelClassLayout admit_artifact_class_layout(const std::filesystem::path& artifact,
    std::size_t output_width, const std::optional<ModelClassLayout>& embedded,
    const mmltk::common::io::FileDigests& admitted_file, const std::filesystem::path& explicit_descriptor = {},
    const std::vector<ModelClassDescriptor>* admitted_descriptors = nullptr);
}  // namespace mmltk::backend::models::rfdetr
