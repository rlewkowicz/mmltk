#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include "src/backend/data/dataset_loader.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
namespace mmltk::backend::models::rfdetr {
class DatasetBatchLease final {
   public:
    // One compiled stream and one checked-out batch transaction coexist.
    static constexpr std::size_t kSourceRetirementCapacity = 2U;
    DatasetBatchLease(std::shared_ptr<mmltk::backend::data::DatasetLoader> loader, std::uintptr_t stream,
                      std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement)
        : retirement_(std::move(retirement)), loader_(std::move(loader)), stream_(reinterpret_cast<void*>(stream)) {
        if (!retirement_ || !loader_) throw std::invalid_argument("prediction batch custody is unavailable");
        auto lease = retirement_->Reserve();
        if (!lease) throw std::runtime_error("prediction source retirement admission failed");
        retirement_lease_ = std::move(*lease);
    }
    void Adopt(mmltk::backend::data::Batch batch) noexcept {
        if (active_ || !loader_) std::terminate();
        batch_ = batch;
        active_ = true;
    }
    void StopWorkers() {
        if (loader_) loader_->stop_workers();
    }
    ~DatasetBatchLease() noexcept {
        if (active_) {
            try {
                Release();
            } catch (...) {}
        }
    }
    DatasetBatchLease(const DatasetBatchLease&) = delete;
    DatasetBatchLease& operator=(const DatasetBatchLease&) = delete;
    void Release() {
        if (!active_) return;
        try {
            loader_->release_batch(batch_, stream_);
        } catch (...) {
            // A failed release must never destroy a loader with a checked-out
            // lease or leave its CPU workers alive with quarantined storage.
            try {
                StopWorkers();
            } catch (...) {}
            active_ = false;
            std::move(retirement_lease_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(loader_)), cudaErrorUnknown);
            throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, "prediction dataset source release"};
        }
        active_ = false;
    }

   private:
    std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement_;
    mmltk::frameworks::gpu::TerminalCudaRetirementLease retirement_lease_;
    std::shared_ptr<mmltk::backend::data::DatasetLoader> loader_;
    mmltk::backend::data::Batch batch_{};
    void* stream_ = nullptr;
    bool active_ = false;
};
}  // namespace mmltk::backend::models::rfdetr
