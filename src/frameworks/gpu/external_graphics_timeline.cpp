#include "src/frameworks/gpu/external_graphics_timeline.h"

#include <stdexcept>
#include <limits>
#include <utility>

namespace mmltk::frameworks::gpu {

ExternalGraphicsTimeline::ExternalGraphicsTimeline(mmltk::common::io::ScopedFd descriptor)
    : ExternalGraphicsTimeline(std::move(descriptor), &cudaImportExternalSemaphore) {}

ExternalGraphicsTimeline::ExternalGraphicsTimeline(mmltk::common::io::ScopedFd descriptor, const ImportOperation import_operation) {
    if (descriptor.get() < 0 || import_operation == nullptr)
        throw std::invalid_argument("external graphics timeline descriptor is invalid");
    cudaExternalSemaphoreHandleDesc import{};
    import.type = cudaExternalSemaphoreHandleTypeTimelineSemaphoreFd;
    import.handle.fd = descriptor.get();
    const cudaError_t status = import_operation(&semaphore_, &import);
    if (status == cudaSuccess) static_cast<void>(descriptor.release());
    if (status != cudaSuccess || semaphore_ == nullptr) throw std::runtime_error("external graphics timeline import failed");
}

ExternalGraphicsTimeline::~ExternalGraphicsTimeline() noexcept { static_cast<void>(Release()); }

ExternalGraphicsTimeline::ExternalGraphicsTimeline(ExternalGraphicsTimeline&& other) noexcept
    : semaphore_(std::exchange(other.semaphore_, nullptr)) {}

ExternalGraphicsTimeline& ExternalGraphicsTimeline::operator=(ExternalGraphicsTimeline&& other) noexcept {
    if (this == &other) return *this;
    static_cast<void>(Release());
    semaphore_ = std::exchange(other.semaphore_, nullptr);
    return *this;
}

bool ExternalGraphicsTimeline::valid() const noexcept { return semaphore_ != nullptr; }

cudaError_t ExternalGraphicsTimeline::Release() noexcept {
    const cudaExternalSemaphore_t semaphore = std::exchange(semaphore_, nullptr);
    return semaphore == nullptr ? cudaSuccess : cudaDestroyExternalSemaphore(semaphore);
}

void ExternalGraphicsTimeline::SignalReadyAndWaitForRelease(cudaStream_t stream, const std::uint64_t ready_value) {
    SignalReady(stream, ready_value);
    WaitForRelease(stream, ready_value + 1U);
}

void ExternalGraphicsTimeline::SignalReady(cudaStream_t stream, const std::uint64_t ready_value) {
    if (!valid() || stream == nullptr || ready_value == 0U || ready_value == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("external graphics timeline publication is invalid");
    cudaExternalSemaphoreSignalParams signal{};
    signal.params.fence.value = ready_value;
    cudaExternalSemaphore_t timeline = semaphore_;
    cudaError_t status = cudaSignalExternalSemaphoresAsync(&timeline, &signal, 1U, stream);
    if (status != cudaSuccess) throw std::runtime_error("external graphics ready signal failed");
}

void ExternalGraphicsTimeline::WaitForRelease(cudaStream_t stream, const std::uint64_t release_value) {
    if (!valid() || stream == nullptr || release_value <= 1U) throw std::invalid_argument("external graphics release wait is invalid");
    cudaExternalSemaphoreWaitParams wait{};
    wait.params.fence.value = release_value;
    cudaExternalSemaphore_t timeline = semaphore_;
    const cudaError_t status = cudaWaitExternalSemaphoresAsync(&timeline, &wait, 1U, stream);
    if (status != cudaSuccess) throw std::runtime_error("external graphics release wait failed");
}

}  // namespace mmltk::frameworks::gpu
