// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <ctime>  // std::time_t
#include <spdlog/common.h>

namespace spdlog {
namespace details {
namespace os {

SPDLOG_API spdlog::log_clock::time_point now() SPDLOG_NOEXCEPT;

SPDLOG_API std::tm localtime(const std::time_t& time_tt) SPDLOG_NOEXCEPT;

SPDLOG_API std::tm localtime() SPDLOG_NOEXCEPT;

SPDLOG_API std::tm gmtime(const std::time_t& time_tt) SPDLOG_NOEXCEPT;

SPDLOG_API std::tm gmtime() SPDLOG_NOEXCEPT;

#if !defined(SPDLOG_EOL)
#ifdef _WIN32
#define SPDLOG_EOL "\r\n"
#else
#define SPDLOG_EOL "\n"
#endif
#endif

SPDLOG_CONSTEXPR static const char* default_eol = SPDLOG_EOL;

#if !defined(SPDLOG_FOLDER_SEPS)
#ifdef _WIN32
#define SPDLOG_FOLDER_SEPS "\\/"
#else
#define SPDLOG_FOLDER_SEPS "/"
#endif
#endif

SPDLOG_CONSTEXPR static const char folder_seps[] = SPDLOG_FOLDER_SEPS;
SPDLOG_CONSTEXPR static const filename_t::value_type folder_seps_filename[] = SPDLOG_FILENAME_T(SPDLOG_FOLDER_SEPS);

SPDLOG_API bool fopen_s(FILE** fp, const filename_t& filename, const filename_t& mode);

SPDLOG_API int remove(const filename_t& filename) SPDLOG_NOEXCEPT;

SPDLOG_API int remove_if_exists(const filename_t& filename) SPDLOG_NOEXCEPT;

SPDLOG_API int rename(const filename_t& filename1, const filename_t& filename2) SPDLOG_NOEXCEPT;

SPDLOG_API bool path_exists(const filename_t& filename) SPDLOG_NOEXCEPT;

SPDLOG_API size_t filesize(FILE* f);

SPDLOG_API int utc_minutes_offset(const std::tm& tm = details::os::localtime());

SPDLOG_API size_t _thread_id() SPDLOG_NOEXCEPT;

SPDLOG_API size_t thread_id() SPDLOG_NOEXCEPT;

SPDLOG_API void sleep_for_millis(unsigned int milliseconds) SPDLOG_NOEXCEPT;

SPDLOG_API std::string filename_to_str(const filename_t& filename);

SPDLOG_API int pid() SPDLOG_NOEXCEPT;

SPDLOG_API bool is_color_terminal() SPDLOG_NOEXCEPT;

SPDLOG_API bool in_terminal(FILE* file) SPDLOG_NOEXCEPT;

#if (defined(SPDLOG_WCHAR_TO_UTF8_SUPPORT) || defined(SPDLOG_WCHAR_FILENAMES)) && defined(_WIN32)
SPDLOG_API void wstr_to_utf8buf(wstring_view_t wstr, memory_buf_t& target);

SPDLOG_API void utf8_to_wstrbuf(string_view_t str, wmemory_buf_t& target);
#endif

SPDLOG_API filename_t dir_name(const filename_t& path);

SPDLOG_API bool create_dir(const filename_t& path);

SPDLOG_API std::string getenv(const char* field);

SPDLOG_API bool fsync(FILE* fp);

SPDLOG_API bool fwrite_bytes(const void* ptr, const size_t n_bytes, FILE* fp);

}  // namespace os
}  // namespace details
}  // namespace spdlog

#ifdef SPDLOG_HEADER_ONLY
#include "os-inl.h"
#endif
