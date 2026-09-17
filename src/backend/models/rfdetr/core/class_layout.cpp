#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/common/io/file_digest.h"
#include "src/frameworks/serialization/json_wire.h"
#include "src/backend/data/catalog/coco_catalog.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::backend::models::rfdetr {
namespace catalog = mmltk::backend::data::catalog;
ResolvedClassLayout::ResolvedClassLayout(ModelClassLayout record) : record_(std::move(record)) {
    if (record_.version != kClassLayoutVersion || record_.slots.size() > kMaximumClassOutputSlots)
        throw std::invalid_argument("unsupported or oversized RF-DETR class layout");
    if (record_.provenance.producer.find('\0') != std::string::npos || record_.provenance.producer.size() > 1024 ||
        record_.provenance.artifact_sha256.size() > 64)
        throw std::invalid_argument("oversized RF-DETR class provenance");
    if (!record_.provenance.artifact_sha256.empty() &&
        (record_.provenance.artifact_sha256.size() != 64 ||
         !std::ranges::all_of(record_.provenance.artifact_sha256, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })))
        throw std::invalid_argument("invalid RF-DETR class provenance digest");
    if (record_.provenance.origin > ClassLayoutOrigin::VerifiedAsset) throw std::invalid_argument("unknown RF-DETR class provenance");
    if (record_.class_name_evidence.names.size() > catalog::kClassCatalogCapacity) throw std::invalid_argument("oversized class-name evidence");
    for (const auto& name : record_.class_name_evidence.names)
        if (name.value.size() > catalog::kClassNameCapacity || name.value.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid class-name evidence");
    catalog_ = std::make_shared<const catalog::ClassCatalog>(record_.foreground);
    std::array<bool, catalog::kClassCatalogCapacity> seen{};
    std::size_t background_count = 0;
    for (const auto& slot : record_.slots) {
        switch (slot.role) {
            case ClassSlotRole::Foreground:
                if (!slot.foreground_index || *slot.foreground_index >= catalog_->size() || seen[*slot.foreground_index])
                    throw std::invalid_argument("duplicate or invalid foreground output slot");
                seen[*slot.foreground_index] = true;
                break;
            case ClassSlotRole::Background: ++background_count; [[fallthrough]];
            case ClassSlotRole::Unused:
                if (slot.foreground_index) throw std::invalid_argument("non-foreground slot has a foreground index");
                break;
            case ClassSlotRole::Unresolved:
                if (slot.foreground_index) throw std::invalid_argument("unresolved slot has a foreground index");
                semantic_ = false;
                break;
            default: throw std::invalid_argument("unknown RF-DETR class slot role");
        }
    }
    for (std::size_t index = 0; index < catalog_->size(); ++index)
        if (!seen[index]) throw std::invalid_argument("class layout omits a foreground class");
    if (!semantic_ && !catalog_->empty()) throw std::invalid_argument("partial foreground bindings are unsupported");
    if (semantic_ && record_.provenance.origin == ClassLayoutOrigin::Unresolved)
        throw std::invalid_argument("resolved class layout requires authoritative provenance");
    if (record_.supervision_in_foreground_order && !semantic_) throw std::invalid_argument("unresolved class layout cannot declare foreground supervision");
    switch (record_.no_object) {
        case NoObjectEncoding::AllNegative:
            if (background_count) throw std::invalid_argument("all-negative layout declares explicit background");
            break;
        case NoObjectEncoding::ExplicitBackground:
            if (background_count != 1) throw std::invalid_argument("explicit background layout requires one background slot");
            break;
        case NoObjectEncoding::Unspecified:
            if (semantic_ || background_count) throw std::invalid_argument("resolved layout requires no-object encoding");
            break;
        default: throw std::invalid_argument("unknown no-object encoding");
    }
    if (record_.scores != ClassScoreEncoding::SigmoidLogits && record_.scores != ClassScoreEncoding::SoftmaxLogits)
        throw std::invalid_argument("unknown score encoding");
    if (record_.scores == ClassScoreEncoding::SoftmaxLogits && record_.no_object == NoObjectEncoding::AllNegative)
        throw std::invalid_argument("softmax cannot encode all-negative no-object targets");
    eligible_slots_.reserve(record_.slots.size());
    class_references_.reserve(record_.slots.size());
    for (std::size_t index = 0; index < record_.slots.size(); ++index) {
        const auto& slot = record_.slots[index];
        if (slot.role != ClassSlotRole::Foreground && slot.role != ClassSlotRole::Unresolved) continue;
        eligible_slots_.push_back(static_cast<std::int64_t>(index));
        class_references_.push_back(semantic_ ? *slot.foreground_index : static_cast<std::int64_t>(index));
        prefix_identity_ = prefix_identity_ && index == eligible_slots_.size() - 1 && class_references_.back() == static_cast<std::int64_t>(index);
    }
}
catalog::ClassReferenceDomain ResolvedClassLayout::domain() const noexcept {
    return semantic_ ? catalog::ClassReferenceDomain::Foreground : catalog::ClassReferenceDomain::RawOutputSlot;
}
ModelClassLayoutSummary ResolvedClassLayout::summary() const {
    ModelClassLayoutSummary result{.domain = domain(),
                                   .foreground_count = static_cast<std::uint32_t>(catalog_->size()),
                                   .output_count = static_cast<std::uint32_t>(output_width()),
                                   .scores = record_.scores,
                                   .no_object = record_.no_object,
                                   .provenance = record_.provenance};
    for (const auto& slot : record_.slots) {
        result.background_count += slot.role == ClassSlotRole::Background;
        result.unused_count += slot.role == ClassSlotRole::Unused;
    }
    return result;
}
void ResolvedClassLayout::require_execution(bool require_semantic) const {
    if (record_.scores != ClassScoreEncoding::SigmoidLogits) throw std::invalid_argument("RF-DETR execution requires declared sigmoid logits");
    if (require_semantic && !semantic_) throw std::invalid_argument("RF-DETR semantic output requires a bound class layout");
}
ModelClassLayout native_training_class_layout(const catalog::ClassCatalog& catalog) {
    ModelClassLayout result;
    result.foreground = catalog.record();
    result.no_object = NoObjectEncoding::AllNegative;
    result.provenance.origin = ClassLayoutOrigin::NativeTraining;
    result.provenance.producer = "mmltk.rfdetr.native_checkpoint/3";
    result.supervision_in_foreground_order = true;
    for (std::size_t index = 0; index < catalog.size(); ++index) result.slots.push_back({ClassSlotRole::Foreground, static_cast<std::uint32_t>(index)});
    result.slots.push_back({ClassSlotRole::Unused, std::nullopt});
    return result;
}
ModelClassLayout unresolved_class_layout(std::size_t output_width) {
    if (output_width == 0 || output_width > kMaximumClassOutputSlots) throw std::invalid_argument("RF-DETR output width exceeds layout capacity");
    ModelClassLayout result;
    result.slots.resize(output_width);
    return result;
}
ModelClassLayout coco_class_layout(ClassLayoutProvenance provenance) {
    ModelClassLayout result;
    result.slots.resize(91, {ClassSlotRole::Unused, std::nullopt});
    result.no_object = NoObjectEncoding::AllNegative;
    result.provenance = std::move(provenance);
    for (std::size_t index = 0; index < catalog::kCocoCategories.size(); ++index) {
        const auto& category = catalog::kCocoCategories[index];
        result.foreground.names.push_back({std::string(category.name)});
        result.slots[category.source_id] = {ClassSlotRole::Foreground, static_cast<std::uint32_t>(index)};
    }
    return result;
}
namespace {
template <class Record>
std::string encode_record(const Record& record) {
    std::vector<std::byte> bytes;
    if (!mmltk::frameworks::serialization::encode(record, bytes, {.max_bytes = kClassLayoutByteBudget, .max_items = kClassLayoutByteBudget}))
        throw std::invalid_argument("cannot encode RF-DETR class record");
    return mmltk::frameworks::serialization::json_from_cbor({bytes, {}}, {.max_bytes = kClassLayoutByteBudget, .max_items = kClassLayoutByteBudget}).dump();
}
template <class Record>
Record decode_record(std::string_view text) {
    if (text.size() > kClassLayoutByteBudget) throw std::invalid_argument("RF-DETR class record exceeds byte budget");
    const auto bytes = mmltk::frameworks::serialization::json_to_cbor(text, {.max_bytes = kClassLayoutByteBudget, .max_items = kClassLayoutByteBudget});
    auto decoded = mmltk::frameworks::serialization::decode<Record>({bytes, {}}, {.max_bytes = kClassLayoutByteBudget, .max_items = kClassLayoutByteBudget});
    if (!decoded) throw std::invalid_argument("invalid RF-DETR class record");
    return std::move(*decoded);
}
void validate_descriptor(const ModelClassDescriptor& descriptor) {
    if (descriptor.version != 1U) throw std::invalid_argument("unsupported RF-DETR class descriptor version");
    static_cast<void>(mmltk::common::io::parse_sha256_hex(descriptor.artifact_sha256));
    const ResolvedClassLayout validated(descriptor.layout);
    if (descriptor.output_roles.size() > 3) throw std::invalid_argument("oversized class descriptor output roles");
    std::array<bool, 4> roles{};
    for (std::size_t index = 0; index < descriptor.output_roles.size(); ++index) {
        const auto& output = descriptor.output_roles[index];
        const auto role = static_cast<std::size_t>(output.role);
        if (output.name.empty() || output.name.size() > 1024 || output.name.find('\0') != std::string::npos || role == 0 || role >= roles.size() || roles[role])
            throw std::invalid_argument("invalid descriptor output role");
        roles[role] = true;
        for (std::size_t prior = 0; prior < index; ++prior)
            if (descriptor.output_roles[prior].name == output.name) throw std::invalid_argument("duplicate descriptor output name");
    }
}
}  // namespace
std::string encode_class_layout(const ModelClassLayout& layout) {
    const ResolvedClassLayout validated(layout);
    return encode_record(layout);
}
ModelClassLayout decode_class_layout(std::string_view text) {
    auto record = decode_record<ModelClassLayout>(text);
    const ResolvedClassLayout validated(record);
    return record;
}
std::string encode_class_descriptor(const ModelClassDescriptor& descriptor) {
    validate_descriptor(descriptor);
    return encode_record(descriptor);
}
ModelClassDescriptor decode_class_descriptor(std::string_view text) {
    auto descriptor = decode_record<ModelClassDescriptor>(text);
    validate_descriptor(descriptor);
    descriptor.artifact_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::parse_sha256_hex(descriptor.artifact_sha256));
    return descriptor;
}
std::vector<RfdetrNamedOutputRole> class_descriptor_output_roles(std::span<const ModelClassDescriptor> descriptors) {
    std::vector<RfdetrNamedOutputRole> roles;
    for (const auto& descriptor : descriptors)
        for (const auto& declared : descriptor.output_roles) {
            const auto previous = std::ranges::find(roles, declared.name, &RfdetrNamedOutputRole::name);
            if (previous == roles.end())
                roles.push_back(declared);
            else if (previous->role != declared.role)
                throw std::invalid_argument("authoritative output roles disagree");
        }
    if (roles.size() > 3) throw std::invalid_argument("too many artifact output roles");
    return roles;
}
ModelClassLayout admit_artifact_class_layout(std::size_t output_width, const std::optional<ModelClassLayout>& embedded,
                                             std::span<const ModelClassDescriptor> descriptors) {
    std::optional<ModelClassLayout> selected = embedded;
    for (const auto& descriptor : descriptors) {
        auto next = descriptor.layout;
        if (selected) {
            const ResolvedClassLayout previous(*selected);
            if (previous.semantic() && *selected != next) throw std::invalid_argument("authoritative class layouts disagree");
            if (!previous.semantic() && next.class_name_evidence.names.empty()) next.class_name_evidence = selected->class_name_evidence;
        }
        selected = std::move(next);
    }
    auto result = selected ? std::move(*selected) : unresolved_class_layout(output_width);
    const ResolvedClassLayout validated(result);
    if (validated.output_width() != output_width) throw std::invalid_argument("class layout disagrees with artifact output width");
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
