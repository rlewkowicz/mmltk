// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

// WARNING: If the log pattern contains thread id (i.e, %t) while this flag is

// WARNING: if your program forks, UNCOMMENT this flag to prevent undefined

// #define SPDLOG_LEVEL_NAMES { "MY TRACE", "MY DEBUG", "MY INFO", "MY WARNING", "MY ERROR", "MY
// #define SPDLOG_LEVEL_NAMES { "MY TRACE"sv, "MY DEBUG"sv, "MY INFO"sv, "MY WARNING"sv, "MY

// Macros like SPDLOG_DEBUG(..), SPDLOG_INFO(..)  will expand to empty statements if not enabled
