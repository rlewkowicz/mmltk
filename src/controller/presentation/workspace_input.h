#pragma once
#include <cstddef>
#include <optional>
#include <utility>
#include <type_traits>
#include <stdexcept>
#include <vector>
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/workspace_input.h"
namespace mmltk::controller {
// The domain owner supplies synchronization and execution. Removing an item is
// constant time; geometric growth retains capacity across strokes and commands.
template <class Record>
class WorkspaceInputQueue final {
    static_assert(std::is_nothrow_move_constructible_v<Record> && std::is_nothrow_move_assignable_v<Record>);

   public:
    void Push(Record record) {
        if (size_ == storage_.size()) {
            if (storage_.size() > storage_.max_size() / 2U) throw std::length_error("Workspace input capacity is exhausted");
            std::vector<std::optional<Record>> grown(storage_.empty() ? 64U : storage_.size() * 2U);
            for (std::size_t index = 0U; index != size_; ++index) grown[index] = std::move(storage_[(head_ + index) % storage_.size()]);
            storage_.swap(grown);
            head_ = 0U;
        }
        storage_[(head_ + size_) % storage_.size()].emplace(std::move(record));
        ++size_;
    }
    [[nodiscard]] std::optional<Record> Pop() {
        if (!size_) return std::nullopt;
        auto result = std::move(storage_[head_]);
        storage_[head_].reset();
        head_ = (head_ + 1U) % storage_.size();
        --size_;
        return result;
    }
    void Clear() noexcept {
        while (size_) {
            storage_[head_].reset();
            head_ = (head_ + 1U) % storage_.size();
            --size_;
        }
    }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

   private:
    std::vector<std::optional<Record>> storage_;
    std::size_t head_ = 0U;
    std::size_t size_ = 0U;
};
// Passive workspaces still consume every mouse record and retain their native
// pointer state. Their domain may act on that state without scheduling GPU work.
class WorkspaceInput final {
   public:
    void SetPeer(std::uint64_t epoch) noexcept;
    void Accept(WorkspaceMouse, PresentationSourceKind);
    [[nodiscard]] const std::optional<WorkspaceMouse>& latest() const noexcept { return latest_; }
    [[nodiscard]] bool pressed(WorkspaceMouseButton button) const noexcept;

   private:
    std::uint64_t peer_ = 0U;
    std::uint8_t buttons_ = 0U;
    std::optional<WorkspaceMouse> latest_;
    WorkspaceInputQueue<WorkspaceMouse> queue_;
};
}  // namespace mmltk::controller
