#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
namespace mmltk::controller::detail {
// Playback scheduling belongs to Predict, independently of the file decoder.
class PredictionPlayback final {
   public:
    using Clock = std::chrono::steady_clock;
    void Reset() {
        std::scoped_lock lock(mutex_);
        previous_.reset();
        deadline_.reset();
        paused_ = false;
    }
    void Pause(bool value) {
        std::scoped_lock lock(mutex_);
        if (paused_ == value) return;
        if (value) pause_started_ = Clock::now();
        else if (deadline_) *deadline_ += Clock::now() - pause_started_;
        paused_ = value;
        changed_.notify_all();
    }
    [[nodiscard]] static double Interval(std::optional<double> previous, std::optional<double> next, double fps) {
        if (previous && next) {
            const auto elapsed = *next - *previous;
            if (std::isfinite(elapsed) && elapsed > 0.0) return elapsed;
        }
        if (std::isfinite(fps) && fps > 0.0) return 1.0 / fps;
        throw std::runtime_error("video has neither monotonic presentation timestamps nor valid frame rate");
    }
    [[nodiscard]] static Clock::time_point Advance(Clock::time_point previous, Clock::time_point now, double seconds) {
        const auto available = std::chrono::duration<double>(Clock::time_point::max() - previous).count();
        if (!std::isfinite(seconds) || seconds <= 0.0 || seconds >= available)
            throw std::runtime_error("video presentation interval exceeds the playback clock");
        return std::max(previous + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)), now);
    }
    [[nodiscard]] bool Wait(std::optional<double> timestamp, double fps, std::stop_token stop) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait(lock, stop, [&] { return !paused_; })) return false;
        const auto now = Clock::now();
        if (!deadline_) {
            if ((!timestamp || !std::isfinite(*timestamp)) && (!std::isfinite(fps) || fps <= 0.0))
                throw std::runtime_error("video has no usable presentation timing");
            deadline_ = now;
        } else {
            // Rebase a late frame to now. The following frame gets a fresh
            // source interval, so overdue timestamps cannot cause catch-up bursts.
            *deadline_ = Advance(*deadline_, now, Interval(previous_, timestamp, fps));
        }
        if (timestamp && std::isfinite(*timestamp) && (!previous_ || *timestamp > *previous_)) previous_ = timestamp;
        while (!stop.stop_requested()) {
            if (paused_) {
                if (!changed_.wait(lock, stop, [&] { return !paused_; })) return false;
                continue;
            }
            if (Clock::now() >= *deadline_) return true;
            changed_.wait_until(lock, stop, *deadline_, [&] { return paused_; });
        }
        return false;
    }
   private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<double> previous_;
    std::optional<Clock::time_point> deadline_;
    Clock::time_point pause_started_{};
    bool paused_ = false;
};
}
