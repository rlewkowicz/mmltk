#include "src/controller/presentation/visual_diagnostics.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace mmltk::controller {

contracts::DiagnosticSource visual_diagnostic_source(const VisualSourceObservation& observation) noexcept {
    const auto& frame = observation.frame;
    return {
        .source_session = presentation_source_session(frame.source.kind),
        .source_instance = frame.source.instance,
        .source_revision = frame.revision,
        .clean_revision = visual_clean_content_identity(frame).revision,
        .source_observation_revision = observation.snapshot_revision,
        .source_width = frame.extent.width,
        .source_height = frame.extent.height,
        .content_x = frame.content.x,
        .content_y = frame.content.y,
        .content_width = frame.content.width,
        .content_height = frame.content.height,
    };
}

services::RuntimeDiagnosticFact visual_runtime_diagnostic(const VisualDiagnosticFact fact) noexcept {
    return {
        .owner = fact.system,
        .event = visual_diagnostic_event_name(fact.operation),
        .sequence = fact.generation,
        .value = fact.value,
        .detail = fact.detail != 0U ? fact.detail : static_cast<std::uint64_t>(fact.copy_path),
        .device = fact.device,
        .context = fact.context,
        .message = fact.failure_detail,
    };
}

std::string visual_failure_detail(const std::exception_ptr failure, const std::string_view fallback) {
    std::string_view detail = fallback;
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& error) { detail = error.what(); } catch (...) {
    }
    // Dependency exception text is not necessarily UTF-8. Preserve complete
    // valid code points and replace each malformed byte before applying the
    // byte limit, so the bounded runtime JSON encoder never loses this failure.
    std::array<char, kVisualFailureByteCapacity> bounded;
    std::size_t size = 0U;
    while (!detail.empty() && size < bounded.size()) {
        const auto first = static_cast<unsigned char>(detail.front());
        std::size_t length = 1U;
        std::uint32_t point = first;
        std::uint32_t minimum = 0U;
        bool valid = first < 0x80U;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2U;
            point = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3U;
            point = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4U;
            point = first & 0x07U;
            minimum = 0x10000U;
        }
        if (length > 1U && detail.size() >= length) {
            valid = true;
            for (std::size_t index = 1U; index < length; ++index) {
                const auto continuation = static_cast<unsigned char>(detail[index]);
                if ((continuation & 0xc0U) != 0x80U) {
                    valid = false;
                    break;
                }
                point = (point << 6U) | (continuation & 0x3fU);
            }
            valid = valid && point >= minimum && point <= 0x10ffffU && !(point >= 0xd800U && point <= 0xdfffU);
        }
        const auto next = valid ? detail.substr(0U, length) : std::string_view{"\xef\xbf\xbd"};
        if (next.size() > bounded.size() - size) break;
        std::memcpy(bounded.data() + size, next.data(), next.size());
        size += next.size();
        detail.remove_prefix(valid ? length : 1U);
    }
    return {bounded.data(), size};
}

void report_visual_worker_failure(const VisualDiagnosticSink diagnostics, const contracts::DiagnosticOwner system, const int device,
                                  const std::string_view detail, const std::uint64_t generation) noexcept {
    diagnostics.Emit([&] { return VisualDiagnosticFact{
        .system = system,
        .operation = VisualDiagnosticOperation::WorkerFailure,
        .device = device,
        .generation = generation,
        .failure_detail = detail,
    }; });
}

}  // namespace mmltk::controller
