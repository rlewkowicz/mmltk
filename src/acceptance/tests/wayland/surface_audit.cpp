#include "audit_facts.h"
#include "src/controller/contracts/diagnostic_context.h"
#include <charconv>
#include <system_error>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include "surface_audit.h"
namespace mmltk::acceptance::wayland {
auto SurfaceAudit::source_transition(const std::string& id, const std::string_view event, const bool browser, const std::uint64_t code) -> void {
    if (!valid_identity(id) || (!sources.contains(id) && sources.size() == kAcceptanceGenerationLimit)) {
        reject("source admission evidence has invalid identity or exceeds capacity");
        return;
    }
    auto& source = sources[id];
    auto& lifecycle = browser ? source.browser : source.native;
    unsigned next = 0U;
    if (event.ends_with("admitted") || event.ends_with("admission.enqueued"))
        next = 1U;
    else if (event.ends_with("claim_outcome") || event.ends_with("admission.written"))
        next = 2U;
    else if (event.ends_with("ready"))
        next = 3U;
    else if (event.ends_with("withdrawal"))
        next = 4U;
    else if (event.ends_with("retired") || event.ends_with("retirement"))
        next = 5U;
    else if (event.ends_with("import_failed")) {
        if (code == 1U && lifecycle.Cancel()) return;
        source.failed = true;
        reject("source import failed without a valid withdrawal");
        return;
    } else
        return;
    if (!browser && next == 5U && std::ranges::any_of(surfaces, [&](const auto& surface) {
            return std::ranges::any_of(surface.second.transfers, [&](const auto& transfer) {
                return transfer.second.read.source == id && transfer.second.terminal == TerminalRead::Retained;
            });
        })) {
        reject("physical source retirement contradicts installed terminal custody");
        return;
    }
    // Withdrawal closes admission while an already claimed import may
    // still finish. Physical retirement is the terminal boundary.
    if (!lifecycle.Observe(next, native_shutdown && !browser)) reject("source admission or retirement is missing, duplicate, or reordered");
}
auto SurfaceAudit::reject(const std::string_view why) -> void {
    if (failure.empty()) failure = why;
}
auto SurfaceAudit::admit(const std::string& id) -> bool {
    if (!failure.empty()) return false;
    const auto existing = surfaces.find(id);
    if (existing == surfaces.end()) {
        if (surfaces.size() < kAcceptanceGenerationLimit) return true;
    } else {
        const auto& state = existing->second;
        if (state.source_steps.size() < kAcceptanceRecordLimit && state.publication_steps.size() < kAcceptanceRecordLimit &&
            state.reads.size() < kAcceptanceRecordLimit && state.transfers.size() < kAcceptanceRecordLimit && state.receipts.size() < kAcceptanceRecordLimit &&
            state.ended_spans.size() < kAcceptanceRecordLimit && state.samples.size() < kAcceptanceRecordLimit &&
            state.failed_source_operations.size() < kAcceptanceRecordLimit && state.failed_publications.size() < kAcceptanceRecordLimit &&
            draws.size() < kAcceptanceRecordLimit)
            return true;
    }
    reject("physical lifecycle evidence exceeded its bounded acceptance capacity");
    return false;
}
auto SurfaceAudit::native_identity(const nlohmann::json& record, const bool workspace) -> std::string {
    std::string result(32U, '0');
    for (const auto& [offset, field] : {std::pair{0U, "surface_high"}, std::pair{16U, "surface_low"}}) {
        std::array<char, 16U> digits{};
        const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(),
                                             scalar(record, workspace ? (offset == 0U ? "workspace_source_high" : "workspace_source_low") : field), 16);
        const auto count = static_cast<std::size_t>(converted.ptr - digits.data());
        result.replace(offset + 16U - count, count, digits.data(), count);
    }
    return result;
}
auto SurfaceAudit::valid_identity(const std::string_view id) -> bool {
    return id.size() == 32U && id != std::string(32U, '0') &&
           std::ranges::all_of(id, [](const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}
auto SurfaceAudit::metadata_receipt(const nlohmann::json& record) -> std::optional<MetadataReceipt> {
    const auto bytes = scalar(record, "metadata_bytes");
    const std::string fingerprint = record.value("metadata_fingerprint", "");
    if (bytes == 0U || fingerprint.size() != 16U ||
        !std::ranges::all_of(fingerprint, [](const char digit) { return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'); })) {
        reject("image metadata receipt lacks its byte extent or fingerprint");
        return {};
    }
    return MetadataReceipt{bytes, fingerprint};
}
auto SurfaceAudit::valid_transfer_timeline(const nlohmann::json& record, const std::uint64_t transfer) -> bool {
    return transfer != 0U && transfer <= std::numeric_limits<std::uint64_t>::max() / 2U && scalar(record, "timeline_ready") == transfer * 2U - 1U &&
           scalar(record, "timeline_release") == transfer * 2U;
}
auto SurfaceAudit::native(const nlohmann::json& record) -> void {
    const std::string event = record.value("event", "");
    if (event.starts_with("presentation.copy.")) reject("Presentation performed an obsolete pixel copy");
    native_shutdown = native_shutdown || event == "shutdown.requested";
    browser_shutdown = browser_shutdown || event == "shutdown.firefox_terminal";
    browser_exited = browser_exited || event == "shutdown.firefox_terminal";
    if (event.starts_with("presentation.source.admission.") || event == "presentation.source.ready" || event == "presentation.source.withdrawal" ||
        event == "presentation.source.retirement") {
        if (scalar(record, "outcome") != 1U) reject("native source admission or retirement failed");
        const auto id = native_identity(record);
        source_transition(id, event, false);
        if (!failure.empty()) return;
        auto& source = sources.at(id);
        const auto generation = scalar(record, "sequence");
        const auto width = scalar(record, "capacity_width");
        const auto height = scalar(record, "capacity_height");
        const auto allocation = scalar(record, "workspace_allocation");
        if (generation == 0U || width == 0U || height == 0U || allocation == 0U || native_identity(record, true) != id ||
            (source.generation != 0U && (source.generation != generation || source.width != width || source.height != height ||
                                         source.allocation != allocation || source.direct_sampling != record.value("direct_sampling", false))))
            reject("native source admission has missing or inconsistent physical provenance");
        source.generation = generation;
        source.width = width;
        source.height = height;
        const auto bytes = scalar(record, "workspace_bytes"), pitch = scalar(record, "workspace_pitch");
        if (width > pitch / 4U || height == 0U || bytes / height < pitch || scalar(record, "workspace_width") != width ||
            scalar(record, "workspace_height") != height || (source.bytes != 0U && (source.bytes != bytes || source.pitch != pitch)))
            reject("native physical allocation inventory is missing or inconsistent");
        source.bytes = bytes;
        source.pitch = pitch;
        source.allocation = allocation;
        source.direct_sampling = record.value("direct_sampling", false);
        return;
    }
    constexpr std::array transitions{"presentation.arena.advertised",
                                     "presentation.admission.enqueued",
                                     "presentation.admission.written",
                                     "presentation.import.outcome",
                                     "presentation.source_borrow.started",
                                     "presentation.source_borrow.completed",
                                     "presentation.source.read_submitted",
                                     "presentation.ready_sync.started",
                                     "presentation.ready_sync.completed",
                                     "presentation.frame.edge",
                                     "presentation.active.withdrawal",
                                     "presentation.candidate.withdrawal",
                                     "presentation.retirement",
                                     "presentation.release_wait.started",
                                     "presentation.release_wait.completed",
                                     "presentation.terminal_read.completed",
                                     "presentation.terminal_read.retained",
                                     "presentation.replacement"};
    if (record.value("kind", "") != "gui_runtime" || std::ranges::find(transitions, event) == transitions.end()) return;
    const auto id = native_identity(record);
    if (!record.contains("surface_high") || !record.contains("surface_low") || !valid_identity(id)) {
        reject("native lifecycle record has no physical identity");
        return;
    }
    if (!admit(id)) return;
    auto& state = surfaces[id];
    const auto generation = scalar(record, "sequence");
    const auto width = scalar(record, "capacity_width");
    const auto height = scalar(record, "capacity_height");
    if (generation == 0U || width == 0U || height == 0U || scalar(record, "selection_generation") == 0U || scalar(record, "frame_revision") == 0U ||
        !record.contains("condition") || !record.contains("outcome") ||
        (state.generation != 0U && (state.generation != generation || state.width != width || state.height != height)))
        reject("native surface provenance is missing or inconsistent");
    state.generation = generation;
    state.width = width;
    state.height = height;
    const bool copying =
        event == "presentation.source_borrow.started" || event == "presentation.source_borrow.completed" || event == "presentation.source.read_submitted";
    const bool terminal_read = event == "presentation.terminal_read.completed" || event == "presentation.terminal_read.retained";
    const bool source_release = event == "presentation.release_wait.started" || event == "presentation.release_wait.completed" || terminal_read;
    const bool transferred =
        event == "presentation.ready_sync.started" || event == "presentation.ready_sync.completed" || event == "presentation.frame.edge" || source_release;
    if (copying || transferred) {
        if (scalar(record, "source_revision") != scalar(record, "frame_revision") || scalar(record, "allocation_generation") != generation ||
            scalar(record, "presentation_revision") != scalar(record, "value"))
            reject("native operation mixes source allocation or publication identities");
        if (copying && (scalar(record, "presentation_revision") != 0U || scalar(record, "transfer_sequence") != 0U || scalar(record, "timeline_ready") != 0U))
            reject("new native source read inherited an incumbent physical publication");
        if (transferred && (scalar(record, "transfer_sequence") == 0U || scalar(record, "timeline_ready") != scalar(record, "transfer_sequence") * 2U - 1U))
            reject("native physical operation omitted its transfer identity");
    }
    if (source_release) {
        const auto source = sources.find(native_identity(record, true));
        if (source == sources.end() || !source->second.native.ready() || source->second.native.retired())
            reject("native release settlement has no admitted physical source");
        else if (source->second.direct_sampling != record.value("direct_sampling", false))
            reject("native release settlement changed the physical source mode");
    } else if (state.native_retired) {
        reject("native arena transition after physical retirement");
    }
    const auto advance = [&](const unsigned next) {
        if (state.native_stage + 1U != next)
            reject("native admission stages are missing, duplicate, or reordered");
        else
            state.native_stage = next;
    };
    if (event == "presentation.arena.advertised")
        advance(1U);
    else if (event == "presentation.admission.enqueued")
        advance(2U);
    else if (event == "presentation.admission.written") {
        advance(3U);
    } else if (event == "presentation.import.outcome") {
        advance(4U);
        state.import_failed = scalar(record, "value") != 1U;
    } else if (event == "presentation.active.withdrawal" || event == "presentation.candidate.withdrawal") {
        if (state.native_stage != 4U || state.import_failed || state.withdrawn) reject("withdrawal lacks a unique completed import");
        state.withdrawn = true;
        state.candidate_withdrawn = event == "presentation.candidate.withdrawal";
    } else if (event == "presentation.retirement") {
        if (scalar(record, "outcome") != 1U) reject("native physical retirement failed");
        state.native_retired = true;
    } else {
        const bool span_end =
            event == "presentation.source_borrow.completed" || event == "presentation.ready_sync.completed" || event == "presentation.release_wait.completed";
        if (span_end) {
            const auto span_outcome = scalar(record, "span_outcome");
            state.ended_spans.insert_or_assign(scalar(record, "span_id"), span_outcome);
            if (span_outcome != static_cast<std::uint64_t>(mmltk::controller::contracts::DiagnosticSpanOutcome::Success) || scalar(record, "outcome") != 1U) {
                if (event == "presentation.source_borrow.completed")
                    state.failed_source_operations.insert(scalar(record, "trace_id"));
                else if (event == "presentation.ready_sync.completed")
                    state.failed_publications.emplace(scalar(record, "value"), scalar(record, "transfer_sequence"));
                return;
            }
        }
        if (event == "presentation.source.read_submitted" && scalar(record, "outcome") != 0U) return;
        const auto source_operation = scalar(record, "trace_id");
        const auto publication_key = std::pair{scalar(record, "value"), scalar(record, "transfer_sequence")};
        auto& steps = state.source_steps[source_operation];
        const auto observe = [&](unsigned& stage, const unsigned next) {
            if (state.native_stage != 4U || state.import_failed || stage + 1U != next)
                reject("native source/read/ready stages are incomplete or reordered");
            else
                stage = next;
        };
        if (event == "presentation.source_borrow.started") {
            if (state.failed_source_operations.erase(source_operation) != 0U) steps = 0U;
            observe(steps, 1U);
        } else if (event == "presentation.source_borrow.completed")
            observe(steps, 2U);
        else if (event == "presentation.source.read_submitted") {
            if (state.failed_source_operations.contains(source_operation))
                reject("native source read followed a failed borrow");
            else {
                observe(steps, 3U);
                const auto source = native_identity(record, true);
                const auto admitted = sources.find(source);
                const SourceRead read{source,
                                      scalar(record, "workspace_allocation"),
                                      scalar(record, "source_session"),
                                      scalar(record, "source_revision"),
                                      scalar(record, "source_width"),
                                      scalar(record, "source_height")};
                if (admitted == sources.end() || !admitted->second.native.live() || read.allocation == 0U || admitted->second.allocation != read.allocation ||
                    read.session == 0U || read.frame == 0U || read.width == 0U || read.height == 0U || read.width > admitted->second.width ||
                    read.height > admitted->second.height)
                    reject("source read does not name its admitted workspace allocation");
                if (!state.reads.emplace(source_operation, read).second) reject("source read repeats an already submitted operation");
            }
        } else if (event == "presentation.ready_sync.started") {
            if (steps != 3U) reject("native ready synchronization lacks its submitted source read");
            auto& publication = state.publication_steps[publication_key];
            if (state.failed_publications.erase(publication_key) != 0U) publication = 0U;
            observe(publication, 1U);
            const auto read = state.reads.find(source_operation);
            if (read == state.reads.end() || !state.transfers.emplace(publication_key, TransferReceipt{read->second}).second)
                reject("physical publication lacks a unique source read");
        } else if (event == "presentation.ready_sync.completed") {
            auto& publication = state.publication_steps[publication_key];
            observe(publication, 2U);
        } else if (event == "presentation.frame.edge") {
            auto& publication = state.publication_steps[publication_key];
            if (state.failed_publications.contains(publication_key)) {
                reject("native frame publication followed a failed ready synchronization");
            } else {
                observe(publication, 3U);
                const auto publication_revision = scalar(record, "value");
                const auto frame_revision = scalar(record, "frame_revision");
                const auto [existing, inserted] = state.publications.emplace(publication_revision, frame_revision);
                if (!inserted && existing->second != frame_revision) reject("native frame publication changed within one publication revision");
            }
        }
        if (transferred) {
            const auto receipt = state.transfers.find(publication_key);
            const SourceRead observed{native_identity(record, true),     scalar(record, "workspace_allocation"), scalar(record, "source_session"),
                                      scalar(record, "source_revision"), scalar(record, "source_width"),         scalar(record, "source_height")};
            if (receipt == state.transfers.end() || receipt->second.read != observed)
                reject("physical publication changed source read provenance");
            else if (event == "presentation.frame.edge") {
                receipt->second.metadata = metadata_receipt(record);
            } else if (event == "presentation.release_wait.started") {
                if (receipt->second.releasing || receipt->second.terminal == TerminalRead::Retained || state.publication_steps.at(publication_key) != 3U)
                    reject("source release wait lacks its unique published transfer");
                receipt->second.releasing = true;
            } else if (event == "presentation.release_wait.completed") {
                if (!receipt->second.releasing || receipt->second.released || receipt->second.terminal == TerminalRead::Retained)
                    reject("source release wait is missing, duplicated, or contradicts installed terminal custody");
                receipt->second.released = true;
            } else if (terminal_read) {
                if (!native_shutdown || !browser_exited || scalar(record, "outcome") != 1U || receipt->second.terminal != TerminalRead::None ||
                    receipt->second.released || state.publication_steps.at(publication_key) != 3U || !sources.contains(observed.source) ||
                    record.value("direct_sampling", false) != sources.at(observed.source).direct_sampling) {
                    reject("terminal read outcome lacks its exact live read and completed browser boundary");
                } else {
                    receipt->second.terminal = event == "presentation.terminal_read.completed" ? TerminalRead::Completed : TerminalRead::Retained;
                }
            }
        }
    }
}
auto SurfaceAudit::browser(const nlohmann::json& record) -> void {
    ++browser_ordinal;
    const std::string event = record.value("event", "");
    if (event == "browser.invalid_webgpu_texture") reject("Firefox reported an invalid WebGPU texture");
    browser_shutdown = browser_shutdown || (event == "firefox.workspace.channel_terminal" && record.value("terminal", "") == "orderly_bridge_close");
    // An unavailable draw has no acquired image or physical transition.
    // BrowserAudit separately checks missing draws after owned atlas content.
    if (event == "iced.surface.sample_draw_missing") return;
    if (event.starts_with("firefox.workspace.source.")) {
        if (event.ends_with("claim_outcome") && record.value("outcome", "") != "claimed") reject("Firefox failed to claim an admitted source");
        const std::string id = record.value("surface", "");
        source_transition(id, event, true, scalar(record, "code"));
        if (event == "firefox.workspace.source.admitted" && failure.empty()) {
            auto& source = sources.at(id);
            source.browser_width = scalar(record, "width");
            source.browser_height = scalar(record, "height");
            source.browser_allocation = scalar(record, "workspace_allocation");
            source.arena = record.value("arena", "");
            source.browser_direct_sampling = record.value("direct_sampling", false);
            if (source.browser_width == 0U || source.browser_height == 0U || source.browser_allocation == 0U || !valid_identity(source.arena))
                reject("Firefox source admission omitted its allocation or arena");
        }
        return;
    }
    if (event.find("capture") != std::string::npos && event.starts_with("iced.surface.")) reject("Iced performed an obsolete capture pass");
    if (!event.starts_with("firefox.workspace.") && !event.starts_with("iced.surface.") && !event.starts_with("iced.frame.")) return;
    if (!record.contains("surface")) {
        if (event.starts_with("iced.surface.") || event == "firefox.workspace.admitted" || event == "firefox.workspace.claim_outcome" ||
            event == "firefox.workspace.registry_inserted" || event == "firefox.workspace.import_ready_emitted" || event == "firefox.workspace.ready" ||
            event == "firefox.workspace.withdrawal" || event == "firefox.workspace.retired")
            reject("browser lifecycle record has no physical identity");
        return;
    }
    const std::string id = record.value("surface", "");
    if (!valid_identity(id)) {
        reject("browser lifecycle record has malformed physical identity");
        return;
    }
    if (!admit(id)) return;
    auto& state = surfaces[id];
    if ((event == "firefox.workspace.admitted" || event == "firefox.workspace.claim_outcome") && (!record.contains("width") || !record.contains("height")))
        reject("Firefox admission or claim has no dimensions");
    if (record.contains("width")) {
        const auto width = scalar(record, "width");
        const auto height = scalar(record, "height");
        if (width == 0U || height == 0U || (state.browser_width != 0U && (state.browser_width != width || state.browser_height != height)))
            reject("browser dimensions are missing or inconsistent");
        state.browser_width = width;
        state.browser_height = height;
    }
    const auto advance = [&](const unsigned from) {
        if (state.firefox_stage != from) reject("Firefox admission stages are missing, duplicate, or reordered");
        state.firefox_stage = from + 1U;
    };
    if (event == "firefox.workspace.admitted") {
        advance(0U);
        state.admitted = browser_ordinal;
    } else if (event == "firefox.workspace.claim_outcome" && record.value("outcome", "") == "claimed")
        advance(1U);
    else if (event == "firefox.workspace.claim_outcome")
        reject("Firefox failed to claim the advertised surface");
    else if (event == "firefox.workspace.import_failed") {
        if (state.firefox_stage != 2U || state.firefox_import_failed || state.firefox_withdrawn || state.firefox_retired)
            reject("Firefox import failure lacks its unique claimed surface");
        state.firefox_import_failed = true;
    } else if (event == "firefox.workspace.registry_inserted")
        advance(2U);
    else if (event == "firefox.workspace.import_ready_emitted")
        advance(3U);
    else if (event == "firefox.workspace.ready")
        advance(4U);
    else if (event == "firefox.workspace.withdrawal" || event == "firefox.workspace.drop_received") {
        if (state.firefox_stage == 0U || state.firefox_retired) reject("Firefox withdrawal lacks a live admitted arena");
        if (state.firefox_withdrawn == 0U) state.firefox_withdrawn = browser_ordinal;
    } else if (event == "firefox.workspace.retired") {
        if (!state.firefox_withdrawn || state.firefox_retired) reject("Firefox retirement lacks its unique withdrawal");
        state.firefox_retired = browser_ordinal;
    }
    if (event == "firefox.workspace.frame_released") { reject("obsolete offer-driven release has no acquired physical read"); }
    if (event == "firefox.workspace.release_only_submitted" || event == "firefox.workspace.release_only_completed") {
        const auto publication = scalar(record, "presentation_revision");
        const auto transfer = scalar(record, "transfer_sequence");
        const auto session = scalar(record, "content_session");
        const auto frame = scalar(record, "content_sequence");
        const std::string source_id = record.value("source", "");
        const auto source = sources.find(source_id);
        if (publication == 0U || session == 0U || frame == 0U || !valid_transfer_timeline(record, transfer) || record.contains("layer") ||
            record.contains("slot") || source == sources.end() || source->second.arena != id) {
            reject("release-only settlement lacks its exact physical source and timeline");
            return;
        }
        auto& receipt = state.receipts[publication];
        if (event == "firefox.workspace.release_only_submitted") {
            if (receipt.stage != 0U || !source->second.browser.live()) {
                reject("release-only submission duplicates or replaces an existing physical transfer");
                return;
            }
            receipt = {.source = source_id,
                       .transfer = transfer,
                       .session = session,
                       .frame = frame,
                       .stage = 2U,
                       .direct_sampling = source->second.browser_direct_sampling,
                       .release_only = true};
        } else if (!receipt.release_only || receipt.stage != 2U || receipt.source != source_id || receipt.transfer != transfer || receipt.session != session ||
                   receipt.frame != frame) {
            reject("release-only completion lacks its unique submitted physical transfer");
        } else {
            receipt.stage = 3U;
        }
        return;
    }
    if (event == "firefox.workspace.frame_forwarded" || event == "firefox.workspace.frame_dispatched" || event == "firefox.workspace.copy_completed" ||
        event == "firefox.workspace.read_settled") {
        const auto publication = scalar(record, "presentation_revision");
        const ReceiverReceipt observed{record.value("source", ""),
                                       scalar(record, "transfer_sequence"),
                                       scalar(record, "layer"),
                                       scalar(record, "slot"),
                                       scalar(record, "content_session"),
                                       scalar(record, "content_sequence"),
                                       scalar(record, "content_width"),
                                       scalar(record, "content_height"),
                                       0U,
                                       record.value("direct_sampling", false)};
        if (publication == 0U || observed.session == 0U || observed.frame == 0U || observed.width == 0U || observed.height == 0U || !record.contains("layer") ||
            !record.contains("slot") || observed.layer != 0U || observed.slot >= 2U)
            reject("receiver physical receipt has missing or invalid identity");
        auto& receipt = state.receipts[publication];
        if (event == "firefox.workspace.frame_forwarded") {
            const auto source = sources.find(observed.source);
            if (receipt.stage != 0U || observed.transfer == 0U || source == sources.end() || !source->second.browser.live() || source->second.arena != id ||
                observed.width > source->second.browser_width || observed.direct_sampling != source->second.browser_direct_sampling ||
                observed.height > source->second.browser_height || !valid_transfer_timeline(record, observed.transfer))
                reject("Firefox forwarding lacks its exact admitted source transfer");
            receipt = observed;
            receipt.stage = 1U;
        } else {
            const unsigned required = event == "firefox.workspace.frame_dispatched" ? 1U : 2U;
            if (receipt.release_only || receipt.stage != required || receipt.layer != observed.layer || receipt.slot != observed.slot ||
                receipt.session != observed.session || receipt.frame != observed.frame || receipt.width != observed.width ||
                receipt.height != observed.height || receipt.source != observed.source || receipt.transfer != observed.transfer ||
                receipt.direct_sampling != observed.direct_sampling || (event == "firefox.workspace.copy_completed" && receipt.direct_sampling) ||
                (event == "firefox.workspace.read_settled" && !receipt.direct_sampling))
                reject("receiver dispatch or copy completion is missing, duplicate, reordered, or mismatched");
            else
                ++receipt.stage;
        }
    }
    if (event == "iced.surface.draw_encoded" || event == "iced.frame.draw_settled" || event == "iced.frame.draw_abandoned" ||
        event == "iced.frame.sample_released") {
        const auto publication = scalar(record, "presentation_revision");
        const auto receipt = state.receipts.find(publication);
        if (receipt == state.receipts.end() || receipt->second.release_only || receipt->second.stage < 2U ||
            scalar(record, "frame_revision") != receipt->second.frame || scalar(record, "content_session") != receipt->second.session ||
            !record.contains("slot") || scalar(record, "slot") != receipt->second.slot || !record.contains("layer") ||
            scalar(record, "layer") != receipt->second.layer || scalar(record, "content_width") != receipt->second.width ||
            scalar(record, "content_height") != receipt->second.height) {
            reject("Iced reader receipt lacks its exact dispatched sample");
            return;
        }
        auto& custody = state.custody[publication];
        if (custody.released || state.retired != 0U) {
            reject("Iced reader used or released an already retired sample");
        } else if (event == "iced.surface.draw_encoded") {
            if (!custody.acquired || custody.encoded >= custody.selected)
                reject("Iced encoding lacks selection of its acquired image");
            else {
                const auto identity = scalar(record, "draw_identity");
                if (draws.empty() || draws.back().selected != id || draws.back().requested != record.value("requested_surface", "") ||
                    draws.back().publication != publication || draws.back().encoded != 0U || identity == 0U ||
                    !state.draw_indices.emplace(identity, draws.size() - 1U).second)
                    reject("Iced encoding lacks its exact selected draw");
                else {
                    ++custody.encoded;
                    draws.back().encoded = browser_ordinal;
                    draws.back().identity = identity;
                }
            }
        } else if (event == "iced.frame.sample_released") {
            if (custody.encoded != custody.settled + custody.abandoned) reject("Iced final sample release preceded encoded reader settlement");
            custody.released = true;
        } else if (custody.settled + custody.abandoned >= custody.encoded) {
            reject("Iced batch settlement is missing, duplicate, or reordered");
        } else {
            const auto found = state.draw_indices.find(scalar(record, "draw_identity"));
            if (found == state.draw_indices.end()) {
                reject("Iced settlement lacks its exact encoded draw");
                return;
            }
            auto& draw = draws[found->second];
            if (draw.publication != publication || draw.requested != record.value("requested_surface", "") || draw.settled != 0U) {
                reject("Iced settlement duplicates or changes its encoded draw");
                return;
            }
            draw.settled = browser_ordinal;
            draw.abandoned = event == "iced.frame.draw_abandoned";
            if (draw.abandoned)
                ++custody.abandoned;
            else
                ++custody.settled;
        }
    }
    if (!event.starts_with("iced.surface.")) return;
    if (!record.contains("width") || !record.contains("height")) reject("Iced surface provenance is missing or inconsistent");
    if (event == "iced.surface.draw_submitted") {
        const auto publication = scalar(record, "presentation_revision");
        const std::string requested = record.value("requested_surface", "");
        const auto sample = state.samples.find(publication);
        const auto custody = state.custody.find(publication);
        const auto found = state.draw_indices.find(scalar(record, "draw_identity"));
        auto* draw = found != state.draw_indices.end() ? &draws[found->second] : nullptr;
        if (!valid_identity(requested) || sample == state.samples.end() || sample->second != scalar(record, "frame_revision") || draw == nullptr ||
            draw->publication != publication || draw->requested != requested || draw->submitted != 0U || draw->settled != 0U || draw->encoded == 0U ||
            draw->encoded >= browser_ordinal || state.retired != 0U || custody == state.custody.end() || custody->second.released ||
            custody->second.settled + custody->second.abandoned >= custody->second.encoded)
            reject("Iced submission lacks its exact prior encoded draw");
        else
            draw->submitted = browser_ordinal;
    } else if (event == "iced.surface.source_texture_create" || event == "iced.surface.source_texture_destroyed") {
        const std::string source = record.value("source", "");
        if (!valid_identity(source) || !record.value("direct_sampling", false)) {
            reject("direct texture wrapper lacks its exact source and mode");
            return;
        }
        auto& count = state.source_textures[source];
        if (event == "iced.surface.source_texture_create") {
            if (count != 0U) reject("direct physical source wrapped twice concurrently");
            ++count;
        } else {
            if (count != 1U) reject("direct source texture retirement lacks its wrapper");
            for (const auto& [publication, custody] : state.custody) {
                const auto receipt = state.receipts.find(publication);
                if (receipt != state.receipts.end() && receipt->second.source == source &&
                    ((custody.acquired && !custody.released) || custody.encoded != custody.settled + custody.abandoned))
                    reject("direct source texture destruction preceded its exact readers");
            }
            count = 0U;
        }
    } else if (event == "iced.surface.texture_create") {
        if (state.created != 0U || state.retired != 0U) reject("Iced physical texture created twice");
        state.created = browser_ordinal;
    } else if (event == "iced.surface.sample_acquired" || event == "iced.surface.sample_draw_selected") {
        if (state.retired != 0U)
            reject("Iced surface used after texture retirement");
        else if (state.created == 0U || state.firefox_stage != 5U)
            reject("Iced sampled surface lacks texture or complete Firefox import");
        const auto publication = scalar(record, "presentation_revision");
        if (state.samples.size() >= kAcceptanceRecordLimit && !state.samples.contains(publication)) {
            reject("Iced sample identity inventory exhausted");
            return;
        }
        const auto [sample, inserted] = state.samples.emplace(publication, scalar(record, "frame_revision"));
        if (!inserted && sample->second != scalar(record, "frame_revision")) reject("sample publication changed content identity");
        auto& custody = state.custody[scalar(record, "presentation_revision")];
        if (custody.released) reject("Iced reacquired a released sample");
        if (event == "iced.surface.sample_acquired") {
            if (custody.acquired) reject("Iced acquired the same publication twice");
            const auto receipt = state.receipts.find(publication);
            if (receipt == state.receipts.end() || receipt->second.release_only || receipt->second.stage < 2U ||
                receipt->second.source != record.value("source", "") || receipt->second.transfer != scalar(record, "transfer_sequence"))
                reject("Iced metadata acquisition lacks its exact dispatched physical transfer");
            custody.metadata = metadata_receipt(record);
            custody.acquired = true;
            custody.acquired_at = browser_ordinal;
            state.acquired = browser_ordinal;
        } else {
            if (!custody.acquired) reject("Iced selected image without an exact sample lease");
            ++custody.selected;
            if (!valid_identity(record.value("requested_surface", ""))) reject("Iced selected draw lacks requested capability identity");
            if (draws.size() >= kAcceptanceRecordLimit) {
                reject("Iced draw identity inventory exhausted");
                return;
            }
            draws.push_back({id, record.value("requested_surface", ""), publication, browser_ordinal});
        }
    } else if (event == "iced.surface.pending_discarded") {
        if (state.created == 0U || state.discarded != 0U) reject("Iced pending discard does not name a unique live import");
        state.discarded = browser_ordinal;
    } else if (event == "iced.surface.renderer_reconstructed") {
        const std::string requested = record.value("requested_surface", "");
        const auto retained = state.custody.find(scalar(record, "presentation_revision"));
        if (state.retired != 0U)
            reject("Iced surface used after texture retirement");
        else if (!valid_identity(requested) || requested == id || state.acquired == 0U || retained == state.custody.end() || !retained->second.acquired ||
                 retained->second.released)
            reject("pipeline reconstruction lacks its retained sample");
        else {
            if (!admit(requested)) return;
            auto& pending = surfaces[requested];
            const auto preparation = pending.admitted;
            if (pending.reconstruction || preparation == 0U || retained->second.acquired_at >= browser_ordinal || pending.acquired != 0U ||
                pending.discarded != 0U || pending.retired != 0U)
                reject("renderer reconstruction is duplicate or outside its exact pending ownership window");
            else
                pending.reconstruction = Reconstruction{id, requested, scalar(record, "presentation_revision"), browser_ordinal};
        }
    } else if (event == "iced.surface.import_dropped") {
        if (state.created == 0U || state.import_dropped != 0U) reject("Iced import drop lacks its unique owner");
        state.import_dropped = browser_ordinal;
    } else if (event == "iced.surface.texture_destroyed") {
        if (state.created == 0U || state.retired != 0U) reject("Iced retirement lacks texture creation");
        for (const auto& [publication, custody] : state.custody) {
            if ((custody.acquired && !custody.released) || custody.selected != custody.encoded || custody.encoded != custody.settled + custody.abandoned)
                reject("Iced texture destruction preceded its exact final readers");
        }
        if (std::ranges::any_of(state.source_textures, [](const auto& source) { return source.second != 0U; }))
            reject("logical texture retirement preceded physical source wrappers");
        state.retired = browser_ordinal;
    }
}
auto SurfaceAudit::source_joined(const SourceState& source) const -> bool {
    // A withdrawn native request consumes its terminal acknowledgement
    // without installing a timeline. The receiver still proves whether
    // initialization completed or the unclaimed import was cancelled.
    const bool initialized = (source.native.ready() || source.native.retired()) && source.browser.ready();
    const bool cancelled = source.native.retired() && source.browser.retired() && source.browser.cancelled() && !source.native.ready();
    return !source.failed && (initialized || cancelled) && source.generation != 0U && source.width == source.browser_width &&
           source.height == source.browser_height && source.allocation != 0U && source.allocation == source.browser_allocation &&
           source.direct_sampling == source.browser_direct_sampling && valid_identity(source.arena);
}
auto SurfaceAudit::receipts_joined(const std::string& id, const SurfaceState& state, const bool permit_held) const -> bool {
    const auto terminal_read = [&](const TransferReceipt& transfer) {
        if (!native_shutdown || !browser_exited) return false;
        const auto source = sources.find(transfer.read.source);
        if (source == sources.end() || source->second.allocation != transfer.read.allocation) return false;
        if (transfer.terminal == TerminalRead::Retained) return !transfer.released && !source->second.native.retired();
        return transfer.terminal == TerminalRead::Completed || (transfer.releasing && transfer.released && source->second.native.retired());
    };
    const auto retained_read = [&](const std::uint64_t publication, const ReceiverReceipt& receipt) {
        const auto custody = state.custody.find(publication);
        return permit_held && !receipt.release_only && receipt.direct_sampling && receipt.stage == 2U && custody != state.custody.end() &&
               custody->second.acquired && !custody->second.released;
    };
    for (const auto& [publication, receipt] : state.receipts) {
        const auto transfer = state.transfers.find({publication, receipt.transfer});
        const auto source = sources.find(receipt.source);
        const bool held = retained_read(publication, receipt);
        const bool terminal = transfer != state.transfers.end() && terminal_read(transfer->second);
        if ((!held && !terminal && receipt.stage != 3U) || receipt.stage < 2U || transfer == state.transfers.end() ||
            (!held && !terminal && (!transfer->second.releasing || !transfer->second.released)) || source == sources.end() || !source_joined(source->second) ||
            source->second.arena != id)
            return false;
        const auto& read = transfer->second.read;
        if (read.source != receipt.source || read.allocation != source->second.allocation || read.session != receipt.session || read.frame != receipt.frame ||
            (!receipt.release_only && (read.width != receipt.width || read.height != receipt.height)))
            return false;
    }
    for (const auto& [key, transfer] : state.transfers) {
        if (state.failed_publications.contains(key)) continue;
        const auto copy = state.receipts.find(key.first);
        const bool copied = copy != state.receipts.end() && copy->second.transfer == key.second;
        const bool held = copied && retained_read(key.first, copy->second);
        if (terminal_read(transfer)) {
            if (!copied) return false;
            continue;
        }
        if (!transfer.releasing) {
            if (copied && !held) return false;
            continue;
        }
        if ((!transfer.released && !held) || !copied) return false;
    }
    for (const auto& [publication, custody] : state.custody) {
        if (custody.selected != custody.encoded) return false;
        // A visible retained image keeps drawing while acceptance advances.
        // After positive completed-draw evidence, its live read may own
        // additional GPU submissions. Final release and retirement still
        // require every encoded reader to settle below and in browser().
        const bool live_draw = permit_held && state.retired == 0U && custody.acquired && !custody.released && custody.settled != 0U;
        // Process exit ends the page's remaining readers without delivering
        // JavaScript callbacks. It does not establish a successful draw.
        // A released sample or explicitly destroyed texture still needs
        // every ordinary receipt; bridge closure alone is insufficient.
        if (custody.encoded != custody.settled + custody.abandoned && !live_draw && !(browser_exited && state.retired == 0U)) return false;
        if (state.retired != 0U && custody.acquired && !custody.released) return false;
    }
    for (const auto& [publication, frame] : state.samples) {
        const auto receipt = state.receipts.find(publication);
        const auto transfer = receipt != state.receipts.end() ? state.transfers.find({publication, receipt->second.transfer}) : state.transfers.end();
        const bool terminal = transfer != state.transfers.end() && terminal_read(transfer->second);
        if (receipt == state.receipts.end() || receipt->second.release_only ||
            (receipt->second.stage != 3U && !terminal && !(permit_held && receipt->second.direct_sampling && receipt->second.stage == 2U)) ||
            receipt->second.frame != frame)
            return false;
        const auto custody = state.custody.find(publication);
        if (transfer == state.transfers.end() || custody == state.custody.end() || !transfer->second.metadata || !custody->second.metadata ||
            transfer->second.metadata != custody->second.metadata)
            return false;
    }
    return true;
}
auto SurfaceAudit::joined_failure() const -> std::string {
    if (!failure.empty()) return failure;
    for (const auto& [id, state] : surfaces) {
        if (const auto reason = joined_surface_failure(id, state); !reason.empty()) return reason;
    }
    return {};
}
auto SurfaceAudit::joined_surface_failure(const std::string& id, const SurfaceState& state) const -> std::string {
    if (!receipts_joined(id, state)) return "surface " + id + " lacks exact source, paired metadata, dispatch, or physical completion evidence";
    if (state.created == 0U && state.samples.empty()) {
        // An unclaimed arena advertisement owns no descriptors or GPU images.
        // Native retirement and actual browser exit settle that pending admission;
        // bridge closure alone cannot settle a claimed image or its readers.
        const bool unclaimed_shutdown = native_shutdown && browser_exited && state.native_stage == 3U && state.firefox_stage == 1U;
        if ((!unclaimed_shutdown && (state.native_stage != 4U || state.firefox_stage != 5U)) || state.import_failed || state.firefox_import_failed ||
            state.width != state.browser_width || state.height != state.browser_height)
            return "unsampled arena " + id + " lacks matching native and Firefox admission";
        if (!state.native_retired || (!unclaimed_shutdown && (!state.firefox_retired || !state.firefox_withdrawn || !state.withdrawn)))
            return "unsampled arena " + id + " lacks withdrawal and physical retirement";
        const bool released_without_sampling =
            !state.publications.empty() && state.publications.size() == state.receipts.size() &&
            std::ranges::all_of(state.receipts, [](const auto& receipt) { return receipt.second.release_only && receipt.second.stage == 3U; });
        if ((!released_without_sampling && (!state.publications.empty() || !state.reads.empty() || !state.transfers.empty() || !state.receipts.empty())) ||
            !state.custody.empty() || !state.source_textures.empty())
            return "unsampled arena " + id + " has image custody without a receiver texture";
        return {};
    }
    if (state.firefox_import_failed) {
        const bool native_rejection = state.native_stage == 4U && state.import_failed;
        if (!native_rejection || state.width != state.browser_width || state.height != state.browser_height || !state.native_retired)
            return "rejected import " + id + " lacks matching native admission and retirement";
        if (state.firefox_stage != 2U || state.created == 0U || state.acquired != 0U || state.discarded <= state.created || state.retired <= state.discarded ||
            !state.samples.empty() || state.firefox_withdrawn || state.firefox_retired)
            return "rejected import " + id + " lacks exact receiver discard and texture retirement";
        return {};
    }
    // An exact retained-read fact is emitted only after installing the
    // whole writer in its pre-reserved terminal owner. Its arena resources
    // remain physically retained too; this is not a successful GPU release.
    const bool writer_retained =
        native_shutdown && browser_exited && std::ranges::any_of(surfaces, [](const auto& surface) {
            return std::ranges::any_of(surface.second.transfers, [](const auto& transfer) { return transfer.second.terminal == TerminalRead::Retained; });
        });
    if (state.native_stage != 4U || state.width != state.browser_width || state.height != state.browser_height || (!state.native_retired && !writer_retained))
        return "surface " + id + " lacks matching native/browser import and retirement";
    if (!state.withdrawn && !state.firefox_withdrawn && !state.import_failed && !native_shutdown)
        return "surface " + id + " lacks native or receiver withdrawal before physical retirement";
    if (state.import_failed && state.samples.empty()) return {};
    if (state.import_failed || state.firefox_stage != 5U) return "surface " + id + " sampled or withdrew an incomplete import";
    if ((!state.firefox_retired || state.retired == 0U) && !(native_shutdown && browser_shutdown)) return "surface " + id + " lacks receiver retirement";
    for (const auto& [publication, frame] : state.samples) {
        const auto native = state.publications.find(publication);
        const bool native_match = native != state.publications.end() && native->second == frame;
        if (publication == 0U || frame == 0U || !native_match) return "sampled surface " + id + " lacks matching native frame publication";
    }
    if (state.discarded != 0U && (state.retired == 0U || (!state.withdrawn && !state.firefox_withdrawn)))
        return "discarded pending surface " + id + " lacks withdrawal and texture retirement";
    return {};
}
auto SurfaceAudit::evidence_settled(std::string* blocker) const -> bool {
    const auto incomplete = [&](const std::string_view identity, const std::string_view reason) {
        if (blocker) *blocker = std::string{identity} + ": " + std::string{reason};
        return false;
    };
    if (!failure.empty()) return incomplete("physical lifecycle", failure);
    for (const auto& [id, source] : sources) {
        if (!source_joined(source)) return incomplete(id, "source admission provenance");
        if (source.native.retired() && !source.browser.retired() && !browser_shutdown) return incomplete(id, "browser source retirement");
    }
    for (const auto& [id, state] : surfaces) {
        if (state.native_stage != 4U) return incomplete(id, "native arena admission");
        if (!receipts_joined(id, state, true)) return incomplete(id, "source receipt, paired image metadata, or draw custody");
        if (state.native_retired)
            if (const auto reason = joined_surface_failure(id, state); !reason.empty()) return incomplete(id, reason);
        for (const auto& [publication, frame] : state.samples) {
            const auto produced = state.publications.find(publication);
            if (produced == state.publications.end() || produced->second != frame) return incomplete(id, "sample publication provenance");
        }
        for (const auto& [operation, stage] : state.source_steps)
            // A successfully borrowed source may be superseded before any
            // copy is submitted. Actual publications still require stage 3.
            if (stage == 1U && !state.failed_source_operations.contains(operation)) return incomplete(id, "source borrow settlement");
        for (const auto& [publication, stage] : state.publication_steps)
            if (stage != 3U && !state.failed_publications.contains(publication)) return incomplete(id, "publication stage settlement");
    }
    return true;
}
auto SurfaceAudit::SettleScenario() -> void {
    for (auto iterator = surfaces.begin(); iterator != surfaces.end();) {
        auto& [id, state] = *iterator;
        if (state.native_retired && (state.firefox_retired || state.firefox_import_failed) && (state.retired != 0U || state.created == 0U)) {
            if (const auto reason = joined_surface_failure(id, state); !reason.empty()) { return; }
            iterator = surfaces.erase(iterator);
            continue;
        }
        // A scenario boundary is not a release of the receiver-owned image.
        // Retain bounded physical publication/copy provenance, including
        // incomplete independent-stream joins, until actual retirement.
        ++iterator;
    }
    std::erase_if(sources, [&](const auto& item) {
        const auto& [id, source] = item;
        return source.native.retired() && source.browser.retired() && source_joined(source) && !surfaces.contains(source.arena);
    });
    // Keep unfinished callbacks addressable across scenario changes. Only
    // draws selected in the new scenario can prove its renderer handoff.
    scenario_started = browser_ordinal;
    std::erase_if(draws, [&](const Draw& draw) { return draw.settled != 0U || !surfaces.contains(draw.selected); });
    for (auto& [_, state] : surfaces) state.draw_indices.clear();
    for (std::size_t index = 0U; index != draws.size(); ++index)
        if (draws[index].identity != 0U) surfaces.at(draws[index].selected).draw_indices.emplace(draws[index].identity, index);
}
auto SurfaceAudit::settled_draw(const Draw& draw) const -> const SampleCustody* {
    const auto& surface = surfaces.at(draw.selected);
    const auto custody = surface.custody.find(draw.publication);
    if (draw.ordinal <= scenario_started || custody == surface.custody.end() || !custody->second.acquired || custody->second.acquired_at >= draw.ordinal ||
        draw.submitted <= draw.encoded || draw.encoded <= draw.ordinal || draw.settled <= draw.submitted || draw.abandoned)
        return nullptr;
    return &custody->second;
}
auto SurfaceAudit::pending_fallback(const std::string& candidate) const -> const Draw* {
    const auto found = surfaces.find(candidate);
    if (!failure.empty() || found == surfaces.end()) return nullptr;
    const auto& pending = found->second;
    if (pending.admitted == 0U || pending.acquired != 0U || !pending.publications.empty() || !pending.reconstruction ||
        pending.reconstruction->requested != candidate || pending.reconstruction->ordinal <= pending.admitted)
        return nullptr;
    const auto& active_identity = pending.reconstruction->completed;
    const auto& active = surfaces.at(active_identity);
    std::size_t latest_prior_acquisition = 0U;
    for (const auto& [_, surface] : surfaces)
        for (const auto& [publication, custody] : surface.custody)
            if (custody.acquired_at < pending.reconstruction->ordinal) latest_prior_acquisition = std::max(latest_prior_acquisition, custody.acquired_at);
    const auto retained = active.custody.find(pending.reconstruction->publication);
    if (active.generation >= pending.generation || retained == active.custody.end() || retained->second.acquired_at == 0U ||
        retained->second.acquired_at != latest_prior_acquisition || !receipts_joined(active_identity, active, true))
        return nullptr;
    const auto fallback = std::ranges::find_if(draws, [&](const Draw& draw) {
        return draw.selected == active_identity && draw.requested == (pending.created == 0U ? active_identity : candidate) &&
               draw.publication == pending.reconstruction->publication && draw.ordinal > pending.reconstruction->ordinal && settled_draw(draw) != nullptr;
    });
    return fallback == draws.end() ? nullptr : &*fallback;
}
auto SurfaceAudit::pending_supersession_completed() const -> bool {
    if (!failure.empty()) return false;
    for (const auto& [b, pending] : surfaces) {
        const auto preparation = pending.admitted;
        const auto discarded = pending.created == 0U ? pending.firefox_withdrawn : pending.discarded;
        const bool retired = pending.created == 0U ? pending.firefox_retired > discarded : pending.retired > discarded;
        if (!pending.candidate_withdrawn || preparation == 0U || pending.acquired != 0U || !pending.publications.empty() || discarded <= preparation ||
            !retired || !pending.reconstruction || pending.reconstruction->requested != b || pending.reconstruction->ordinal <= preparation ||
            (pending.created != 0U && pending.reconstruction->ordinal >= discarded) || !pending.firefox_retired || !pending.native_retired)
            continue;
        if (!joined_surface_failure(b, pending).empty()) continue;
        const auto* fallback = pending_fallback(b);
        if (!fallback) continue;
        for (const auto& draw : draws) {
            const auto& latest = surfaces.at(draw.selected);
            const auto custody = settled_draw(draw);
            if (latest.generation > pending.generation && latest.created > preparation && latest.acquired > discarded &&
                latest.acquired > pending.reconstruction->ordinal && draw.ordinal > fallback->ordinal && draw.requested == draw.selected &&
                custody != nullptr && custody->acquired_at == latest.acquired && receipts_joined(draw.selected, latest, true))
                return true;
        }
    }
    return false;
}
}  // namespace mmltk::acceptance::wayland
