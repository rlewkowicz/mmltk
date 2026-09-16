#pragma once
#include <cuda.h>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace mmltk::frameworks::gpu {
struct CudaContextApi final {
    void* context = nullptr;
    CUresult (*get)(void*, CUcontext*) noexcept = [](void*, CUcontext* current) noexcept { return cuCtxGetCurrent(current); };
    CUresult (*set)(void*, CUcontext) noexcept = [](void*, CUcontext current) noexcept { return cuCtxSetCurrent(current); };
};
class CudaContextFailure final : public std::exception {
   public:
    explicit CudaContextFailure(bool terminal) noexcept : terminal_(terminal) {}
    [[nodiscard]] const char* what() const noexcept override { return "CUDA caller context restoration failed"; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }

   private:
    bool terminal_;
};
struct CudaContextOwner final {
    void* state;
    void (*retain)(void*) noexcept;
};
// Captures exact driver identity, including null and same-device isolated contexts.
// Owners reserve physical custody before construction. Run is the explicit
// operation/Finalize boundary; the destructor never performs CUDA or callbacks.
class CudaContextScope final {
   public:
    explicit CudaContextScope(CudaContextOwner owner, CudaContextApi api = {}) noexcept : owner_(owner), api_(api) {
        status_ = api_.get && api_.set ? api_.get(api_.context, &previous_) : CUDA_ERROR_INVALID_VALUE;
        if (status_ == CUDA_SUCCESS)
            active_ = true;
        else
            Abandon();
    }
    ~CudaContextScope() {
        if (active_) std::terminate();
    }
    CudaContextScope(const CudaContextScope&) = delete;
    CudaContextScope& operator=(const CudaContextScope&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return active_; }
    [[nodiscard]] CUresult status() const noexcept { return status_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }
    [[nodiscard]] CUcontext Current() {
        CUcontext current{};
        if (!active_) throw CudaContextFailure(true);
        status_ = api_.get(api_.context, &current);
        if (status_ != CUDA_SUCCESS) {
            Abandon();
            throw CudaContextFailure(true);
        }
        return current;
    }
    void Select(CUcontext context) {
        if (!active_) throw CudaContextFailure(true);
        status_ = api_.set(api_.context, context);
        if (Lost(status_)) {
            Abandon();
            throw CudaContextFailure(true);
        }
        if (status_ != CUDA_SUCCESS) throw std::runtime_error("CUDA owner context selection failed");
    }
    void Abandon() noexcept {
        active_ = false;
        if (std::exchange(terminal_, true)) return;
        if (owner_.retain) owner_.retain(owner_.state);
    }
    [[nodiscard]] CUresult Finalize() noexcept {
        if (!active_) return status_;
        active_ = false;
        const auto first = api_.set(api_.context, previous_);
        if (Lost(first))
            Abandon();
        else if (first != CUDA_SUCCESS && api_.set(api_.context, previous_) != CUDA_SUCCESS)
            Abandon();
        if (status_ == CUDA_SUCCESS) status_ = first;
        return status_;
    }
    template <class Operation>
    decltype(auto) Run(Operation&& operation) {
        using Result = std::invoke_result_t<Operation>;
        try {
            if (!active_) throw CudaContextFailure(true);
            if constexpr (std::is_void_v<Result>) {
                std::forward<Operation>(operation)();
                Finish();
            } else {
                Result result = std::forward<Operation>(operation)();
                Finish();
                return result;
            }
        } catch (const CudaContextFailure& error) {
            if (error.terminal()) Abandon();
            Finish();
            throw;
        } catch (...) {
            if (active_) {
                static_cast<void>(Finalize());
                if (terminal_) throw CudaContextFailure(true);
            }
            throw;
        }
    }

   private:
    [[nodiscard]] static bool Lost(CUresult status) noexcept { return status == CUDA_ERROR_DEINITIALIZED || status == CUDA_ERROR_CONTEXT_IS_DESTROYED; }
    void Finish() {
        if (active_ && Finalize() != CUDA_SUCCESS) throw CudaContextFailure(terminal_);
    }
    CudaContextOwner owner_;
    CudaContextApi api_;
    CUcontext previous_ = nullptr;
    CUresult status_ = CUDA_SUCCESS;
    bool active_ = false;
    bool terminal_ = false;
};
}  // namespace mmltk::frameworks::gpu
