#include "video_media_detail.h"
#include <stdexcept>
#include <string>
extern "C" {
#include <libavutil/error.h>
}
namespace mmltk::backend::media::video {
void require_media(int result, const char* operation) {
 if (result >= 0) return;
 char message[AV_ERROR_MAX_STRING_SIZE]{};
 av_strerror(result, message, sizeof(message));
 throw std::runtime_error(std::string(operation) + ": " + message);
}
}  // namespace mmltk::backend::media::video
