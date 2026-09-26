#pragma once
#include <cstddef>
#include <memory>
#include "src/backend/ml/runtime/backend_factory.h"
namespace mmltk::backend::models::rfdetr {
// Fixed forward capacity. Each admission reserves exactly one independent
// stream until ordered delivery has consumed that batch. Source copies have
// a separate completion boundary and never retain loader/decode credits.
class InferenceLanes final {
public:
 InferenceLanes(int device, std::size_t capacity);
 ~InferenceLanes();
 InferenceLanes(const InferenceLanes&) = delete;
 InferenceLanes& operator=(const InferenceLanes&) = delete;
 [[nodiscard]] std::size_t capacity() const noexcept;
 [[nodiscard]] std::size_t pending() const noexcept;
 void RestrictCapacity(std::size_t capacity);
 [[nodiscard]] mmltk::backend::ml::runtime::BorrowedCommandStream stream(std::size_t lane) const;
 [[nodiscard]] std::size_t Admit();
 void WaitSource(std::size_t lane, mmltk::backend::ml::runtime::BorrowedCommandStream producer);
 // Record on the lane after its last borrowed source read, before forward.
 // A decode producer also waits for that event before reusing its CHW view.
 void ReleaseSource(std::size_t lane, mmltk::backend::ml::runtime::BorrowedCommandStream producer = {});
 void Submitted(std::size_t lane);
 [[nodiscard]] std::size_t WaitOldest();
 void ReleaseOldest();
 // Covers partially submitted work and callback work after Submitted, too.
 [[nodiscard]] mmltk::backend::ml::runtime::RuntimeStatus Drain() noexcept;
 [[nodiscard]] mmltk::backend::ml::runtime::RuntimeStatus Close() noexcept;

private:
 void retire() noexcept;
 struct State;
 std::shared_ptr<State> state_;
};
}  // namespace mmltk::backend::models::rfdetr
