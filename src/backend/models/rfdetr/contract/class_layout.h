#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/models/rfdetr/contract/output_roles.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"

namespace mmltk::backend::models::rfdetr {
inline constexpr std::uint32_t kClassLayoutVersion = 1U;
inline constexpr std::size_t kMaximumClassOutputSlots = 4096U;
inline constexpr std::size_t kClassLayoutByteBudget = 1024U * 1024U;
enum class ClassSlotRole : std::uint8_t { Foreground, Background, Unused, Unresolved };
enum class ClassScoreEncoding : std::uint8_t { SigmoidLogits, SoftmaxLogits };
enum class NoObjectEncoding : std::uint8_t { AllNegative, ExplicitBackground, Unspecified };
enum class ClassLayoutOrigin : std::uint8_t { Unresolved, NativeTraining, Embedded, DigestDescriptor, VerifiedAsset };
MMLTK_REFLECT_ENUM(ClassSlotRole)
MMLTK_REFLECT_ENUM(ClassScoreEncoding)
MMLTK_REFLECT_ENUM(NoObjectEncoding)
MMLTK_REFLECT_ENUM(ClassLayoutOrigin)
struct ModelClassSlot final {
    ClassSlotRole role = ClassSlotRole::Unresolved;
    std::optional<std::uint32_t> foreground_index;
    auto operator<=>(const ModelClassSlot&) const = default;
};
MMLTK_REFLECT_FIELDS(ModelClassSlot)
struct ClassLayoutProvenance final {
    ClassLayoutOrigin origin = ClassLayoutOrigin::Unresolved;
    [[= mmltk::frameworks::reflection::MaxBytes{1024U}]] std::string producer;
    [[= mmltk::frameworks::reflection::MaxBytes{64U}]] std::string artifact_sha256;
    auto operator<=>(const ClassLayoutProvenance&) const = default;
};
MMLTK_REFLECT_FIELDS(ClassLayoutProvenance)
struct ModelClassLayout final {
    std::uint32_t version = kClassLayoutVersion;
    mmltk::backend::data::catalog::OrderedClassCatalog foreground;
    // Producer names without a slot schema are evidence only, never labels.
    mmltk::backend::data::catalog::OrderedClassCatalog class_name_evidence;
    [[= mmltk::frameworks::reflection::MaxItems{kMaximumClassOutputSlots}]] std::vector<ModelClassSlot> slots;
    ClassScoreEncoding scores = ClassScoreEncoding::SigmoidLogits;
    NoObjectEncoding no_object = NoObjectEncoding::Unspecified;
    ClassLayoutProvenance provenance;
    // Only supported producer metadata establishes the independent supervision
    // class axis. A sparse classifier map does not establish that ordering.
    bool supervision_in_foreground_order = false;
    auto operator<=>(const ModelClassLayout&) const = default;
};
MMLTK_REFLECT_FIELDS(ModelClassLayout)
struct ModelClassLayoutSummary final {
    mmltk::backend::data::catalog::ClassReferenceDomain domain = mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot;
    std::uint32_t foreground_count = 0, output_count = 0, background_count = 0, unused_count = 0;
    ClassScoreEncoding scores = ClassScoreEncoding::SigmoidLogits;
    NoObjectEncoding no_object = NoObjectEncoding::Unspecified;
    ClassLayoutProvenance provenance;
    auto operator<=>(const ModelClassLayoutSummary&) const = default;
};
MMLTK_REFLECT_FIELDS(ModelClassLayoutSummary)
struct ModelClassDescriptor final {
    std::uint32_t version = 1U;
    [[= mmltk::frameworks::reflection::MaxBytes{64U}]] std::string artifact_sha256;
    ModelClassLayout layout;
    [[= mmltk::frameworks::reflection::MaxItems{3U}]] std::vector<RfdetrNamedOutputRole> output_roles;
};
MMLTK_REFLECT_FIELDS(ModelClassDescriptor)
}  // namespace mmltk::backend::models::rfdetr
