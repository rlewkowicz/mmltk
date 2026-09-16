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
        fallback_seconds_ = 0.0;
        pause_started_ = {};
        paused_ = false;
    }
    void Pause(bool value) {
        std::scoped_lock lock(mutex_);
        PauseLocked(value, Clock::now());
    }
    void Pause(bool value, Clock::time_point now) {
        std::scoped_lock lock(mutex_);
        PauseLocked(value, now);
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
    // Direct scheduling seam; callers serialize frames, and Wait uses the same
    // state transition after the pause gate. Supplied clock values share one epoch.
    [[nodiscard]] Clock::time_point Schedule(std::optional<double> timestamp, double fps, Clock::time_point now) {
        std::scoped_lock lock(mutex_);
        return ScheduleLocked(timestamp, fps, now);
    }
    [[nodiscard]] bool Wait(std::optional<double> timestamp, double fps, std::stop_token stop) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait(lock, stop, [&] { return !paused_; })) return false;
        ScheduleLocked(timestamp, fps, Clock::now());
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
    void PauseLocked(bool value, Clock::time_point now) {
        if (paused_ == value) return;
        if (value)
            pause_started_ = now;
        else if (deadline_) {
            const auto elapsed = now - pause_started_;
            if (elapsed < Clock::duration::zero() || elapsed >= Clock::time_point::max() - *deadline_)
                throw std::runtime_error("video pause exceeds the playback clock");
            *deadline_ += elapsed;
        }
        paused_ = value;
        changed_.notify_all();
    }
    Clock::time_point ScheduleLocked(std::optional<double> timestamp, double fps, Clock::time_point now) {
        const bool advances_source = timestamp && std::isfinite(*timestamp) && (!previous_ || *timestamp > *previous_);
        auto next_deadline = now;
        auto next_fallback = fallback_seconds_;
        if (!deadline_) {
            if (!advances_source && (!std::isfinite(fps) || fps <= 0.0)) throw std::runtime_error("video has no usable presentation timing");
        } else {
            auto seconds = Interval(previous_, timestamp, fps);
            if (advances_source && previous_ && std::isfinite(*timestamp - *previous_)) {
                // Missing/invalid PTS already consumed source time through FPS.
                // Recovery contributes only the remainder, never rewinds, and
                // establishes a fresh baseline even when fallback overtook PTS.
                seconds = std::max(0.0, seconds - fallback_seconds_);
            } else if (previous_ && !advances_source) {
                next_fallback += seconds;
                if (!std::isfinite(next_fallback)) throw std::runtime_error("video fallback duration exceeds the playback clock");
            }
            // Late inference rebases only the wall-clock deadline. Its delay,
            // like a pause, is not source time credited against recovered PTS.
            next_deadline = seconds == 0.0 ? std::max(*deadline_, now) : Advance(*deadline_, now, seconds);
        }
        // Commit timing together only after every interval/overflow check.
        deadline_ = next_deadline;
        fallback_seconds_ = advances_source ? 0.0 : next_fallback;
        if (advances_source) previous_ = timestamp;
        return next_deadline;
    }
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<double> previous_;
    std::optional<Clock::time_point> deadline_;
    double fallback_seconds_ = 0.0;
    Clock::time_point pause_started_{};
    bool paused_ = false;
};
}  // namespace mmltk::controller::detail
