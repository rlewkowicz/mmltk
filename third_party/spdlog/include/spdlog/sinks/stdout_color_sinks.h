
#pragma once

#include <spdlog/details/synchronous_factory.h>
#include <spdlog/sinks/ansicolor_sink.h>

namespace spdlog {
namespace sinks {
using stdout_color_sink_mt = ansicolor_stdout_sink_mt;
using stdout_color_sink_st = ansicolor_stdout_sink_st;
using stderr_color_sink_mt = ansicolor_stderr_sink_mt;
using stderr_color_sink_st = ansicolor_stderr_sink_st;
}

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stdout_color_mt(const std::string& logger_name, color_mode mode = color_mode::automatic);

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stdout_color_st(const std::string& logger_name, color_mode mode = color_mode::automatic);

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stderr_color_mt(const std::string& logger_name, color_mode mode = color_mode::automatic);

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stderr_color_st(const std::string& logger_name, color_mode mode = color_mode::automatic);

}

#ifdef SPDLOG_HEADER_ONLY
#include "stdout_color_sinks-inl.h"
#endif
