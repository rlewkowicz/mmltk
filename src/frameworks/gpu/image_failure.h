#pragma once

#include <exception>
#include <stdexcept>
#include <utility>

namespace mmltk::frameworks::gpu {

// Failures form an ordered tree: the initiating failure is primary, and
// subsequent settlement/retirement failures are secondary. Leaves retain the
// original exception objects, rather than only their diagnostic strings.
class ImageFailure : public std::runtime_error {
   public:
    ImageFailure(std::exception_ptr primary, std::exception_ptr secondary = {})
        : std::runtime_error(Message(primary ? primary : secondary)), primary_(std::move(primary)), secondary_(std::move(secondary)) {}
    [[nodiscard]] const std::exception_ptr& primary() const noexcept { return primary_; }
    [[nodiscard]] const std::exception_ptr& secondary() const noexcept { return secondary_; }

   private:
    static const char* Message(const std::exception_ptr& failure) noexcept {
        try {
            if (failure) std::rethrow_exception(failure);
        } catch (const std::exception& error) { return error.what(); } catch (...) {
        }
        return "image stream completion boundary was not established";
    }
    std::exception_ptr primary_;
    std::exception_ptr secondary_;
};

class ImageStreamExecutionFailure : public ImageFailure {
   public:
    using ImageFailure::ImageFailure;
};

// Inspect the ordered tree without replacing any exception object. Primary
// branches win when both branches carry the requested type.
template <class Failure>
[[nodiscard]] std::exception_ptr find_image_failure(const std::exception_ptr& failure) noexcept {
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const Failure&) { return failure; } catch (const ImageFailure& aggregate) {
        if (auto primary = find_image_failure<Failure>(aggregate.primary())) return primary;
        return find_image_failure<Failure>(aggregate.secondary());
    } catch (...) {}
    return {};
}

[[nodiscard]] inline bool is_image_execution_failure(const std::exception_ptr& failure) noexcept {
    return static_cast<bool>(find_image_failure<ImageStreamExecutionFailure>(failure));
}

[[nodiscard]] inline std::exception_ptr combine_image_failures(std::exception_ptr primary, std::exception_ptr secondary) noexcept {
    if (!primary) return secondary;
    if (!secondary || secondary == primary) return primary;
    try {
        if (is_image_execution_failure(primary) || is_image_execution_failure(secondary))
            throw ImageStreamExecutionFailure(std::move(primary), std::move(secondary));
        throw ImageFailure(std::move(primary), std::move(secondary));
    } catch (...) { return std::current_exception(); }
}

}  // namespace mmltk::frameworks::gpu
