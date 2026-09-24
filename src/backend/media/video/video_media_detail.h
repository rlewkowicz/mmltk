#pragma once
#include "video_media.h"
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
namespace mmltk::backend::media::video {
void require_media(int result, const char* operation);
struct VideoMediaInfo::State final {
 struct Track final {
  int source = -1, disposition = 0;
  AVRational time_base{};
  AVCodecParameters* parameters = avcodec_parameters_alloc();
  AVDictionary* metadata = nullptr;
  Track() = default;
  Track(const Track&) = delete;
  ~Track() { avcodec_parameters_free(&parameters); av_dict_free(&metadata); }
 };
 AVRational time_base{};
 std::int64_t origin_us = 0;
 std::vector<std::unique_ptr<Track>> audio;
};
struct VideoAudioPacket::State final {
 AVPacket* packet = av_packet_alloc();
 ~State() { av_packet_free(&packet); }
};
}
