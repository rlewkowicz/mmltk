#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/types.h>
#include "src/controller/subsystems/explore/explore_system.h"
namespace mmltk::acceptance::wayland {
using mmltk::controller::ExploreAcceptanceGate;
struct NativeAudit final {
    struct FinalCursorGenerations final {
        std::uint64_t material = 0U;
        std::uint64_t cursor = 0U;
    };
    struct ViewportAcceptance final {
        std::uint64_t endpoint = 0U;
        std::uint64_t generation = 0U;
        std::size_t ordinal = 0U;
    };
    bool server_started = false;
    bool peer_opened = false;
    bool shutdown_requested = false;
    bool firefox_terminal = false;
    bool shutdown_complete = false;
    bool shutdown_incomplete = false;
    bool explore_rendered = false;
    bool annotation_copied = false;
    bool annotation_opened = false;
    bool annotation_edited = false;
    bool presentation_ready = false;
    bool worker_failed = false;
    bool invalid_message = false;
    bool peer_replaced = false;
    // CLEANUP-IGNORE: Native audit flags and browser UI state are separate acceptance evidence records.
    bool interaction_rejected = false;
    bool peer_closed_after_shutdown = false;
    bool explore_placeholder = false;
    bool explore_partial_patch = false;
    bool explore_ready_batch = false;
    bool explore_stale_discard = false;
    bool explore_tile_regressed = false;
    bool explore_nproc_changed = false;
    bool explore_stale_patch = false;
    bool admission_seen = false;
    bool admission_priority_valid = true;
    std::set<std::pair<std::uint64_t, std::uint64_t>> admitted_reads;
    bool acceptance_first_patch_exact = false;
    bool acceptance_held_read = false;
    bool acceptance_held_completed = false;
    bool acceptance_held_released = false;
    bool acceptance_held_stale = false;
    bool causal_inconsistent = false;
    std::string_view causal_failure;
    // CLEANUP-IGNORE: Native process custody begins a distinct evidence group from CUDA render-layout records.
    pid_t firefox_pid = -1;
    // CLEANUP-IGNORE: Presentation and Explore runtime counters are acceptance evidence, not kernel ABI geometry.
    std::uint64_t presentation_timeline = 0U;
    std::uint64_t explore_generation = 0U;
    std::uint64_t explore_nproc = 0U;
    std::uint64_t partial_generation = 0U;
    std::uint64_t explore_stale_count = 0U;
    std::size_t explore_placeholder_ordinal = 0U;
    // CLEANUP-IGNORE: Partial-patch ordering counters are distinct from rendered-card geometry fields.
    std::size_t partial_placeholder_ordinal = 0U;
    std::size_t partial_first_patch_ordinal = 0U;
    std::uint64_t partial_first_tile_count = 0U;
    // CLEANUP-IGNORE: Acceptance scheduling and hold-custody fields are not the reflected diagnostic envelope schema.
    std::size_t explore_max_pinned = 0U;
    std::size_t ordinal = 0U;
    std::size_t peer_open_count = 0U;
    std::size_t peer_close_count = 0U;
    std::uint64_t held_generation = 0U;
    std::uint64_t held_slot = 0U;
    std::uint64_t held_compiled_index = 0U;
    std::uint64_t held_capacity = 0U;
    std::size_t held_ordinal = 0U;
    std::size_t held_release_ordinal = 0U;
    std::size_t held_discard_ordinal = 0U;
    std::map<std::uint64_t, std::size_t> accepted_generations;
    std::vector<ViewportAcceptance> accepted_viewports;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> placeholder_slots;
    struct InitialCache final {
        std::set<std::uint64_t> slots;
        std::size_t restored_ordinal = 0U;
        bool forward = true;
    };
    std::map<std::uint64_t, InitialCache> initial_cache;
    bool cached_first_valid = true;
    std::map<std::uint64_t, std::size_t> placeholder_cardinalities;
    std::map<std::uint64_t, std::uint64_t> placeholder_digests;
    std::map<std::uint64_t, std::size_t> placeholder_ordinals;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> patched_slots;
    std::map<std::uint64_t, std::size_t> last_patch_ordinals;
    std::map<std::uint64_t, std::size_t> first_publication_ordinals;
    std::map<std::uint64_t, std::size_t> last_publication_ordinals;
    // CLEANUP-IGNORE: Tile, augmentation, and frame evidence have distinct identities from placeholder/patch inventories.
    std::map<std::uint64_t, std::uint64_t> first_published_tiles;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> first_patched_slots;
    std::map<std::uint64_t, std::uint64_t> published_tiles;
    std::map<std::uint64_t, std::uint64_t> augmentation_seeds;
    std::map<std::uint64_t, std::uint64_t> augmentation_pixel_seeds;
    using FramePublication = std::pair<std::size_t, std::uint64_t>;
    std::map<std::uint64_t, std::vector<FramePublication>> published_frames;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> presented_explore_frames;
    bool overlay_descriptors = false;
    std::set<std::pair<std::uint64_t, std::uint64_t>> transformed_overlay_slots;
    std::set<std::pair<std::uint64_t, std::uint64_t>> semantic_overlay_slots;
    enum class PaddingOrientation : std::uint8_t {
        Vertical,
        Horizontal,
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> padded_card_slots;
    std::map<std::pair<std::uint64_t, std::uint64_t>, PaddingOrientation> padding_orientations;
    using OverlayDescriptorIdentity = std::pair<std::uint64_t, std::uint64_t>;
    std::map<std::pair<std::uint64_t, std::uint64_t>, OverlayDescriptorIdentity> selected_overlay_slots;
    std::map<std::pair<std::uint64_t, std::uint64_t>, OverlayDescriptorIdentity> hidden_overlay_slots;
    using ProbeKey = std::pair<std::uint64_t, std::uint64_t>;
    using ProbeSlots = std::map<ProbeKey, std::uint64_t>;
    using ProbeOrdinals = std::map<ProbeSlots::key_type, std::size_t>;
    using ProbeFrames = std::map<ProbeKey, std::vector<std::uint64_t>>;
    ProbeSlots rendered_probe_slots;
    ProbeSlots transition_probe_slots;
    ProbeOrdinals rendered_probe_ordinals;
    ProbeOrdinals transition_probe_ordinals;
    ProbeFrames rendered_probe_frames;
    bool donor_descriptors = false;
    void reject_causal_evidence(const std::string_view reason) noexcept;
    void reconcile_incremental_publication(const std::uint64_t generation);
    void reconcile_rendered_probes(const std::uint64_t generation);
    void record_frame(const std::uint64_t generation, const std::uint64_t revision, const std::size_t publication_ordinal);
    void record_tile_publication(const std::uint64_t generation, const std::uint64_t count, const std::size_t publication_ordinal);
    void join_gallery_publication(const std::uint64_t generation, const std::map<std::uint64_t, std::uint64_t>& complete_slots);
    void record_probe(ProbeSlots& probes, ProbeOrdinals& ordinals, const ProbeSlots::key_type& key, const ProbeSlots::mapped_type compiled_index,
                      const bool valid, const std::string_view failure);
    void record_card_geometry(const std::uint64_t generation, const std::uint64_t slot, const std::uint64_t compiled_index, const std::uint64_t card_width,
                              const std::uint64_t card_height, const std::uint64_t content_x, const std::uint64_t content_y, const std::uint64_t content_width,
                              const std::uint64_t content_height);
    void consume_explore_evidence(const char* const event, const std::uint64_t value, const std::uint64_t detail = 0U, const std::uint64_t staging_bytes = 0U);
    void consume(const nlohmann::json& record);
    [[nodiscard]] std::optional<FinalCursorGenerations> final_generations_for(const std::map<std::uint64_t, std::uint64_t>& rendered_slots,
                                                                              const std::uint64_t generation,
                                                                              const std::uint64_t frame_revision) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> generation_for(const std::map<std::uint64_t, std::uint64_t>& rendered_slots) const noexcept;
    [[nodiscard]] bool held_stale_read_discarded() const noexcept;
    [[nodiscard]] bool stale_thumbnail_discarded() const noexcept;
    [[nodiscard]] bool superseding_placeholder_observed() const noexcept;
    void RecordHeldControlObservation(const ExploreAcceptanceGate::ControlObservation observation) noexcept;
    [[nodiscard]] std::string_view causal_stale_blocker(const FinalCursorGenerations final) const noexcept;
    [[nodiscard]] bool causal_stale_chain(const FinalCursorGenerations final) const noexcept;
    [[nodiscard]] bool exact_partial_slot_identity() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> augmentation_generation_for(const std::uint64_t seed, const std::uint64_t frame_revision) const noexcept;
    [[nodiscard]] bool augmentation_pixels_observed(const std::uint64_t seed) const noexcept;
    [[nodiscard]] bool aligned_rendered_probe(const std::pair<std::uint64_t, std::uint64_t> key) const noexcept;
    [[nodiscard]] bool filtered_overlay_identity() const noexcept;
    [[nodiscard]] bool aligned_padding_orientation(const PaddingOrientation orientation) const noexcept;
    [[nodiscard]] bool aligned_overlay_pixels() const noexcept;
    [[nodiscard]] std::string_view overlay_readiness_blocker(const bool require_pixel_probes) const noexcept;
    [[nodiscard]] std::string_view readiness_blocker(const FinalCursorGenerations final, const bool seeded_augmentation_ready,
                                                     const bool require_overlay_pixel_probes) const noexcept;
    [[nodiscard]] bool product_completed(const FinalCursorGenerations final, const bool seeded_augmentation_ready,
                                         const bool require_overlay_pixel_probes) const noexcept;
    [[nodiscard]] bool product_ready(const FinalCursorGenerations final, const bool seeded_augmentation_ready,
                                     const bool require_overlay_pixel_probes) const noexcept;
    [[nodiscard]] bool failed_before_termination() const noexcept;
    [[nodiscard]] std::string_view failure_blocker() const noexcept;
    [[nodiscard]] bool active_peer() const noexcept;
};
// Acceptance consumes bounded records only when lifecycle reporting is enabled.
// Retain one offending record and its causal context, independently of verdicts.

} // namespace mmltk::acceptance::wayland
