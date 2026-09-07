#include "src/controller/presentation/visual_diagnostics.h"

namespace mmltk::controller {

std::string visual_failure_detail(const std::exception_ptr failure, const std::string_view fallback) {
    std::string_view detail = fallback;
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& error) { detail = error.what(); } catch (...) {
    }
    return std::string{detail.substr(0U, std::min(detail.size(), kVisualFailureByteCapacity))};
}

void report_visual_worker_failure(const VisualDiagnosticSink diagnostics, const VisualSystemKind system, const int device,
                                  const std::string_view detail, const std::uint64_t generation) noexcept {
    diagnostics({
        .system = system,
        .operation = VisualDiagnosticOperation::WorkerFailure,
        .device = device,
        .generation = generation,
        .failure_detail = detail,
    });
}

}  // namespace mmltk::controller
