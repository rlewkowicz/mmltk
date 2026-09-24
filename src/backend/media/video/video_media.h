#pragma once
#include <cstdint>
#include <memory>
#include <optional>
namespace mmltk::backend::media::video {
struct VideoTiming final {
 std::optional<std::int64_t> pts;
 std::int64_t duration = 0;
 int time_base_numerator = 0, time_base_denominator = 1;
};
class VideoFileSink;
class VideoMediaInfo final {
public:
 struct State;
 std::uint32_t width = 0, height = 0;
 int rate_numerator = 0, rate_denominator = 1;

private:
 std::shared_ptr<const State> state_;
 friend class VideoFileSource;
 friend class VideoFileSink;
};
class VideoAudioPacket final {
public:
 struct State;

private:
 std::shared_ptr<const State> state_;
 friend class VideoFileSource;
 friend class VideoFileSink;
};
}  // namespace mmltk::backend::media::video
