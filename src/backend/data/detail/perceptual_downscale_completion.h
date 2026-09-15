// SPDX-License-Identifier: MIT
// Private completion custody for the Öztireli/Gross perceptual downscaler.
#pragma once
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mmltk::backend::data::perceptual {
// Query errors retain their Driver identity. Only documented argument/range
// failures are recoverable; a prior asynchronous failure can surface here.
enum class AdmissionQuery { Context, Pointer, Range, Stream };
struct DriverAdmission {
    CUresult status=CUDA_SUCCESS;
    bool valid=true;
    AdmissionQuery query=AdmissionQuery::Context;
    bool caller_rejection() const noexcept {
        if(status==CUDA_SUCCESS) return !valid;
        if(query==AdmissionQuery::Context) return false;
        if(status==CUDA_ERROR_INVALID_VALUE) return true;
        return (query==AdmissionQuery::Range && status==CUDA_ERROR_NOT_FOUND) ||
               (query==AdmissionQuery::Stream && status==CUDA_ERROR_INVALID_HANDLE);
    }
    bool physical_failure() const noexcept {return status!=CUDA_SUCCESS && !caller_rejection();}
};
class CompletionAllocationFailure final : public std::runtime_error {
 public:
    explicit CompletionAllocationFailure(cudaError_t status)
        :std::runtime_error("perceptual CUDA completion allocation failed"),status_(status) {}
    cudaError_t status() const noexcept {return status_;}
    bool physical_failure() const noexcept {return status_!=cudaErrorMemoryAllocation;}
 private:
    cudaError_t status_;
};
struct CompletionApi {
    decltype(&cudaEventCreateWithFlags) create=&cudaEventCreateWithFlags;
    decltype(&cudaEventRecord) record=&cudaEventRecord;
    decltype(&cudaEventQuery) query=&cudaEventQuery;
    decltype(&cudaEventSynchronize) wait=&cudaEventSynchronize;
    decltype(&cudaStreamSynchronize) stream_wait=&cudaStreamSynchronize;
    decltype(&cudaStreamWaitEvent) order=&cudaStreamWaitEvent;
    decltype(&cudaEventDestroy) destroy=&cudaEventDestroy;
};
// Owns only the exact in-flight accesses and their completion events. Device
// workspace and terminal retirement remain with GpuPerceptualDownscaler.
class CudaDownscaleCompletion final {
 public:
    struct Submission {
        cudaEvent_t event=nullptr;
        cudaStream_t stream=nullptr;
        bool pending=false, recorded=false;
        std::shared_ptr<const void> source, destination;
    };
    explicit CudaDownscaleCompletion(CompletionApi api={}) :api_(api) {}
    CudaDownscaleCompletion(const CudaDownscaleCompletion&)=delete;
    CudaDownscaleCompletion& operator=(const CudaDownscaleCompletion&)=delete;
    // Event allocation precedes custody installation. The issuing owner calls
    // settle(false) first and handles its failure before a new reservation.
    Submission& reserve(cudaStream_t stream,std::shared_ptr<const void> source,std::shared_ptr<const void> destination) {
        for(auto& slot:slots_) {
            const auto same_owner=[](const auto& a,const auto& b){return !a.owner_before(b) && !b.owner_before(a);};
            if(slot.pending && slot.stream==stream && same_owner(slot.source,source) && same_owner(slot.destination,destination)) {
                // Repeated views of one batch aggregate extend its existing
                // exact completion boundary without another custody slot.
                slot.recorded=false;
                return slot;
            }
        }
        for (auto& slot:slots_) {
            if (slot.pending) continue;
            if (!slot.event) {
                const auto status=api_.create(&slot.event,cudaEventDisableTiming);
                if(status!=cudaSuccess) throw CompletionAllocationFailure(status);
            }
            slot.source=std::move(source);slot.destination=std::move(destination);
            slot.stream=stream;slot.pending=true;slot.recorded=false;
            return slot;
        }
        throw std::runtime_error("perceptual CUDA custody capacity reached; finish before submitting more images");
    }
    cudaError_t order(const Submission& slot) noexcept {
        return latest_ && latest_stream_!=slot.stream ? api_.order(slot.stream,latest_,0):cudaSuccess;
    }
    cudaError_t record(Submission& slot) noexcept {
        const auto status=api_.record(slot.event,slot.stream);
        slot.recorded=status==cudaSuccess;
        if (slot.recorded) {latest_=slot.event;latest_stream_=slot.stream;}
        return status;
    }
    cudaError_t settle(bool wait) noexcept {
        for (auto& slot:slots_) {
            if (!slot.pending) continue;
            const auto status=slot.recorded ? (wait ? api_.wait(slot.event):api_.query(slot.event)):
                                             (wait ? api_.stream_wait(slot.stream):cudaErrorNotReady);
            if (status==cudaErrorNotReady && !wait) continue;
            if (status!=cudaSuccess) return status;
            slot.pending=false;slot.recorded=false;slot.source.reset();slot.destination.reset();
        }
        return cudaSuccess;
    }
    void forget_order() noexcept {latest_=nullptr;latest_stream_=nullptr;}
    cudaError_t release() noexcept {
        const auto status=settle(true);
        if (status!=cudaSuccess) return status;
        for (auto& slot:slots_) {
            if (!slot.event) continue;
            const auto destroyed=api_.destroy(slot.event);
            if (destroyed!=cudaSuccess) return destroyed;
            slot.event=nullptr;
        }
        forget_order();
        return cudaSuccess;
    }
 private:
    CompletionApi api_;
    std::array<Submission,16> slots_{};
    cudaEvent_t latest_=nullptr;
    cudaStream_t latest_stream_=nullptr;
};
}
