#pragma once

#include "rfdetr/torch_cuda_utils.h"

#include <memory>

namespace mmltk::rfdetr {

class SharedCudaEvent final {
public:
    explicit SharedCudaEvent(cudaEvent_t event) : event_(event) {}

    ~SharedCudaEvent() {
        if (event_ != nullptr) {
            cudaEventDestroy(event_);
        }
    }

    SharedCudaEvent(const SharedCudaEvent&) = delete;
    SharedCudaEvent& operator=(const SharedCudaEvent&) = delete;

    [[nodiscard]] cudaEvent_t get() const { return event_; }

private:
    cudaEvent_t event_ = nullptr;
};

inline std::shared_ptr<SharedCudaEvent> record_shared_cuda_event(cudaStream_t stream, const char* context) {
    cudaEvent_t event = nullptr;
    ensure_cuda_ok(cudaEventCreateWithFlags(&event, cudaEventDisableTiming), context);
    auto handle = std::make_shared<SharedCudaEvent>(event);
    ensure_cuda_ok(cudaEventRecord(handle->get(), stream), context);
    return handle;
}

inline void wait_for_shared_cuda_event(cudaStream_t stream,
                                       const SharedCudaEvent& event,
                                       const char* context) {
    ensure_cuda_ok(cudaStreamWaitEvent(stream, event.get(), 0), context);
}

} // namespace mmltk::rfdetr
