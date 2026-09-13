#pragma once

#include <cstdint>
#include <string_view>
#include "src/common/types/strong_id.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

namespace mmltk::controller::contracts {

enum class DiagnosticOwner : std::uint8_t {
    BrowserRuntime,
    BrowserServer,
    FirefoxProcess,
    Explore,
    Annotation,
    Upscale,
    Live,
    Presentation,
    AnnotationResource,
    // CLEANUP-IGNORE: Closing the diagnostic-owner enum before the source schema is a canonical reflected boundary,
    // not a reusable scalar record shared with kernel ABIs.
};

// Copies of identities owned elsewhere. None of these facts participates in
// ordering, resource release, or source matching.
// CLEANUP-IGNORE: DiagnosticSource is the canonical trace source schema, not a common ABI shared with CUDA records.
struct DiagnosticSource final {
    // CLEANUP-IGNORE: Trace source identity scalars cannot share storage with unrelated physical geometry records.
    std::uint64_t source_session = 0U;
    std::uint64_t source_instance = 0U;
    // CLEANUP-IGNORE: Source revision and adjacent source/demand fields are canonical trace facts, not physical layout storage.
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
    // CLEANUP-IGNORE: Allocation identity and the following workspace schema are independent reflected records, not a repeated
    // implementation.
    std::uint64_t allocation_generation = 0U;
    // CLEANUP-IGNORE: Adjacent allocation and workspace schemas are not a repeated progress record.
};
// CLEANUP-IGNORE: Workspace storage fields differ from requested and observed admission progress.
struct DiagnosticWorkspace final {
    // CLEANUP-IGNORE: Workspace provenance is its canonical reflected schema; ABI and acceptance inventories retain separate ownership.
    std::uint64_t workspace_source_high = 0U;
    // CLEANUP-IGNORE: Source and allocation identities are not requested or observed product state.
    std::uint64_t workspace_source_low = 0U;
    std::uint64_t workspace_allocation = 0U;
    std::uint64_t workspace_arena_high = 0U;
    // CLEANUP-IGNORE: Workspace arena identity and byte layout are unrelated to the CUDA probe ABI and admission demand fields.
    std::uint64_t workspace_arena_low = 0U;
    // CLEANUP-IGNORE: Workspace byte geometry and admission priority have distinct meanings from pixel evidence and trace capacity.
    std::uint64_t workspace_bytes = 0U;
    std::uint64_t workspace_pitch = 0U;
    std::uint64_t workspace_width = 0U;
    std::uint64_t workspace_height = 0U;
    // Process-local pointer provenance; neither field is a cross-process GPU
    // allocation identity or participates in workspace ownership.
    std::uint64_t native_process_id = 0U;
    std::uint64_t workspace_plane = 0U;
    bool direct_sampling = false;
};
struct DiagnosticWorkspaceProgress final {
    // CLEANUP-IGNORE: Progress observations are independent of graphics ABI fields and trace capacities.
    std::uint64_t requested_product_owner = 0U;
    std::uint64_t requested_product_revision = 0U;
    std::uint64_t observed_product_owner = 0U;
    std::uint64_t observed_product_revision = 0U;
    std::uint64_t suppressed_request_owner = 0U;
    // CLEANUP-IGNORE: Admission state differs from the acceptance viewer's rendered-grid evidence.
    std::uint64_t admitted_allocation = 0U;
    std::uint64_t candidate_allocation = 0U;
    std::uint64_t candidate_product_owner = 0U;
    std::uint64_t expected_workspace_pitch = 0U;
    std::uint64_t expected_workspace_bytes = 0U;
    std::uint64_t expected_device_incarnation = 0U;
    std::uint64_t live_source_count = 0U;
    bool candidate_admitted = false;
    bool candidate_write_available = false;
    bool workspace_layout_matches = false;
    bool workspace_admitted = false;
    bool workspace_write_available = false;
    bool source_timeline_imported = false;
    bool source_acquired = false;
    bool source_release_submitted = false;
    bool source_withdrawing = false;
};
struct DiagnosticExploreAdmission final {
    std::uint64_t admission_position = 0U;
    std::uint64_t admission_first_row = 0U;
    std::uint64_t admission_row_count = 0U;
    std::uint64_t admission_columns = 0U;
    std::uint64_t admission_tier = 0U;
    bool admission_forward = true;
    std::uint64_t admission_immediate_eligible = 0U;
    std::uint64_t admission_forward_eligible = 0U;
    std::uint64_t admission_backward_eligible = 0U;
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
struct DiagnosticPixel final {
    std::uint32_t sample_index = 0U;
    std::uint32_t sample_x = 0U;
    std::uint32_t sample_y = 0U;
    // CLEANUP-IGNORE: Pixel color followed by the trace envelope is not workspace admission state.
    std::uint32_t sample_rgba = 0U;
    // CLEANUP-IGNORE: Closing the pixel evidence record before the trace envelope is a reflected schema boundary.
};
// CLEANUP-IGNORE: DiagnosticContext is the canonical heterogeneous trace envelope, not a kernel ABI.
struct DiagnosticContext final {
    // CLEANUP-IGNORE: The diagnostic capacity envelope is not interchangeable with a scheduler lane or an external image ABI.
    std::uint64_t capacity_width = 0U;
    // CLEANUP-IGNORE: Trace capacity fields are not interchangeable with source or crop geometry.
    std::uint64_t capacity_height = 0U;
    std::uint64_t staging_bytes = 0U;
    std::uint64_t cache_bytes = 0U;
    std::uint64_t descriptor_bytes = 0U;
    std::uint64_t metadata_bytes = 0U;
    std::string_view metadata_fingerprint{};
    std::uint64_t gpu_bytes = 0U;
    // CLEANUP-IGNORE: Augmentation capacity and source selection are diagnostic facts, not a physical source admission record.
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
    DiagnosticWorkspace workspace{};
    DiagnosticWorkspaceProgress workspace_progress{};
    DiagnosticExploreAdmission admission{};
    DiagnosticLink link{};
    DiagnosticSpanTiming span{};
    DiagnosticPixel pixel{};
};

MMLTK_REFLECT_ENUM(DiagnosticOwner)
MMLTK_REFLECT_ENUM(DiagnosticSpanOutcome)
MMLTK_REFLECT_FIELDS(DiagnosticSource)
MMLTK_REFLECT_FIELDS(DiagnosticDemand)
MMLTK_REFLECT_FIELDS(DiagnosticPublication)
MMLTK_REFLECT_FIELDS(DiagnosticAllocation)
MMLTK_REFLECT_FIELDS(DiagnosticWorkspace)
MMLTK_REFLECT_FIELDS(DiagnosticWorkspaceProgress)
MMLTK_REFLECT_FIELDS(DiagnosticExploreAdmission)
MMLTK_REFLECT_FIELDS(DiagnosticTransfer)
MMLTK_REFLECT_FIELDS(DiagnosticLink)
MMLTK_REFLECT_FIELDS(DiagnosticSpanTiming)
MMLTK_REFLECT_FIELDS(DiagnosticPixel)
MMLTK_REFLECT_FIELDS(DiagnosticContext)

}  // namespace mmltk::controller::contracts
