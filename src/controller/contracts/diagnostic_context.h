#pragma once

#include <cstdint>
#include <string_view>
#include "src/common/types/strong_id.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

namespace mmltk::controller::contracts {

enum class DiagnosticOwner : std::uint8_t {
    BrowserRuntime, BrowserServer, FirefoxProcess, Explore, Annotation, Upscale, Live, Presentation, AnnotationResource,
};

// Copies of identities owned elsewhere. None of these facts participates in
// ordering, resource release, or source matching.
struct DiagnosticSource final {
    std::uint64_t source_session = 0U;
    std::uint64_t source_instance = 0U;
    std::uint64_t source_revision = 0U;
    std::uint64_t clean_revision = 0U;
    std::uint64_t source_observation_revision = 0U;
    std::uint64_t source_width = 0U;
    std::uint64_t source_height = 0U;
    std::uint64_t content_x = 0U;
    std::uint64_t content_y = 0U;
    std::uint64_t content_width = 0U;
    std::uint64_t content_height = 0U;
};
struct DiagnosticDemand final {
    std::uint64_t demand_generation = 0U;
};
struct DiagnosticPublication final {
    std::uint64_t presentation_revision = 0U;
};
struct DiagnosticAllocation final {
    std::uint64_t allocation_generation = 0U;
};
struct DiagnosticTransfer final {
    std::uint64_t transfer_sequence = 0U;
    std::uint64_t timeline_ready = 0U;
};
using DiagnosticTraceId = mmltk::common::types::StrongId<struct DiagnosticTraceTag>;
using DiagnosticSpanId = mmltk::common::types::StrongId<struct DiagnosticSpanTag>;
struct DiagnosticLink final {
    DiagnosticTraceId trace_id{};
    DiagnosticSpanId span_id{};
    DiagnosticSpanId parent_span_id{};
};
enum class DiagnosticSpanOutcome : std::uint8_t { Unspecified, ScopeExit, Success, Cancelled, Exception };
struct DiagnosticSpanTiming final {
    std::uint64_t duration_ns = 0U;
    DiagnosticSpanOutcome span_outcome = DiagnosticSpanOutcome::Unspecified;
};
struct DiagnosticContext final {
    std::uint64_t capacity_width = 0U;
    std::uint64_t capacity_height = 0U;
    std::uint64_t staging_bytes = 0U;
    std::uint64_t cache_bytes = 0U;
    std::uint64_t descriptor_bytes = 0U;
    std::uint64_t gpu_bytes = 0U;
    std::uint64_t augmentation_device_bytes = 0U;
    std::uint64_t augmentation_pinned_bytes = 0U;
    std::uint64_t surface_high = 0U;
    std::uint64_t surface_low = 0U;
    std::uint64_t selection_generation = 0U;
    std::uint64_t frame_revision = 0U;
    std::uint64_t condition = 0U;
    std::uint64_t outcome = 0U;
    std::uint64_t observation_revision = 0U;
    std::string_view document_resource{};
    std::uint64_t document_revision = 0U;
    std::uint64_t document_meaning_identity = 0U;
    DiagnosticSource source{};
    DiagnosticDemand demand{};
    DiagnosticPublication publication{};
    DiagnosticAllocation allocation{};
    DiagnosticTransfer transfer{};
    DiagnosticLink link{};
    DiagnosticSpanTiming span{};
};

MMLTK_REFLECT_ENUM(DiagnosticOwner)
MMLTK_REFLECT_ENUM(DiagnosticSpanOutcome)
MMLTK_REFLECT_FIELDS(DiagnosticSource)
MMLTK_REFLECT_FIELDS(DiagnosticDemand)
MMLTK_REFLECT_FIELDS(DiagnosticPublication)
MMLTK_REFLECT_FIELDS(DiagnosticAllocation)
MMLTK_REFLECT_FIELDS(DiagnosticTransfer)
MMLTK_REFLECT_FIELDS(DiagnosticLink)
MMLTK_REFLECT_FIELDS(DiagnosticSpanTiming)
MMLTK_REFLECT_FIELDS(DiagnosticContext)

}  // namespace mmltk::controller::contracts
