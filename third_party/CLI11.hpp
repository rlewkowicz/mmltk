
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#define CLI11_VERSION_MAJOR 2
#define CLI11_VERSION_MINOR 6
#define CLI11_VERSION_PATCH 2
#define CLI11_VERSION "2.6.2"

#if !(defined(_MSC_VER) && __cplusplus == 199711L) && !defined(__INTEL_COMPILER)
#if __cplusplus >= 201402L
#define CLI11_CPP14
#if __cplusplus >= 201703L
#define CLI11_CPP17
#if __cplusplus > 201703L
#define CLI11_CPP20
#if __cplusplus > 202002L
#define CLI11_CPP23
#if __cplusplus > 202302L
#define CLI11_CPP26
#endif
#endif
#endif
#endif
#endif
#elif defined(_MSC_VER) && __cplusplus == 199711L
#if _MSVC_LANG >= 201402L
#define CLI11_CPP14
#if _MSVC_LANG > 201402L && _MSC_VER >= 1910
#define CLI11_CPP17
#if _MSVC_LANG > 201703L && _MSC_VER >= 1910
#define CLI11_CPP20
#if _MSVC_LANG > 202002L && _MSC_VER >= 1922
#define CLI11_CPP23
#endif
#endif
#endif
#endif
#endif

#if defined(CLI11_CPP14)
#define CLI11_DEPRECATED(reason) [[deprecated(reason)]]
#elif defined(_MSC_VER)
#define CLI11_DEPRECATED(reason) __declspec(deprecated(reason))
#else
#define CLI11_DEPRECATED(reason) __attribute__((deprecated(reason)))
#endif

#if !defined(CLI11_CPP17) || \
    (defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER) && __GNUC__ < 10 && __GNUC__ > 4)
#define CLI11_NODISCARD
#else
#define CLI11_NODISCARD [[nodiscard]]
#endif

#ifndef CLI11_USE_STATIC_RTTI
#if (defined(_HAS_STATIC_RTTI) && _HAS_STATIC_RTTI)
#define CLI11_USE_STATIC_RTTI 1
#elif defined(__cpp_rtti)
#if (defined(_CPPRTTI) && _CPPRTTI == 0)
#define CLI11_USE_STATIC_RTTI 1
#else
#define CLI11_USE_STATIC_RTTI 0
#endif
#elif (defined(__GCC_RTTI) && __GXX_RTTI)
#define CLI11_USE_STATIC_RTTI 0
#else
#define CLI11_USE_STATIC_RTTI 1
#endif
#endif

#if defined CLI11_CPP17 && defined __has_include && !defined CLI11_HAS_FILESYSTEM
#if __has_include(<filesystem>)
#if defined __MAC_OS_X_VERSION_MIN_REQUIRED && __MAC_OS_X_VERSION_MIN_REQUIRED < 101500
#define CLI11_HAS_FILESYSTEM 0
#elif defined(__wasi__)
#define CLI11_HAS_FILESYSTEM 0
#else
#include <filesystem>
#if defined __cpp_lib_filesystem && __cpp_lib_filesystem >= 201703
#if defined _GLIBCXX_RELEASE && _GLIBCXX_RELEASE >= 9
#define CLI11_HAS_FILESYSTEM 1
#elif defined(__GLIBCXX__)
#define CLI11_HAS_FILESYSTEM 0
#else
#define CLI11_HAS_FILESYSTEM 1
#endif
#else
#define CLI11_HAS_FILESYSTEM 0
#endif
#endif
#endif
#endif

#if !defined(CLI11_CPP26) && !defined(CLI11_HAS_CODECVT)
#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER) && __GNUC__ < 5
#define CLI11_HAS_CODECVT 0
#else
#define CLI11_HAS_CODECVT 1
#include <codecvt>
#endif
#else
#if defined(CLI11_HAS_CODECVT)
#if CLI11_HAS_CODECVT > 0
#include <codecvt>
#endif
#else
#define CLI11_HAS_CODECVT 0
#endif
#endif

#ifndef CLI11_HAS_RTTI
#if defined(__GXX_RTTI) && __GXX_RTTI == 1
#define CLI11_HAS_RTTI 1
#elif defined(_CPPRTTI) && _CPPRTTI == 1
#define CLI11_HAS_RTTI 1
#elif defined(__NO_RTTI__) && __NO_RTTI__ == 1
#define CLI11_HAS_RTTI 0
#elif defined(__has_feature)
#if __has_feature(cxx_rtti)
#define CLI11_HAS_RTTI 1
#else
#define CLI11_HAS_RTTI 0
#endif
#elif defined(__RTTI) || defined(__INTEL_RTTI__)
#define CLI11_HAS_RTTI 1
#else
#define CLI11_HAS_RTTI 0
#endif
#endif

#if defined(__GNUC__)  // GCC or clang
#define CLI11_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
#define CLI11_DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")

#define CLI11_DIAGNOSTIC_IGNORE_DEPRECATED _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")

#elif defined(_MSC_VER)
#define CLI11_DIAGNOSTIC_PUSH __pragma(warning(push))
#define CLI11_DIAGNOSTIC_POP __pragma(warning(pop))

#define CLI11_DIAGNOSTIC_IGNORE_DEPRECATED __pragma(warning(disable : 4996))

#else
#define CLI11_DIAGNOSTIC_PUSH
#define CLI11_DIAGNOSTIC_POP

#define CLI11_DIAGNOSTIC_IGNORE_DEPRECATED

#endif

#ifdef CLI11_COMPILE
#define CLI11_INLINE
#else
#define CLI11_INLINE inline
#endif

#if defined CLI11_CPP17
#define CLI11_MODULE_INLINE inline
#else
#define CLI11_MODULE_INLINE static
#endif

#if defined CLI11_HAS_FILESYSTEM && CLI11_HAS_FILESYSTEM > 0
#include <filesystem>  // NOLINT(build/include)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#ifdef CLI11_CPP17
#include <string_view>
#endif  // CLI11_CPP17

#if defined CLI11_HAS_FILESYSTEM && CLI11_HAS_FILESYSTEM > 0
#include <filesystem>
#include <string_view>  // NOLINT(build/include)
#endif                  // CLI11_HAS_FILESYSTEM

#if defined(_WIN32)
#if !(defined(_AMD64_) || defined(_X86_) || defined(_ARM_))
#if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64) || defined(_M_X64) || \
    defined(_M_AMD64)
#define _AMD64_
#elif defined(i386) || defined(__i386) || defined(__i386__) || defined(__i386__) || defined(_M_IX86)
#define _X86_
#elif defined(__arm__) || defined(_M_ARM) || defined(_M_ARMT)
#define _ARM_
#elif defined(__aarch64__) || defined(_M_ARM64)
#define _ARM64_
#elif defined(_M_ARM64EC)
#define _ARM64EC_
#endif
#endif

#ifndef NOMINMAX
#define NOMINMAX
#include <windef.h>
#undef NOMINMAX
#else
#include <windef.h>
#endif

#include <winbase.h>
#include <processthreadsapi.h>
#include <shellapi.h>
#endif

namespace CLI {

CLI11_INLINE std::string narrow(const std::wstring& str);
CLI11_INLINE std::string narrow(const wchar_t* str);
CLI11_INLINE std::string narrow(const wchar_t* str, std::size_t size);

CLI11_INLINE std::wstring widen(const std::string& str);
CLI11_INLINE std::wstring widen(const char* str);
CLI11_INLINE std::wstring widen(const char* str, std::size_t size);

#ifdef CLI11_CPP17
CLI11_INLINE std::string narrow(std::wstring_view str);
CLI11_INLINE std::wstring widen(std::string_view str);
#endif  // CLI11_CPP17

#if defined CLI11_HAS_FILESYSTEM && CLI11_HAS_FILESYSTEM > 0
CLI11_INLINE std::filesystem::path to_path(std::string_view str);
#endif  // CLI11_HAS_FILESYSTEM

namespace detail {

#if !CLI11_HAS_CODECVT
CLI11_INLINE void set_unicode_locale() {
    static const std::array<const char*, 3> unicode_locales{{"C.UTF-8", "en_US.UTF-8", ".UTF-8"}};

    for (const auto& locale_name : unicode_locales) {
        if (std::setlocale(LC_ALL, locale_name) != nullptr) {
            return;
        }
    }
    throw std::runtime_error("CLI::narrow: could not set locale to C.UTF-8");
}

template <typename F>
struct scope_guard_t {
    F closure;

    explicit scope_guard_t(F closure_) : closure(closure_) {}
    ~scope_guard_t() {
        closure();
    }
};

template <typename F>
CLI11_NODISCARD CLI11_INLINE scope_guard_t<F> scope_guard(F&& closure) {
    return scope_guard_t<F>{std::forward<F>(closure)};
}

#endif  // !CLI11_HAS_CODECVT

CLI11_DIAGNOSTIC_PUSH
CLI11_DIAGNOSTIC_IGNORE_DEPRECATED

CLI11_INLINE std::string narrow_impl(const wchar_t* str, std::size_t str_size) {
#if CLI11_HAS_CODECVT
#ifdef _WIN32
    return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(str, str + str_size);

#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(str, str + str_size);

#endif  // _WIN32
#else   // CLI11_HAS_CODECVT
    (void)str_size;
    std::mbstate_t state = std::mbstate_t();
    const wchar_t* it = str;

    std::string old_locale = std::setlocale(LC_ALL, nullptr);
    auto sg = scope_guard([&] { std::setlocale(LC_ALL, old_locale.c_str()); });
    set_unicode_locale();

    std::size_t new_size = std::wcsrtombs(nullptr, &it, 0, &state);
    if (new_size == static_cast<std::size_t>(-1)) {
        throw std::runtime_error("CLI::narrow: conversion error in std::wcsrtombs at offset " +
                                 std::to_string(it - str));
    }
    std::string result(new_size, '\0');
    std::wcsrtombs(const_cast<char*>(result.data()), &str, new_size, &state);

    return result;

#endif  // CLI11_HAS_CODECVT
}

CLI11_INLINE std::wstring widen_impl(const char* str, std::size_t str_size) {
#if CLI11_HAS_CODECVT
#ifdef _WIN32
    return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(str, str + str_size);

#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(str, str + str_size);

#endif  // _WIN32
#else   // CLI11_HAS_CODECVT
    (void)str_size;
    std::mbstate_t state = std::mbstate_t();
    const char* it = str;

    std::string old_locale = std::setlocale(LC_ALL, nullptr);
    auto sg = scope_guard([&] { std::setlocale(LC_ALL, old_locale.c_str()); });
    set_unicode_locale();

    std::size_t new_size = std::mbsrtowcs(nullptr, &it, 0, &state);
    if (new_size == static_cast<std::size_t>(-1)) {
        throw std::runtime_error("CLI::widen: conversion error in std::mbsrtowcs at offset " +
                                 std::to_string(it - str));
    }
    std::wstring result(new_size, L'\0');
    std::mbsrtowcs(const_cast<wchar_t*>(result.data()), &str, new_size, &state);

    return result;

#endif  // CLI11_HAS_CODECVT
}

CLI11_DIAGNOSTIC_POP

}  

CLI11_INLINE std::string narrow(const wchar_t* str, std::size_t str_size) {
    return detail::narrow_impl(str, str_size);
}
CLI11_INLINE std::string narrow(const std::wstring& str) {
    return detail::narrow_impl(str.data(), str.size());
}
CLI11_INLINE std::string narrow(const wchar_t* str) {
    return detail::narrow_impl(str, std::wcslen(str));
}

CLI11_INLINE std::wstring widen(const char* str, std::size_t str_size) {
    return detail::widen_impl(str, str_size);
}
CLI11_INLINE std::wstring widen(const std::string& str) {
    return detail::widen_impl(str.data(), str.size());
}
CLI11_INLINE std::wstring widen(const char* str) {
    return detail::widen_impl(str, std::strlen(str));
}

#ifdef CLI11_CPP17
CLI11_INLINE std::string narrow(std::wstring_view str) {
    return detail::narrow_impl(str.data(), str.size());
}
CLI11_INLINE std::wstring widen(std::string_view str) {
    return detail::widen_impl(str.data(), str.size());
}
#endif  // CLI11_CPP17

#if defined CLI11_HAS_FILESYSTEM && CLI11_HAS_FILESYSTEM > 0
CLI11_INLINE std::filesystem::path to_path(std::string_view str) {
    return std::filesystem::path{
#ifdef _WIN32
        widen(str)
#else
        str
#endif  // _WIN32
    };
}
#endif  // CLI11_HAS_FILESYSTEM

namespace detail {
#ifdef _WIN32
CLI11_INLINE std::vector<std::string> compute_win32_argv();
#endif
}  

namespace detail {

#ifdef _WIN32
CLI11_INLINE std::vector<std::string> compute_win32_argv() {
    std::vector<std::string> result;
    int argc = 0;

    auto deleter = [](wchar_t** ptr) { LocalFree(ptr); };
    // NOLINTBEGIN(*-avoid-c-arrays)
    auto wargv = std::unique_ptr<wchar_t*[], decltype(deleter)>(CommandLineToArgvW(GetCommandLineW(), &argc), deleter);
    // NOLINTEND(*-avoid-c-arrays)

    if (wargv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed with code " + std::to_string(GetLastError()));
    }

    result.reserve(static_cast<size_t>(argc));
    for (size_t i = 0; i < static_cast<size_t>(argc); ++i) {
        result.push_back(narrow(wargv[i]));
    }

    return result;
}
#endif

}  

namespace enums {

template <typename T, typename = typename std::enable_if<std::is_enum<T>::value>::type>
std::ostream& operator<<(std::ostream& in, const T& item) {
    return in << +static_cast<typename std::underlying_type<T>::type>(item);
}

}  

using enums::operator<<;

namespace detail {
CLI11_MODULE_INLINE constexpr int expected_max_vector_size{1 << 29};
CLI11_INLINE std::vector<std::string> split(const std::string& s, char delim);

template <typename T>
std::string join(const T& v, std::string delim = ",") {
    std::ostringstream s;
    auto beg = std::begin(v);
    auto end = std::end(v);
    if (beg != end)
        s << *beg++;
    while (beg != end) {
        s << delim << *beg++;
    }
    auto rval = s.str();
    if (!rval.empty() && delim.size() == 1 && rval.back() == delim[0]) {
        rval.pop_back();
    }
    return rval;
}

template <typename T, typename Callable,
          typename = typename std::enable_if<!std::is_constructible<std::string, Callable>::value>::type>
std::string join(const T& v, Callable func, std::string delim = ",") {
    std::ostringstream s;
    auto beg = std::begin(v);
    auto end = std::end(v);
    auto loc = s.tellp();
    while (beg != end) {
        auto nloc = s.tellp();
        if (nloc > loc) {
            s << delim;
            loc = nloc;
        }
        s << func(*beg++);
    }
    return s.str();
}

template <typename T>
std::string rjoin(const T& v, std::string delim = ",") {
    std::ostringstream s;
    for (std::size_t start = 0; start < v.size(); start++) {
        if (start > 0)
            s << delim;
        s << v[v.size() - start - 1];
    }
    return s.str();
}

CLI11_INLINE std::string& ltrim(std::string& str);

CLI11_INLINE std::string& ltrim(std::string& str, const std::string& filter);

CLI11_INLINE std::string& rtrim(std::string& str);

CLI11_INLINE std::string& rtrim(std::string& str, const std::string& filter);

inline std::string& trim(std::string& str) {
    return ltrim(rtrim(str));
}

inline std::string& trim(std::string& str, const std::string filter) {
    return ltrim(rtrim(str, filter), filter);
}

inline std::string trim_copy(const std::string& str) {
    std::string s = str;
    return trim(s);
}

CLI11_INLINE std::string& remove_quotes(std::string& str);

CLI11_INLINE void remove_quotes(std::vector<std::string>& args);

CLI11_INLINE std::string fix_newlines(const std::string& leader, std::string input);

inline std::string trim_copy(const std::string& str, const std::string& filter) {
    std::string s = str;
    return trim(s, filter);
}

CLI11_INLINE std::ostream& format_aliases(std::ostream& out, const std::vector<std::string>& aliases, std::size_t wid);

template <typename T>
bool valid_first_char(T c) {
    return ((c != '-') && (static_cast<unsigned char>(c) > 33));
}

template <typename T>
bool valid_later_char(T c) {
    return ((c != '=') && (c != ':') && (c != '{') && ((static_cast<unsigned char>(c) > 32) || c == '\t'));
}

CLI11_INLINE bool valid_name_string(const std::string& str);

inline bool valid_alias_name_string(const std::string& str) {
    return ((str.find_first_of('\n') == std::string::npos) && (str.find_first_of('\0') == std::string::npos));
}

inline bool is_separator(const std::string& str) {
    return (str.empty() || (str.size() == 2 && str[0] == '%' && str[1] == '%'));
}

inline bool isalpha(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) { return std::isalpha(c, std::locale()); });
}

inline std::string to_lower(std::string str) {
    std::transform(std::begin(str), std::end(str), std::begin(str),
                   [](const std::string::value_type& x) { return std::tolower(x, std::locale()); });
    return str;
}

inline std::string remove_underscore(std::string str) {
    str.erase(std::remove(std::begin(str), std::end(str), '_'), std::end(str));
    return str;
}

CLI11_INLINE std::string get_group_separators();

CLI11_INLINE std::string find_and_replace(std::string str, std::string from, std::string to);

inline bool has_default_flag_values(const std::string& flags) {
    return (flags.find_first_of("{!") != std::string::npos);
}

CLI11_INLINE void remove_default_flag_values(std::string& flags);

CLI11_INLINE std::ptrdiff_t find_member(std::string name, const std::vector<std::string> names,
                                        bool ignore_case = false, bool ignore_underscore = false);

template <typename Callable>
inline std::string find_and_modify(std::string str, std::string trigger, Callable modify) {
    std::size_t start_pos = 0;
    while ((start_pos = str.find(trigger, start_pos)) != std::string::npos) {
        start_pos = modify(str, start_pos);
    }
    return str;
}

CLI11_INLINE std::size_t close_sequence(const std::string& str, std::size_t start, char closure_char);

CLI11_INLINE std::vector<std::string> split_up(std::string str, char delimiter = '\0');

CLI11_INLINE std::string get_environment_value(const std::string& env_name);

CLI11_INLINE std::size_t escape_detect(std::string& str, std::size_t offset);

CLI11_INLINE bool has_escapable_character(const std::string& str);

CLI11_INLINE std::string add_escaped_characters(const std::string& str);

CLI11_INLINE std::string remove_escaped_characters(const std::string& str);

CLI11_INLINE std::string binary_escape_string(const std::string& string_to_escape, bool force = false);

CLI11_INLINE bool is_binary_escaped_string(const std::string& escaped_string);

CLI11_INLINE std::string extract_binary_string(const std::string& escaped_string);

CLI11_INLINE bool process_quoted_string(std::string& str, char string_char = '\"', char literal_char = '\'',
                                        bool disable_secondary_array_processing = false);

CLI11_INLINE std::ostream& streamOutAsParagraph(std::ostream& out, const std::string& text, std::size_t paragraphWidth,
                                                const std::string& linePrefix = "", bool skipPrefixOnFirstLine = false);

}  

namespace detail {
CLI11_INLINE std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> elems;
    if (s.empty()) {
        elems.emplace_back();
    } else {
        std::stringstream ss;
        ss.str(s);
        std::string item;
        while (std::getline(ss, item, delim)) {
            elems.push_back(item);
        }
    }
    return elems;
}

CLI11_INLINE std::string& ltrim(std::string& str) {
    auto it = std::find_if(str.begin(), str.end(), [](char ch) { return !std::isspace<char>(ch, std::locale()); });
    str.erase(str.begin(), it);
    return str;
}

CLI11_INLINE std::string& ltrim(std::string& str, const std::string& filter) {
    auto it = std::find_if(str.begin(), str.end(), [&filter](char ch) { return filter.find(ch) == std::string::npos; });
    str.erase(str.begin(), it);
    return str;
}

CLI11_INLINE std::string& rtrim(std::string& str) {
    auto it = std::find_if(str.rbegin(), str.rend(), [](char ch) { return !std::isspace<char>(ch, std::locale()); });
    str.erase(it.base(), str.end());
    return str;
}

CLI11_INLINE std::string& rtrim(std::string& str, const std::string& filter) {
    auto it =
        std::find_if(str.rbegin(), str.rend(), [&filter](char ch) { return filter.find(ch) == std::string::npos; });
    str.erase(it.base(), str.end());
    return str;
}

CLI11_INLINE std::string& remove_quotes(std::string& str) {
    if (str.length() > 1 && (str.front() == '"' || str.front() == '\'' || str.front() == '`')) {
        if (str.front() == str.back()) {
            str.pop_back();
            str.erase(str.begin(), str.begin() + 1);
        }
    }
    return str;
}

CLI11_INLINE std::string& remove_outer(std::string& str, char key) {
    if (str.length() > 1 && (str.front() == key)) {
        if (str.front() == str.back()) {
            str.pop_back();
            str.erase(str.begin(), str.begin() + 1);
        }
    }
    return str;
}

CLI11_INLINE std::string fix_newlines(const std::string& leader, std::string input) {
    std::string::size_type n = 0;
    while (n != std::string::npos && n < input.size()) {
        n = input.find_first_of("\r\n", n);
        if (n != std::string::npos) {
            input = input.substr(0, n + 1) + leader + input.substr(n + 1);
            n += leader.size();
        }
    }
    return input;
}

CLI11_INLINE std::ostream& format_aliases(std::ostream& out, const std::vector<std::string>& aliases, std::size_t wid) {
    if (!aliases.empty()) {
        out << std::setw(static_cast<int>(wid)) << "     aliases: ";
        bool front = true;
        for (const auto& alias : aliases) {
            if (!front) {
                out << ", ";
            } else {
                front = false;
            }
            out << detail::fix_newlines("              ", alias);
        }
        out << "\n";
    }
    return out;
}

CLI11_INLINE bool valid_name_string(const std::string& str) {
    if (str.empty() || !valid_first_char(str[0])) {
        return false;
    }
    auto e = str.end();
    for (auto c = str.begin() + 1; c != e; ++c)
        if (!valid_later_char(*c))
            return false;
    return true;
}

CLI11_INLINE std::string get_group_separators() {
    std::string separators{"_'"};
#if CLI11_HAS_RTTI != 0
    char group_separator = std::use_facet<std::numpunct<char>>(std::locale()).thousands_sep();
    separators.push_back(group_separator);
#endif
    return separators;
}

CLI11_INLINE std::string find_and_replace(std::string str, std::string from, std::string to) {
    std::size_t start_pos = 0;

    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }

    return str;
}

CLI11_INLINE void remove_default_flag_values(std::string& flags) {
    auto loc = flags.find_first_of('{', 2);
    while (loc != std::string::npos) {
        auto finish = flags.find_first_of("},", loc + 1);
        if ((finish != std::string::npos) && (flags[finish] == '}')) {
            flags.erase(flags.begin() + static_cast<std::ptrdiff_t>(loc),
                        flags.begin() + static_cast<std::ptrdiff_t>(finish) + 1);
        }
        loc = flags.find_first_of('{', loc + 1);
    }
    flags.erase(std::remove(flags.begin(), flags.end(), '!'), flags.end());
}

CLI11_INLINE std::ptrdiff_t find_member(std::string name, const std::vector<std::string> names, bool ignore_case,
                                        bool ignore_underscore) {
    auto it = std::end(names);
    if (ignore_case) {
        if (ignore_underscore) {
            name = detail::to_lower(detail::remove_underscore(name));
            it = std::find_if(std::begin(names), std::end(names), [&name](std::string local_name) {
                return detail::to_lower(detail::remove_underscore(local_name)) == name;
            });
        } else {
            name = detail::to_lower(name);
            it = std::find_if(std::begin(names), std::end(names),
                              [&name](std::string local_name) { return detail::to_lower(local_name) == name; });
        }

    } else if (ignore_underscore) {
        name = detail::remove_underscore(name);
        it = std::find_if(std::begin(names), std::end(names),
                          [&name](std::string local_name) { return detail::remove_underscore(local_name) == name; });
    } else {
        it = std::find(std::begin(names), std::end(names), name);
    }

    return (it != std::end(names)) ? (it - std::begin(names)) : (-1);
}

CLI11_MODULE_INLINE const std::string& escapedChars("\b\t\n\f\r\"\\");
CLI11_MODULE_INLINE const std::string& escapedCharsCode("btnfr\"\\");
CLI11_MODULE_INLINE const std::string& bracketChars("\"'`[(<{");
CLI11_MODULE_INLINE const std::string& matchBracketChars("\"'`])>}");

CLI11_INLINE bool has_escapable_character(const std::string& str) {
    return (str.find_first_of(escapedChars) != std::string::npos);
}

CLI11_INLINE std::string add_escaped_characters(const std::string& str) {
    std::string out;
    out.reserve(str.size() + 4);
    for (char s : str) {
        auto sloc = escapedChars.find_first_of(s);
        if (sloc != std::string::npos) {
            out.push_back('\\');
            out.push_back(escapedCharsCode[sloc]);
        } else {
            out.push_back(s);
        }
    }
    return out;
}

CLI11_INLINE std::uint32_t hexConvert(char hc) {
    int hcode{0};
    if (hc >= '0' && hc <= '9') {
        hcode = (hc - '0');
    } else if (hc >= 'A' && hc <= 'F') {
        hcode = (hc - 'A' + 10);
    } else if (hc >= 'a' && hc <= 'f') {
        hcode = (hc - 'a' + 10);
    } else {
        hcode = -1;
    }
    return static_cast<uint32_t>(hcode);
}

CLI11_INLINE char make_char(std::uint32_t code) {
    return static_cast<char>(static_cast<unsigned char>(code));
}

CLI11_INLINE void append_codepoint(std::string& str, std::uint32_t code) {
    if (code < 0x80) {
        str.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        str.push_back(make_char(0xC0 | code >> 6));
        str.push_back(make_char(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        if (0xD800 <= code && code <= 0xDFFF) {
            throw std::invalid_argument("[0xD800, 0xDFFF] are not valid UTF-8.");
        }
        str.push_back(make_char(0xE0 | code >> 12));
        str.push_back(make_char(0x80 | (code >> 6 & 0x3F)));
        str.push_back(make_char(0x80 | (code & 0x3F)));
    } else if (code < 0x110000) {
        str.push_back(make_char(0xF0 | code >> 18));
        str.push_back(make_char(0x80 | (code >> 12 & 0x3F)));
        str.push_back(make_char(0x80 | (code >> 6 & 0x3F)));
        str.push_back(make_char(0x80 | (code & 0x3F)));
    }
}

CLI11_INLINE std::string remove_escaped_characters(const std::string& str) {
    std::string out;
    out.reserve(str.size());
    for (auto loc = str.begin(); loc < str.end(); ++loc) {
        if (*loc == '\\') {
            if (str.end() - loc < 2) {
                throw std::invalid_argument("invalid escape sequence " + str);
            }
            auto ecloc = escapedCharsCode.find_first_of(*(loc + 1));
            if (ecloc != std::string::npos) {
                out.push_back(escapedChars[ecloc]);
                ++loc;
            } else if (*(loc + 1) == 'u') {
                if (str.end() - loc < 6) {
                    throw std::invalid_argument("unicode sequence must have 4 hex codes " + str);
                }
                std::uint32_t code{0};
                std::uint32_t mplier{16 * 16 * 16};
                for (int ii = 2; ii < 6; ++ii) {
                    std::uint32_t res = hexConvert(*(loc + ii));
                    if (res > 0x0F) {
                        throw std::invalid_argument("unicode sequence must have 4 hex codes " + str);
                    }
                    code += res * mplier;
                    mplier = mplier / 16;
                }
                append_codepoint(out, code);
                loc += 5;
            } else if (*(loc + 1) == 'U') {
                if (str.end() - loc < 10) {
                    throw std::invalid_argument("unicode sequence must have 8 hex codes " + str);
                }
                std::uint32_t code{0};
                std::uint32_t mplier{16 * 16 * 16 * 16 * 16 * 16 * 16};
                for (int ii = 2; ii < 10; ++ii) {
                    std::uint32_t res = hexConvert(*(loc + ii));
                    if (res > 0x0F) {
                        throw std::invalid_argument("unicode sequence must have 8 hex codes " + str);
                    }
                    code += res * mplier;
                    mplier = mplier / 16;
                }
                append_codepoint(out, code);
                loc += 9;
            } else if (*(loc + 1) == '0') {
                out.push_back('\0');
                ++loc;
            } else {
                throw std::invalid_argument(std::string("unrecognized escape sequence \\") + *(loc + 1) + " in " + str);
            }
        } else {
            out.push_back(*loc);
        }
    }
    return out;
}

CLI11_INLINE std::size_t close_string_quote(const std::string& str, std::size_t start, char closure_char) {
    std::size_t loc{0};
    for (loc = start + 1; loc < str.size(); ++loc) {
        if (str[loc] == closure_char) {
            break;
        }
        if (str[loc] == '\\') {
            ++loc;
        }
    }
    return loc;
}

CLI11_INLINE std::size_t close_literal_quote(const std::string& str, std::size_t start, char closure_char) {
    auto loc = str.find_first_of(closure_char, start + 1);
    return (loc != std::string::npos ? loc : str.size());
}

CLI11_INLINE std::size_t close_sequence(const std::string& str, std::size_t start, char closure_char) {
    auto bracket_loc = matchBracketChars.find(closure_char);
    switch (bracket_loc) {
        case 0:
            return close_string_quote(str, start, closure_char);
        case 1:
        case 2:
#if defined(_MSC_VER) && _MSC_VER < 1920
        case (std::size_t)-1:
#else
        case std::string::npos:
#endif
            return close_literal_quote(str, start, closure_char);
        default:
            break;
    }

    std::string closures(1, closure_char);
    auto loc = start + 1;

    while (loc < str.size()) {
        if (str[loc] == closures.back()) {
            closures.pop_back();
            if (closures.empty()) {
                return loc;
            }
        }
        bracket_loc = bracketChars.find(str[loc]);
        if (bracket_loc != std::string::npos) {
            switch (bracket_loc) {
                case 0:
                    loc = close_string_quote(str, loc, str[loc]);
                    break;
                case 1:
                case 2:
                    loc = close_literal_quote(str, loc, str[loc]);
                    break;
                default:
                    closures.push_back(matchBracketChars[bracket_loc]);
                    break;
            }
        }
        ++loc;
    }
    if (loc > str.size()) {
        loc = str.size();
    }
    return loc;
}

CLI11_INLINE std::vector<std::string> split_up(std::string str, char delimiter) {
    auto find_ws = [delimiter](char ch) {
        return (delimiter == '\0') ? std::isspace<char>(ch, std::locale()) : (ch == delimiter);
    };
    trim(str);

    std::vector<std::string> output;
    while (!str.empty()) {
        if (bracketChars.find_first_of(str[0]) != std::string::npos) {
            auto bracketLoc = bracketChars.find_first_of(str[0]);
            auto end = close_sequence(str, 0, matchBracketChars[bracketLoc]);
            if (end >= str.size()) {
                output.push_back(std::move(str));
                str.clear();
            } else {
                output.push_back(str.substr(0, end + 1));
                if (end + 2 < str.size()) {
                    str = str.substr(end + 2);
                } else {
                    str.clear();
                }
            }

        } else {
            auto it = std::find_if(std::begin(str), std::end(str), find_ws);
            if (it != std::end(str)) {
                std::string value = std::string(str.begin(), it);
                output.push_back(value);
                str = std::string(it + 1, str.end());
            } else {
                output.push_back(str);
                str.clear();
            }
        }
        trim(str);
    }
    return output;
}

CLI11_INLINE std::size_t escape_detect(std::string& str, std::size_t offset) {
    auto next = str[offset + 1];
    if ((next == '\"') || (next == '\'') || (next == '`')) {
        auto astart = str.find_last_of("-/ \"\'`", offset - 1);
        if (astart != std::string::npos) {
            if (str[astart] == ((str[offset] == '=') ? '-' : '/'))
                str[offset] = ' ';
        }
    }
    return offset + 1;
}

CLI11_INLINE std::string binary_escape_string(const std::string& string_to_escape, bool force) {
    std::string escaped_string{};
    for (char c : string_to_escape) {
        if (isprint(static_cast<unsigned char>(c)) == 0) {
            std::stringstream stream;
            stream << std::hex << static_cast<unsigned int>(static_cast<unsigned char>(c));
            std::string code = stream.str();
            escaped_string += std::string("\\x") + (code.size() < 2 ? "0" : "") + code;
        } else if (c == 'x' || c == 'X') {
            if (!escaped_string.empty() && escaped_string.back() == '\\') {
                escaped_string += std::string("\\x") + (c == 'x' ? "78" : "58");
            } else {
                escaped_string.push_back(c);
            }

        } else {
            escaped_string.push_back(c);
        }
    }
    if (escaped_string != string_to_escape || force) {
        auto sqLoc = escaped_string.find('\'');
        while (sqLoc != std::string::npos) {
            escaped_string[sqLoc] = '\\';
            escaped_string.insert(sqLoc + 1, "x27");
            sqLoc = escaped_string.find('\'');
        }
        escaped_string.insert(0, "'B\"(");
        escaped_string.push_back(')');
        escaped_string.push_back('"');
        escaped_string.push_back('\'');
    }
    return escaped_string;
}

CLI11_INLINE bool is_binary_escaped_string(const std::string& escaped_string) {
    size_t ssize = escaped_string.size();
    if (escaped_string.compare(0, 3, "B\"(") == 0 && escaped_string.compare(ssize - 2, 2, ")\"") == 0) {
        return true;
    }
    return (escaped_string.compare(0, 4, "'B\"(") == 0 && escaped_string.compare(ssize - 3, 3, ")\"'") == 0);
}

CLI11_INLINE std::string extract_binary_string(const std::string& escaped_string) {
    std::size_t start{0};
    std::size_t tail{0};
    size_t ssize = escaped_string.size();
    if (escaped_string.compare(0, 3, "B\"(") == 0 && escaped_string.compare(ssize - 2, 2, ")\"") == 0) {
        start = 3;
        tail = 2;
    } else if (escaped_string.compare(0, 4, "'B\"(") == 0 && escaped_string.compare(ssize - 3, 3, ")\"'") == 0) {
        start = 4;
        tail = 3;
    }

    if (start == 0) {
        return escaped_string;
    }
    std::string outstring;

    outstring.reserve(ssize - start - tail);
    std::size_t loc = start;
    while (loc < ssize - tail) {
        if (escaped_string[loc] == '\\' && (escaped_string[loc + 1] == 'x' || escaped_string[loc + 1] == 'X')) {
            auto c1 = escaped_string[loc + 2];
            auto c2 = escaped_string[loc + 3];

            std::uint32_t res1 = hexConvert(c1);
            std::uint32_t res2 = hexConvert(c2);
            if (res1 <= 0x0F && res2 <= 0x0F) {
                loc += 4;
                outstring.push_back(static_cast<char>(res1 * 16 + res2));
                continue;
            }
        }
        outstring.push_back(escaped_string[loc]);
        ++loc;
    }
    return outstring;
}

CLI11_INLINE void remove_quotes(std::vector<std::string>& args) {
    for (auto& arg : args) {
        if (arg.front() == '\"' && arg.back() == '\"') {
            remove_quotes(arg);
            arg = remove_escaped_characters(arg);
        } else {
            remove_quotes(arg);
        }
    }
}

CLI11_INLINE void handle_secondary_array(std::string& str) {
    if (str.size() >= 2 && str.front() == '[' && str.back() == ']') {
        std::string tstr{"[["};
        for (std::size_t ii = 1; ii < str.size(); ++ii) {
            tstr.push_back(str[ii]);
            tstr.push_back(str[ii]);
        }
        str = std::move(tstr);
    }
}

CLI11_INLINE bool process_quoted_string(std::string& str, char string_char, char literal_char,
                                        bool disable_secondary_array_processing) {
    if (str.size() <= 1) {
        return false;
    }
    if (detail::is_binary_escaped_string(str)) {
        str = detail::extract_binary_string(str);
        if (!disable_secondary_array_processing)
            handle_secondary_array(str);
        return true;
    }
    if (str.front() == string_char && str.back() == string_char) {
        detail::remove_outer(str, string_char);
        if (str.find_first_of('\\') != std::string::npos) {
            str = detail::remove_escaped_characters(str);
        }
        if (!disable_secondary_array_processing)
            handle_secondary_array(str);
        return true;
    }
    if ((str.front() == literal_char || str.front() == '`') && str.back() == str.front()) {
        detail::remove_outer(str, str.front());
        if (!disable_secondary_array_processing)
            handle_secondary_array(str);
        return true;
    }
    return false;
}

std::string get_environment_value(const std::string& env_name) {
    std::string ename_string;

#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t sz = 0;
    if (_dupenv_s(&buffer, &sz, env_name.c_str()) == 0 && buffer != nullptr) {
        ename_string = std::string(buffer);
        free(buffer);
    }
#else

    const char* buffer = nullptr;
    buffer = std::getenv(env_name.c_str());
    if (buffer != nullptr) {
        ename_string = std::string(buffer);
    }
#endif
    return ename_string;
}

CLI11_INLINE std::ostream& streamOutAsParagraph(std::ostream& out, const std::string& text, std::size_t paragraphWidth,
                                                const std::string& linePrefix, bool skipPrefixOnFirstLine) {
    if (!skipPrefixOnFirstLine)
        out << linePrefix;

    std::istringstream lss(text);
    std::string line = "";
    while (std::getline(lss, line)) {
        std::istringstream iss(line);
        std::string word = "";
        std::size_t charsWritten = 0;

        while (iss >> word) {
            if (charsWritten > 0 && (word.length() + 1 + charsWritten > paragraphWidth)) {
                out << '\n' << linePrefix;
                charsWritten = 0;
            }
            if (charsWritten == 0) {
                out << word;
                charsWritten += word.length();
            } else {
                out << ' ' << word;
                charsWritten += word.length() + 1;
            }
        }

        if (!lss.eof())
            out << '\n' << linePrefix;
    }
    return out;
}

}  

#define CLI11_ERROR_DEF(parent, name)                                                                                \
   protected:                                                                                                        \
    name(std::string ename, std::string msg, int exit_code) : parent(std::move(ename), std::move(msg), exit_code) {} \
    name(std::string ename, std::string msg, ExitCodes exit_code)                                                    \
        : parent(std::move(ename), std::move(msg), exit_code) {}                                                     \
                                                                                                                     \
   public:                                                                                                           \
    name(std::string msg, ExitCodes exit_code) : parent(#name, std::move(msg), exit_code) {}                         \
    name(std::string msg, int exit_code) : parent(#name, std::move(msg), exit_code) {}

#define CLI11_ERROR_SIMPLE(name) \
    explicit name(std::string msg) : name(#name, msg, ExitCodes::name) {}

enum class ExitCodes : int {
    Success = 0,
    IncorrectConstruction = 100,
    BadNameString,
    OptionAlreadyAdded,
    FileError,
    ConversionError,
    ValidationError,
    RequiredError,
    RequiresError,
    ExcludesError,
    ExtrasError,
    ConfigError,
    InvalidError,
    HorribleError,
    OptionNotFound,
    ArgumentMismatch,
    BaseClass = 127
};

class Error : public std::runtime_error {
    int actual_exit_code;
    std::string error_name{"Error"};

   public:
    CLI11_NODISCARD int get_exit_code() const {
        return actual_exit_code;
    }

    CLI11_NODISCARD std::string get_name() const {
        return error_name;
    }

    Error(std::string name, std::string msg, int exit_code = static_cast<int>(ExitCodes::BaseClass))
        : runtime_error(msg), actual_exit_code(exit_code), error_name(std::move(name)) {}

    Error(std::string name, std::string msg, ExitCodes exit_code) : Error(name, msg, static_cast<int>(exit_code)) {}
};

class ConstructionError : public Error {
    CLI11_ERROR_DEF(Error, ConstructionError)
};

class IncorrectConstruction : public ConstructionError {
    CLI11_ERROR_DEF(ConstructionError, IncorrectConstruction)
    CLI11_ERROR_SIMPLE(IncorrectConstruction)
    static IncorrectConstruction PositionalFlag(std::string name) {
        return IncorrectConstruction(name + ": Flags cannot be positional");
    }
    static IncorrectConstruction Set0Opt(std::string name) {
        return IncorrectConstruction(name + ": Cannot set 0 expected, use a flag instead");
    }
    static IncorrectConstruction SetFlag(std::string name) {
        return IncorrectConstruction(name + ": Cannot set an expected number for flags");
    }
    static IncorrectConstruction ChangeNotVector(std::string name) {
        return IncorrectConstruction(name + ": You can only change the expected arguments for vectors");
    }
    static IncorrectConstruction AfterMultiOpt(std::string name) {
        return IncorrectConstruction(
            name + ": You can't change expected arguments after you've changed the multi option policy!");
    }
    static IncorrectConstruction MissingOption(std::string name) {
        return IncorrectConstruction("Option " + name + " is not defined");
    }
    static IncorrectConstruction MultiOptionPolicy(std::string name) {
        return IncorrectConstruction(name + ": multi_option_policy only works for flags and exact value options");
    }
};

class BadNameString : public ConstructionError {
    CLI11_ERROR_DEF(ConstructionError, BadNameString)
    CLI11_ERROR_SIMPLE(BadNameString)
    static BadNameString OneCharName(std::string name) {
        return BadNameString("Invalid one char name: " + name);
    }
    static BadNameString MissingDash(std::string name) {
        return BadNameString("Long names strings require 2 dashes " + name);
    }
    static BadNameString BadLongName(std::string name) {
        return BadNameString("Bad long name: " + name);
    }
    static BadNameString BadPositionalName(std::string name) {
        return BadNameString("Invalid positional Name: " + name);
    }
    static BadNameString ReservedName(std::string name) {
        return BadNameString("Names '-','--','++' are reserved and not allowed as option names " + name);
    }
    static BadNameString MultiPositionalNames(std::string name) {
        return BadNameString("Only one positional name allowed, remove: " + name);
    }
};

class OptionAlreadyAdded : public ConstructionError {
    CLI11_ERROR_DEF(ConstructionError, OptionAlreadyAdded)
    explicit OptionAlreadyAdded(std::string name)
        : OptionAlreadyAdded(name + " is already added", ExitCodes::OptionAlreadyAdded) {}
    static OptionAlreadyAdded Requires(std::string name, std::string other) {
        return {name + " requires " + other, ExitCodes::OptionAlreadyAdded};
    }
    static OptionAlreadyAdded Excludes(std::string name, std::string other) {
        return {name + " excludes " + other, ExitCodes::OptionAlreadyAdded};
    }
};

class ParseError : public Error {
    CLI11_ERROR_DEF(Error, ParseError)
};

class Success : public ParseError {
    CLI11_ERROR_DEF(ParseError, Success)
    Success() : Success("Successfully completed, should be caught and quit", ExitCodes::Success) {}
};

class CallForHelp : public Success {
    CLI11_ERROR_DEF(Success, CallForHelp)
    CallForHelp() : CallForHelp("This should be caught in your main function, see examples", ExitCodes::Success) {}
};

class CallForAllHelp : public Success {
    CLI11_ERROR_DEF(Success, CallForAllHelp)
    CallForAllHelp()
        : CallForAllHelp("This should be caught in your main function, see examples", ExitCodes::Success) {}
};

class CallForVersion : public Success {
    CLI11_ERROR_DEF(Success, CallForVersion)
    CallForVersion()
        : CallForVersion("This should be caught in your main function, see examples", ExitCodes::Success) {}
};

class RuntimeError : public ParseError {
    CLI11_ERROR_DEF(ParseError, RuntimeError)
    explicit RuntimeError(int exit_code = 1) : RuntimeError("Runtime error", exit_code) {}
};

class FileError : public ParseError {
    CLI11_ERROR_DEF(ParseError, FileError)
    CLI11_ERROR_SIMPLE(FileError)
    static FileError Missing(std::string name) {
        return FileError(name + " was not readable (missing?)");
    }
};

class ConversionError : public ParseError {
    CLI11_ERROR_DEF(ParseError, ConversionError)
    CLI11_ERROR_SIMPLE(ConversionError)
    ConversionError(std::string member, std::string name)
        : ConversionError("The value " + member + " is not an allowed value for " + name) {}
    ConversionError(std::string name, std::vector<std::string> results)
        : ConversionError("Could not convert: " + name + " = " + detail::join(results)) {}
    static ConversionError TooManyInputsFlag(std::string name) {
        return ConversionError(name + ": too many inputs for a flag");
    }
    static ConversionError TrueFalse(std::string name) {
        return ConversionError(name + ": Should be true/false or a number");
    }
};

class ValidationError : public ParseError {
    CLI11_ERROR_DEF(ParseError, ValidationError)
    CLI11_ERROR_SIMPLE(ValidationError)
    explicit ValidationError(std::string name, std::string msg) : ValidationError(name + ": " + msg) {}
};

class RequiredError : public ParseError {
    CLI11_ERROR_DEF(ParseError, RequiredError)
    explicit RequiredError(std::string name) : RequiredError(name + " is required", ExitCodes::RequiredError) {}
    static RequiredError Subcommand(std::size_t min_subcom) {
        if (min_subcom == 1) {
            return RequiredError("A subcommand");
        }
        return {"Requires at least " + std::to_string(min_subcom) + " subcommands", ExitCodes::RequiredError};
    }
    static RequiredError Option(std::size_t min_option, std::size_t max_option, std::size_t used,
                                const std::string& option_list) {
        if ((min_option == 1) && (max_option == 1) && (used == 0))
            return RequiredError("Exactly 1 option from [" + option_list + "]");
        if ((min_option == 1) && (max_option == 1) && (used > 1)) {
            return {
                "Exactly 1 option from [" + option_list + "] is required but " + std::to_string(used) + " were given",
                ExitCodes::RequiredError};
        }
        if ((min_option == 1) && (used == 0))
            return RequiredError("At least 1 option from [" + option_list + "]");
        if (used < min_option) {
            return {"Requires at least " + std::to_string(min_option) + " options used but only " +
                        std::to_string(used) + " were given from [" + option_list + "]",
                    ExitCodes::RequiredError};
        }
        if (max_option == 1)
            return {"Requires at most 1 options be given from [" + option_list + "]", ExitCodes::RequiredError};

        return {"Requires at most " + std::to_string(max_option) + " options be used but " + std::to_string(used) +
                    " were given from [" + option_list + "]",
                ExitCodes::RequiredError};
    }
};

class ArgumentMismatch : public ParseError {
    CLI11_ERROR_DEF(ParseError, ArgumentMismatch)
    CLI11_ERROR_SIMPLE(ArgumentMismatch)
    ArgumentMismatch(std::string name, int expected, std::size_t received)
        : ArgumentMismatch(expected > 0 ? ("Expected exactly " + std::to_string(expected) + " arguments to " + name +
                                           ", got " + std::to_string(received))
                                        : ("Expected at least " + std::to_string(-expected) + " arguments to " + name +
                                           ", got " + std::to_string(received)),
                           ExitCodes::ArgumentMismatch) {}

    static ArgumentMismatch AtLeast(std::string name, int num, std::size_t received) {
        return ArgumentMismatch(name + ": At least " + std::to_string(num) + " required but received " +
                                std::to_string(received));
    }
    static ArgumentMismatch AtMost(std::string name, int num, std::size_t received) {
        return ArgumentMismatch(name + ": At most " + std::to_string(num) + " required but received " +
                                std::to_string(received));
    }
    static ArgumentMismatch TypedAtLeast(std::string name, int num, std::string type) {
        return ArgumentMismatch(name + ": " + std::to_string(num) + " required " + type + " missing");
    }
    static ArgumentMismatch FlagOverride(std::string name) {
        return ArgumentMismatch(name + " was given a disallowed flag override");
    }
    static ArgumentMismatch PartialType(std::string name, int num, std::string type) {
        return ArgumentMismatch(name + ": " + type + " only partially specified: " + std::to_string(num) +
                                " required for each element");
    }
};

class RequiresError : public ParseError {
    CLI11_ERROR_DEF(ParseError, RequiresError)
    RequiresError(std::string curname, std::string subname)
        : RequiresError(curname + " requires " + subname, ExitCodes::RequiresError) {}
};

class ExcludesError : public ParseError {
    CLI11_ERROR_DEF(ParseError, ExcludesError)
    ExcludesError(std::string curname, std::string subname)
        : ExcludesError(curname + " excludes " + subname, ExitCodes::ExcludesError) {}
};

class ExtrasError : public ParseError {
    CLI11_ERROR_DEF(ParseError, ExtrasError)
    explicit ExtrasError(std::vector<std::string> args)
        : ExtrasError((args.size() > 1 ? "The following arguments were not expected: "
                                       : "The following argument was not expected: ") +
                          detail::join(args, " "),
                      ExitCodes::ExtrasError) {}
    ExtrasError(const std::string& name, std::vector<std::string> args)
        : ExtrasError(name,
                      (args.size() > 1 ? "The following arguments were not expected: "
                                       : "The following argument was not expected: ") +
                          detail::join(args, " "),
                      ExitCodes::ExtrasError) {}
};

class ConfigError : public ParseError {
    CLI11_ERROR_DEF(ParseError, ConfigError)
    CLI11_ERROR_SIMPLE(ConfigError)
    static ConfigError Extras(std::string item) {
        return ConfigError("INI was not able to parse " + item);
    }
    static ConfigError NotConfigurable(std::string item) {
        return ConfigError(item + ": This option is not allowed in a configuration file");
    }
};

class InvalidError : public ParseError {
    CLI11_ERROR_DEF(ParseError, InvalidError)
    explicit InvalidError(std::string name)
        : InvalidError(name + ": Too many positional arguments with unlimited expected args", ExitCodes::InvalidError) {
    }
};

class HorribleError : public ParseError {
    CLI11_ERROR_DEF(ParseError, HorribleError)
    CLI11_ERROR_SIMPLE(HorribleError)
};

class OptionNotFound : public Error {
    CLI11_ERROR_DEF(Error, OptionNotFound)
    explicit OptionNotFound(std::string name) : OptionNotFound(name + " not found", ExitCodes::OptionNotFound) {}
};

#undef CLI11_ERROR_DEF
#undef CLI11_ERROR_SIMPLE

namespace detail {
enum class enabler : std::uint8_t {
};

CLI11_MODULE_INLINE constexpr enabler dummy = {};
}  

template <bool B, class T = void>
using enable_if_t = typename std::enable_if<B, T>::type;

template <typename... Ts>
struct make_void {
    using type = void;
};

template <typename... Ts>
using void_t = typename make_void<Ts...>::type;

template <bool B, class T, class F>
using conditional_t = typename std::conditional<B, T, F>::type;

template <typename T>
struct is_bool : std::false_type {};

template <>
struct is_bool<bool> : std::true_type {};

template <typename T>
struct is_shared_ptr : std::false_type {};

template <typename T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};

template <typename T>
struct is_shared_ptr<const std::shared_ptr<T>> : std::true_type {};

template <typename T>
struct is_copyable_ptr {
    static bool const value = is_shared_ptr<T>::value || std::is_pointer<T>::value;
};

template <typename T>
struct IsMemberType {
    using type = T;
};

template <>
struct IsMemberType<const char*> {
    using type = std::string;
};

namespace adl_detail {
template <typename T, typename S = std::string>
class is_lexical_castable {
    template <typename TT, typename SS>
    static auto test(int) -> decltype(lexical_cast(std::declval<const SS&>(), std::declval<TT&>()), std::true_type());

    template <typename, typename>
    static auto test(...) -> std::false_type;

   public:
    static constexpr bool value = decltype(test<T, S>(0))::value;
};
}  

namespace detail {

template <typename T, typename Enable = void>
struct element_type {
    using type = T;
};

template <typename T>
struct element_type<T, typename std::enable_if<is_copyable_ptr<T>::value>::type> {
    using type = typename std::pointer_traits<T>::element_type;
};

template <typename T>
struct element_value_type {
    using type = typename element_type<T>::type::value_type;
};

template <typename T, typename _ = void>
struct pair_adaptor : std::false_type {
    using value_type = typename T::value_type;
    using first_type = typename std::remove_const<value_type>::type;
    using second_type = typename std::remove_const<value_type>::type;

    template <typename Q>
    static auto first(Q&& pair_value) -> decltype(std::forward<Q>(pair_value)) {
        return std::forward<Q>(pair_value);
    }
    template <typename Q>
    static auto second(Q&& pair_value) -> decltype(std::forward<Q>(pair_value)) {
        return std::forward<Q>(pair_value);
    }
};

template <typename T>
struct pair_adaptor<
    T, conditional_t<false, void_t<typename T::value_type::first_type, typename T::value_type::second_type>, void>>
    : std::true_type {
    using value_type = typename T::value_type;
    using first_type = typename std::remove_const<typename value_type::first_type>::type;
    using second_type = typename std::remove_const<typename value_type::second_type>::type;

    template <typename Q>
    static auto first(Q&& pair_value) -> decltype(std::get<0>(std::forward<Q>(pair_value))) {
        return std::get<0>(std::forward<Q>(pair_value));
    }
    template <typename Q>
    static auto second(Q&& pair_value) -> decltype(std::get<1>(std::forward<Q>(pair_value))) {
        return std::get<1>(std::forward<Q>(pair_value));
    }
};

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
#endif
template <typename T, typename C>
class is_direct_constructible {
    template <typename TT, typename CC>
    static auto test(int, std::true_type) -> decltype(
#ifdef __CUDACC__
#ifdef __NVCC_DIAG_PRAGMA_SUPPORT__
#pragma nv_diag_suppress 2361
#else
#pragma diag_suppress 2361
#endif
#endif
                                              TT{std::declval<CC>()}
#ifdef __CUDACC__
#ifdef __NVCC_DIAG_PRAGMA_SUPPORT__
#pragma nv_diag_default 2361
#else
#pragma diag_default 2361
#endif
#endif
                                              ,
                                              std::is_move_assignable<TT>());

    template <typename TT, typename CC>
    static auto test(int, std::false_type) -> std::false_type;

    template <typename, typename>
    static auto test(...) -> std::false_type;

   public:
    static constexpr bool value = decltype(test<T, C>(0, typename std::is_constructible<T, C>::type()))::value;
};
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

template <typename T, typename S = std::ostringstream>
class is_ostreamable {
    template <typename TT, typename SS>
    static auto test(int) -> decltype(std::declval<SS&>() << std::declval<TT>(), std::true_type());

    template <typename, typename>
    static auto test(...) -> std::false_type;

   public:
    static constexpr bool value = decltype(test<T, S>(0))::value;
};

template <typename T, typename S = std::istringstream>
class is_istreamable {
    template <typename TT, typename SS>
    static auto test(int) -> decltype(std::declval<SS&>() >> std::declval<TT&>(), std::true_type());

    template <typename, typename>
    static auto test(...) -> std::false_type;

   public:
    static constexpr bool value = decltype(test<T, S>(0))::value;
};

template <typename T>
class is_complex {
    template <typename TT>
    static auto test(int) -> decltype(std::declval<TT>().real(), std::declval<TT>().imag(), std::true_type());

    template <typename>
    static auto test(...) -> std::false_type;

   public:
    static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T, enable_if_t<is_istreamable<T>::value, detail::enabler> = detail::dummy>
bool from_stream(const std::string& istring, T& obj) {
    std::istringstream is;
    is.str(istring);
    is >> obj;
    return !is.fail() && !is.rdbuf()->in_avail();
}

template <typename T, enable_if_t<!is_istreamable<T>::value, detail::enabler> = detail::dummy>
bool from_stream(const std::string&, T&) {
    return false;
}

template <typename T, typename _ = void>
struct is_mutable_container : std::false_type {};

template <typename T>
struct is_mutable_container<
    T,
    conditional_t<false,
                  void_t<typename T::value_type, decltype(std::declval<T>().end()), decltype(std::declval<T>().clear()),
                         decltype(std::declval<T>().insert(std::declval<decltype(std::declval<T>().end())>(),
                                                           std::declval<const typename T::value_type&>()))>,
                  void>> : public conditional_t<std::is_constructible<T, std::string>::value ||
                                                    std::is_constructible<T, std::wstring>::value,
                                                std::false_type, std::true_type> {};

template <typename T, typename _ = void>
struct is_readable_container : std::false_type {};

template <typename T>
struct is_readable_container<
    T, conditional_t<false, void_t<decltype(std::declval<T>().end()), decltype(std::declval<T>().begin())>, void>>
    : public std::true_type {};

template <typename T, typename _ = void>
struct is_wrapper : std::false_type {};

template <typename T>
struct is_wrapper<T, conditional_t<false, void_t<typename T::value_type>, void>> : public std::true_type {};

template <typename S>
class is_tuple_like {
    template <typename SS, enable_if_t<!is_complex<SS>::value, detail::enabler> = detail::dummy>
    static auto test(int) -> decltype(std::tuple_size<typename std::decay<SS>::type>::value, std::true_type{});
    template <typename>
    static auto test(...) -> std::false_type;

   public:
    static constexpr bool value = decltype(test<S>(0))::value;
};

template <typename T, typename Enable = void>
struct type_count_base {
    static const int value{0};
};

template <typename T>
struct type_count_base<T, typename std::enable_if<!is_tuple_like<T>::value && !is_mutable_container<T>::value &&
                                                  !std::is_void<T>::value>::type> {
    static constexpr int value{1};
};

template <typename T>
struct type_count_base<T, typename std::enable_if<is_tuple_like<T>::value && !is_mutable_container<T>::value>::type> {
    static constexpr int value{// cppcheck-suppress unusedStructMember
                               std::tuple_size<typename std::decay<T>::type>::value};
};

template <typename T>
struct type_count_base<T, typename std::enable_if<is_mutable_container<T>::value>::type> {
    static constexpr int value{type_count_base<typename T::value_type>::value};
};

template <typename T, enable_if_t<std::is_convertible<T, std::string>::value, detail::enabler> = detail::dummy>
auto to_string(T&& value) -> decltype(std::forward<T>(value)) {
    return std::forward<T>(value);
}

template <typename T,
          enable_if_t<std::is_constructible<std::string, T>::value && !std::is_convertible<T, std::string>::value,
                      detail::enabler> = detail::dummy>
std::string to_string(T&& value) {
    return std::string(value);  // NOLINT(google-readability-casting)
}

template <typename T, enable_if_t<!std::is_convertible<T, std::string>::value &&
                                      !std::is_constructible<std::string, T>::value && is_ostreamable<T>::value,
                                  detail::enabler> = detail::dummy>
std::string to_string(T&& value) {
    std::stringstream stream;
    stream << value;
    return stream.str();
}

template <typename T,
          enable_if_t<!std::is_convertible<T, std::string>::value && !std::is_constructible<std::string, T>::value &&
                          !is_ostreamable<T>::value && is_tuple_like<T>::value && type_count_base<T>::value == 1,
                      detail::enabler> = detail::dummy>
inline std::string to_string(T&& value);

template <typename T,
          enable_if_t<!std::is_convertible<T, std::string>::value && !std::is_constructible<std::string, T>::value &&
                          !is_ostreamable<T>::value && is_tuple_like<T>::value && type_count_base<T>::value >= 2,
                      detail::enabler> = detail::dummy>
inline std::string to_string(T&& value);

template <typename T, enable_if_t<!std::is_convertible<T, std::string>::value &&
                                      !std::is_constructible<std::string, T>::value && !is_ostreamable<T>::value &&
                                      !is_readable_container<typename std::remove_const<T>::type>::value &&
                                      !is_tuple_like<T>::value,
                                  detail::enabler> = detail::dummy>
inline std::string to_string(T&&) {
    return {};
}

template <typename T,
          enable_if_t<!std::is_convertible<T, std::string>::value && !std::is_constructible<std::string, T>::value &&
                          !is_ostreamable<T>::value && is_readable_container<T>::value && !is_tuple_like<T>::value,
                      detail::enabler> = detail::dummy>
inline std::string to_string(T&& variable) {
    auto cval = variable.begin();
    auto end = variable.end();
    if (cval == end) {
        return {"{}"};
    }
    std::vector<std::string> defaults;
    while (cval != end) {
        defaults.emplace_back(CLI::detail::to_string(*cval));
        ++cval;
    }
    return {"[" + detail::join(defaults) + "]"};
}

template <typename T, std::size_t I>
inline typename std::enable_if<I == type_count_base<T>::value, std::string>::type tuple_value_string(T&&);

template <typename T, std::size_t I>
inline typename std::enable_if<(I < type_count_base<T>::value), std::string>::type tuple_value_string(T&& value);

template <typename T,
          enable_if_t<!std::is_convertible<T, std::string>::value && !std::is_constructible<std::string, T>::value &&
                          !is_ostreamable<T>::value && is_tuple_like<T>::value && type_count_base<T>::value == 1,
                      detail::enabler>>
inline std::string to_string(T&& value) {
    return to_string(std::get<0>(value));
}

template <typename T,
          enable_if_t<!std::is_convertible<T, std::string>::value && !std::is_constructible<std::string, T>::value &&
                          !is_ostreamable<T>::value && is_tuple_like<T>::value && type_count_base<T>::value >= 2,
                      detail::enabler>>
inline std::string to_string(T&& value) {
    auto tname = std::string(1, '[') + tuple_value_string<T, 0>(value);
    tname.push_back(']');
    return tname;
}

template <typename T, std::size_t I>
inline typename std::enable_if<I == type_count_base<T>::value, std::string>::type tuple_value_string(T&&) {
    return std::string{};
}

template <typename T, std::size_t I>
inline typename std::enable_if<(I < type_count_base<T>::value), std::string>::type tuple_value_string(T&& value) {
    auto str = std::string{to_string(std::get<I>(value))} + ',' + tuple_value_string<T, I + 1>(value);
    if (str.back() == ',')
        str.pop_back();
    return str;
}

template <typename T1, typename T2, typename T,
          enable_if_t<std::is_same<T1, T2>::value, detail::enabler> = detail::dummy>
auto checked_to_string(T&& value) -> decltype(to_string(std::forward<T>(value))) {
    return to_string(std::forward<T>(value));
}

template <typename T1, typename T2, typename T,
          enable_if_t<!std::is_same<T1, T2>::value, detail::enabler> = detail::dummy>
std::string checked_to_string(T&&) {
    return std::string{};
}
template <typename T, enable_if_t<std::is_arithmetic<T>::value, detail::enabler> = detail::dummy>
std::string value_string(const T& value) {
    return std::to_string(value);
}
template <typename T, enable_if_t<std::is_enum<T>::value, detail::enabler> = detail::dummy>
std::string value_string(const T& value) {
    return std::to_string(static_cast<typename std::underlying_type<T>::type>(value));
}
template <typename T,
          enable_if_t<!std::is_enum<T>::value && !std::is_arithmetic<T>::value, detail::enabler> = detail::dummy>
auto value_string(const T& value) -> decltype(to_string(value)) {
    return to_string(value);
}

template <typename T, typename def, typename Enable = void>
struct wrapped_type {
    using type = def;
};

template <typename T, typename def>
struct wrapped_type<T, def, typename std::enable_if<is_wrapper<T>::value>::type> {
    using type = typename T::value_type;
};

template <typename T>
struct subtype_count;

template <typename T>
struct subtype_count_min;

template <typename T, typename Enable = void>
struct type_count {
    static const int value{0};
};

template <typename T>
struct type_count<T, typename std::enable_if<!is_wrapper<T>::value && !is_tuple_like<T>::value &&
                                             !is_complex<T>::value && !std::is_void<T>::value>::type> {
    static constexpr int value{1};
};

template <typename T>
struct type_count<T, typename std::enable_if<is_complex<T>::value>::type> {
    static constexpr int value{2};
};

template <typename T>
struct type_count<T, typename std::enable_if<is_mutable_container<T>::value>::type> {
    static constexpr int value{subtype_count<typename T::value_type>::value};
};

template <typename T>
struct type_count<T, typename std::enable_if<is_wrapper<T>::value && !is_complex<T>::value &&
                                             !is_tuple_like<T>::value && !is_mutable_container<T>::value>::type> {
    static constexpr int value{type_count<typename T::value_type>::value};
};

template <typename T, std::size_t I>
constexpr typename std::enable_if<I == type_count_base<T>::value, int>::type tuple_type_size() {
    return 0;
}

template <typename T, std::size_t I>
    constexpr typename std::enable_if < I<type_count_base<T>::value, int>::type tuple_type_size() {
    return subtype_count<typename std::tuple_element<I, T>::type>::value + tuple_type_size<T, I + 1>();
}

template <typename T>
struct type_count<T, typename std::enable_if<is_tuple_like<T>::value && !is_complex<T>::value>::type> {
    static constexpr int value{tuple_type_size<T, 0>()};
};

template <typename T>
struct subtype_count {
    static constexpr int value{is_mutable_container<T>::value ? expected_max_vector_size : type_count<T>::value};
};

template <typename T, typename Enable = void>
struct type_count_min {
    static const int value{0};
};

template <typename T>
struct type_count_min<
    T, typename std::enable_if<!is_mutable_container<T>::value && !is_tuple_like<T>::value && !is_wrapper<T>::value &&
                               !is_complex<T>::value && !std::is_void<T>::value>::type> {
    static constexpr int value{type_count<T>::value};
};

template <typename T>
struct type_count_min<T, typename std::enable_if<is_complex<T>::value>::type> {
    static constexpr int value{1};
};

template <typename T>
struct type_count_min<
    T, typename std::enable_if<is_wrapper<T>::value && !is_complex<T>::value && !is_tuple_like<T>::value>::type> {
    static constexpr int value{subtype_count_min<typename T::value_type>::value};
};

template <typename T, std::size_t I>
constexpr typename std::enable_if<I == type_count_base<T>::value, int>::type tuple_type_size_min() {
    return 0;
}

template <typename T, std::size_t I>
    constexpr typename std::enable_if < I<type_count_base<T>::value, int>::type tuple_type_size_min() {
    return subtype_count_min<typename std::tuple_element<I, T>::type>::value + tuple_type_size_min<T, I + 1>();
}

template <typename T>
struct type_count_min<T, typename std::enable_if<is_tuple_like<T>::value && !is_complex<T>::value>::type> {
    static constexpr int value{tuple_type_size_min<T, 0>()};
};

template <typename T>
struct subtype_count_min {
    static constexpr int value{is_mutable_container<T>::value
                                   ? ((type_count<T>::value < expected_max_vector_size) ? type_count<T>::value : 0)
                                   : type_count_min<T>::value};
};

template <typename T, typename Enable = void>
struct expected_count {
    static const int value{0};
};

template <typename T>
struct expected_count<T, typename std::enable_if<!is_mutable_container<T>::value && !is_wrapper<T>::value &&
                                                 !std::is_void<T>::value>::type> {
    static constexpr int value{1};
};
template <typename T>
struct expected_count<T, typename std::enable_if<is_mutable_container<T>::value>::type> {
    static constexpr int value{expected_max_vector_size};
};

template <typename T>
struct expected_count<T, typename std::enable_if<!is_mutable_container<T>::value && is_wrapper<T>::value>::type> {
    static constexpr int value{expected_count<typename T::value_type>::value};
};

enum class object_category : std::uint8_t {
    char_value = 1,
    integral_value = 2,
    unsigned_integral = 4,
    enumeration = 6,
    boolean_value = 8,
    floating_point = 10,
    number_constructible = 12,
    double_constructible = 14,
    integer_constructible = 16,
    string_assignable = 23,
    string_constructible = 24,
    wstring_assignable = 25,
    wstring_constructible = 26,
    other = 45,
    wrapper_value = 50,
    complex_number = 60,
    tuple_value = 70,
    container_value = 80,
};

template <typename T, typename Enable = void>
struct classify_object {
    static constexpr object_category value{object_category::other};
};

template <typename T>
struct classify_object<
    T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, char>::value &&
                               std::is_signed<T>::value && !is_bool<T>::value && !std::is_enum<T>::value>::type> {
    static constexpr object_category value{object_category::integral_value};
};

template <typename T>
struct classify_object<T, typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value &&
                                                  !std::is_same<T, char>::value && !is_bool<T>::value>::type> {
    static constexpr object_category value{object_category::unsigned_integral};
};

template <typename T>
struct classify_object<T, typename std::enable_if<std::is_same<T, char>::value && !std::is_enum<T>::value>::type> {
    static constexpr object_category value{object_category::char_value};
};

template <typename T>
struct classify_object<T, typename std::enable_if<is_bool<T>::value>::type> {
    static constexpr object_category value{object_category::boolean_value};
};

template <typename T>
struct classify_object<T, typename std::enable_if<std::is_floating_point<T>::value>::type> {
    static constexpr object_category value{object_category::floating_point};
};
#if defined _MSC_VER
#define WIDE_STRING_CHECK !std::is_assignable<T&, std::wstring>::value && !std::is_constructible<T, std::wstring>::value
#define STRING_CHECK true
#else
#define WIDE_STRING_CHECK true
#define STRING_CHECK !std::is_assignable<T&, std::string>::value && !std::is_constructible<T, std::string>::value
#endif

template <typename T>
struct classify_object<T,
                       typename std::enable_if<!std::is_floating_point<T>::value && !std::is_integral<T>::value &&
                                               WIDE_STRING_CHECK && std::is_assignable<T&, std::string>::value>::type> {
    static constexpr object_category value{object_category::string_assignable};
};

template <typename T>
struct classify_object<
    T, typename std::enable_if<!std::is_floating_point<T>::value && !std::is_integral<T>::value &&
                               !std::is_assignable<T&, std::string>::value && (type_count<T>::value == 1) &&
                               WIDE_STRING_CHECK && std::is_constructible<T, std::string>::value>::type> {
    static constexpr object_category value{object_category::string_constructible};
};

template <typename T>
struct classify_object<T, typename std::enable_if<!std::is_floating_point<T>::value && !std::is_integral<T>::value &&
                                                  STRING_CHECK && std::is_assignable<T&, std::wstring>::value>::type> {
    static constexpr object_category value{object_category::wstring_assignable};
};

template <typename T>
struct classify_object<
    T, typename std::enable_if<!std::is_floating_point<T>::value && !std::is_integral<T>::value &&
                               !std::is_assignable<T&, std::wstring>::value && (type_count<T>::value == 1) &&
                               STRING_CHECK && std::is_constructible<T, std::wstring>::value>::type> {
    static constexpr object_category value{object_category::wstring_constructible};
};

template <typename T>
struct classify_object<T, typename std::enable_if<std::is_enum<T>::value>::type> {
    static constexpr object_category value{object_category::enumeration};
};

template <typename T>
struct classify_object<T, typename std::enable_if<is_complex<T>::value>::type> {
    static constexpr object_category value{object_category::complex_number};
};

template <typename T>
struct uncommon_type {
    using type = typename std::conditional<
        !std::is_floating_point<T>::value && !std::is_integral<T>::value &&
            !std::is_assignable<T&, std::string>::value && !std::is_constructible<T, std::string>::value &&
            !std::is_assignable<T&, std::wstring>::value && !std::is_constructible<T, std::wstring>::value &&
            !is_complex<T>::value && !is_mutable_container<T>::value && !std::is_enum<T>::value,
        std::true_type, std::false_type>::type;
    static constexpr bool value = type::value;
};

template <typename T>
struct classify_object<T, typename std::enable_if<(!is_mutable_container<T>::value && is_wrapper<T>::value &&
                                                   !is_tuple_like<T>::value && uncommon_type<T>::value)>::type> {
    static constexpr object_category value{object_category::wrapper_value};
};

template <typename T>
struct classify_object<T, typename std::enable_if<uncommon_type<T>::value && type_count<T>::value == 1 &&
                                                  !is_wrapper<T>::value && is_direct_constructible<T, double>::value &&
                                                  is_direct_constructible<T, int>::value>::type> {
    static constexpr object_category value{object_category::number_constructible};
};

template <typename T>
struct classify_object<T, typename std::enable_if<uncommon_type<T>::value && type_count<T>::value == 1 &&
                                                  !is_wrapper<T>::value && !is_direct_constructible<T, double>::value &&
                                                  is_direct_constructible<T, int>::value>::type> {
    static constexpr object_category value{object_category::integer_constructible};
};

template <typename T>
struct classify_object<T, typename std::enable_if<uncommon_type<T>::value && type_count<T>::value == 1 &&
                                                  !is_wrapper<T>::value && is_direct_constructible<T, double>::value &&
                                                  !is_direct_constructible<T, int>::value>::type> {
    static constexpr object_category value{object_category::double_constructible};
};

template <typename T>
struct classify_object<
    T, typename std::enable_if<is_tuple_like<T>::value &&
                               ((type_count<T>::value >= 2 && !is_wrapper<T>::value) ||
                                (uncommon_type<T>::value && !is_direct_constructible<T, double>::value &&
                                 !is_direct_constructible<T, int>::value) ||
                                (uncommon_type<T>::value && type_count<T>::value >= 2))>::type> {
    static constexpr object_category value{object_category::tuple_value};
};

template <typename T>
struct classify_object<T, typename std::enable_if<is_mutable_container<T>::value>::type> {
    static constexpr object_category value{object_category::container_value};
};

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::char_value, detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "CHAR";
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::integral_value ||
                                      classify_object<T>::value == object_category::integer_constructible,
                                  detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "INT";
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::unsigned_integral, detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "UINT";
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::floating_point ||
                                      classify_object<T>::value == object_category::number_constructible ||
                                      classify_object<T>::value == object_category::double_constructible,
                                  detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "FLOAT";
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::enumeration, detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "ENUM";
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::boolean_value, detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "BOOLEAN";
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::complex_number, detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "COMPLEX";
}

template <typename T, enable_if_t<classify_object<T>::value >= object_category::string_assignable &&
                                      classify_object<T>::value <= object_category::other,
                                  detail::enabler> = detail::dummy>
constexpr const char* type_name() {
    return "TEXT";
}
template <typename T,
          enable_if_t<classify_object<T>::value == object_category::tuple_value && type_count_base<T>::value >= 2,
                      detail::enabler> = detail::dummy>
std::string type_name();

template <typename T, enable_if_t<classify_object<T>::value == object_category::container_value ||
                                      classify_object<T>::value == object_category::wrapper_value,
                                  detail::enabler> = detail::dummy>
std::string type_name();

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::tuple_value && type_count_base<T>::value == 1,
                      detail::enabler> = detail::dummy>
inline std::string type_name() {
    return type_name<typename std::decay<typename std::tuple_element<0, T>::type>::type>();
}

template <typename T, std::size_t I>
inline typename std::enable_if<I == type_count_base<T>::value, std::string>::type tuple_name() {
    return std::string{};
}

template <typename T, std::size_t I>
inline typename std::enable_if<(I < type_count_base<T>::value), std::string>::type tuple_name() {
    auto str = std::string{type_name<typename std::decay<typename std::tuple_element<I, T>::type>::type>()} + ',' +
               tuple_name<T, I + 1>();
    if (str.back() == ',')
        str.pop_back();
    return str;
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::tuple_value && type_count_base<T>::value >= 2,
                      detail::enabler>>
inline std::string type_name() {
    auto tname = std::string(1, '[') + tuple_name<T, 0>();
    tname.push_back(']');
    return tname;
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::container_value ||
                                      classify_object<T>::value == object_category::wrapper_value,
                                  detail::enabler>>
inline std::string type_name() {
    return type_name<typename T::value_type>();
}

template <typename T, enable_if_t<std::is_unsigned<T>::value, detail::enabler> = detail::dummy>
bool integral_conversion(const std::string& input, T& output) noexcept {
    if (input.empty() || input.front() == '-') {
        return false;
    }
    char* val{nullptr};
    errno = 0;
    std::uint64_t output_ll = std::strtoull(input.c_str(), &val, 0);
    if (errno == ERANGE) {
        return false;
    }
    output = static_cast<T>(output_ll);
    if (val == (input.c_str() + input.size()) && static_cast<std::uint64_t>(output) == output_ll) {
        return true;
    }
    val = nullptr;
    std::int64_t output_sll = std::strtoll(input.c_str(), &val, 0);
    if (val == (input.c_str() + input.size())) {
        output = (output_sll < 0) ? static_cast<T>(0) : static_cast<T>(output_sll);
        return (static_cast<std::int64_t>(output) == output_sll);
    }
    auto group_separators = get_group_separators();
    if (input.find_first_of(group_separators) != std::string::npos) {
        std::string nstring = input;
        for (auto& separator : group_separators) {
            if (input.find_first_of(separator) != std::string::npos) {
                nstring.erase(std::remove(nstring.begin(), nstring.end(), separator), nstring.end());
            }
        }
        return integral_conversion(nstring, output);
    }

    if (std::isspace(static_cast<unsigned char>(input.back()))) {
        return integral_conversion(trim_copy(input), output);
    }
    if (input.compare(0, 2, "0o") == 0 || input.compare(0, 2, "0O") == 0) {
        val = nullptr;
        errno = 0;
        output_ll = std::strtoull(input.c_str() + 2, &val, 8);
        if (errno == ERANGE) {
            return false;
        }
        output = static_cast<T>(output_ll);
        return (val == (input.c_str() + input.size()) && static_cast<std::uint64_t>(output) == output_ll);
    }
    if (input.compare(0, 2, "0b") == 0 || input.compare(0, 2, "0B") == 0) {
        val = nullptr;
        errno = 0;
        output_ll = std::strtoull(input.c_str() + 2, &val, 2);
        if (errno == ERANGE) {
            return false;
        }
        output = static_cast<T>(output_ll);
        return (val == (input.c_str() + input.size()) && static_cast<std::uint64_t>(output) == output_ll);
    }
    return false;
}

template <typename T, enable_if_t<std::is_signed<T>::value, detail::enabler> = detail::dummy>
bool integral_conversion(const std::string& input, T& output) noexcept {
    if (input.empty()) {
        return false;
    }
    char* val = nullptr;
    errno = 0;
    std::int64_t output_ll = std::strtoll(input.c_str(), &val, 0);
    if (errno == ERANGE) {
        return false;
    }
    output = static_cast<T>(output_ll);
    if (val == (input.c_str() + input.size()) && static_cast<std::int64_t>(output) == output_ll) {
        return true;
    }
    if (input == "true") {
        output = static_cast<T>(1);
        return true;
    }
    auto group_separators = get_group_separators();
    if (input.find_first_of(group_separators) != std::string::npos) {
        for (auto& separator : group_separators) {
            if (input.find_first_of(separator) != std::string::npos) {
                std::string nstring = input;
                nstring.erase(std::remove(nstring.begin(), nstring.end(), separator), nstring.end());
                return integral_conversion(nstring, output);
            }
        }
    }
    if (std::isspace(static_cast<unsigned char>(input.back()))) {
        return integral_conversion(trim_copy(input), output);
    }
    if (input.compare(0, 2, "0o") == 0 || input.compare(0, 2, "0O") == 0) {
        val = nullptr;
        errno = 0;
        output_ll = std::strtoll(input.c_str() + 2, &val, 8);
        if (errno == ERANGE) {
            return false;
        }
        output = static_cast<T>(output_ll);
        return (val == (input.c_str() + input.size()) && static_cast<std::int64_t>(output) == output_ll);
    }
    if (input.compare(0, 2, "0b") == 0 || input.compare(0, 2, "0B") == 0) {
        val = nullptr;
        errno = 0;
        output_ll = std::strtoll(input.c_str() + 2, &val, 2);
        if (errno == ERANGE) {
            return false;
        }
        output = static_cast<T>(output_ll);
        return (val == (input.c_str() + input.size()) && static_cast<std::int64_t>(output) == output_ll);
    }
    return false;
}

inline std::int64_t to_flag_value(std::string val) noexcept {
    static const std::string trueString("true");
    static const std::string falseString("false");
    if (val == trueString) {
        return 1;
    }
    if (val == falseString) {
        return -1;
    }
    val = detail::to_lower(val);
    std::int64_t ret = 0;
    if (val.size() == 1) {
        if (val[0] >= '1' && val[0] <= '9') {
            return (static_cast<std::int64_t>(val[0]) - '0');
        }
        switch (val[0]) {
            case '0':
            case 'f':
            case 'n':
            case '-':
                ret = -1;
                break;
            case 't':
            case 'y':
            case '+':
                ret = 1;
                break;
            default:
                errno = EINVAL;
                return -1;
        }
        return ret;
    }
    if (val == trueString || val == "on" || val == "yes" || val == "enable") {
        ret = 1;
    } else if (val == falseString || val == "off" || val == "no" || val == "disable") {
        ret = -1;
    } else {
        char* loc_ptr{nullptr};
        ret = std::strtoll(val.c_str(), &loc_ptr, 0);
        if (loc_ptr != (val.c_str() + val.size()) && errno == 0) {
            errno = EINVAL;
        }
    }
    return ret;
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::integral_value ||
                                      classify_object<T>::value == object_category::unsigned_integral,
                                  detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    return integral_conversion(input, output);
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::char_value, detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    if (input.size() == 1) {
        output = static_cast<T>(input[0]);
        return true;
    }
    std::int8_t res{0};
    bool result = integral_conversion(input, res);
    if (result) {
        output = static_cast<T>(res);
    }
    return result;
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::boolean_value, detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    errno = 0;
    auto out = to_flag_value(input);
    if (errno == 0) {
        output = (out > 0);
    } else if (errno == ERANGE) {
        output = (input[0] != '-');
    } else {
        return false;
    }
    return true;
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::floating_point, detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    if (input.empty()) {
        return false;
    }
    char* val = nullptr;
    auto output_ld = std::strtold(input.c_str(), &val);
    output = static_cast<T>(output_ld);
    if (val == (input.c_str() + input.size())) {
        return true;
    }
    while (std::isspace(static_cast<unsigned char>(*val))) {
        ++val;
        if (val == (input.c_str() + input.size())) {
            return true;
        }
    }

    auto group_separators = get_group_separators();
    if (input.find_first_of(group_separators) != std::string::npos) {
        for (auto& separator : group_separators) {
            if (input.find_first_of(separator) != std::string::npos) {
                std::string nstring = input;
                nstring.erase(std::remove(nstring.begin(), nstring.end(), separator), nstring.end());
                return lexical_cast(nstring, output);
            }
        }
    }
    return false;
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::complex_number, detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    using XC = typename wrapped_type<T, double>::type;
    XC x{0.0}, y{0.0};
    auto str1 = input;
    bool worked = false;
    auto nloc = str1.find_last_of("+-");
    if (nloc != std::string::npos && nloc > 0) {
        worked = lexical_cast(str1.substr(0, nloc), x);
        str1 = str1.substr(nloc);
        if (str1.back() == 'i' || str1.back() == 'j')
            str1.pop_back();
        worked = worked && lexical_cast(str1, y);
    } else {
        if (str1.back() == 'i' || str1.back() == 'j') {
            str1.pop_back();
            worked = lexical_cast(str1, y);
            x = XC{0};
        } else {
            worked = lexical_cast(str1, x);
            y = XC{0};
        }
    }
    if (worked) {
        output = T{x, y};
        return worked;
    }
    return from_stream(input, output);
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::string_assignable, detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    output = input;
    return true;
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::string_constructible, detail::enabler> =
                          detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    output = T(input);
    return true;
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::wstring_assignable, detail::enabler> =
                          detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    output = widen(input);
    return true;
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::wstring_constructible,
                                  detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    output = T{widen(input)};
    return true;
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::enumeration, detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    typename std::underlying_type<T>::type val;
    if (!integral_conversion(input, val)) {
        return false;
    }
    output = static_cast<T>(val);
    return true;
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::wrapper_value &&
                                      std::is_assignable<T&, typename T::value_type>::value,
                                  detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    typename T::value_type val;
    if (lexical_cast(input, val)) {
        output = val;
        return true;
    }
    return from_stream(input, output);
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::wrapper_value &&
                          !std::is_assignable<T&, typename T::value_type>::value && std::is_assignable<T&, T>::value,
                      detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    typename T::value_type val;
    if (lexical_cast(input, val)) {
        output = T{val};
        return true;
    }
    return from_stream(input, output);
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::number_constructible, detail::enabler> =
                          detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    int val = 0;
    if (integral_conversion(input, val)) {
        output = T(val);
        return true;
    }

    double dval = 0.0;
    if (lexical_cast(input, dval)) {
        output = T{dval};
        return true;
    }

    return from_stream(input, output);
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::integer_constructible,
                                  detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    int val = 0;
    if (integral_conversion(input, val)) {
        output = T(val);
        return true;
    }
    return from_stream(input, output);
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::double_constructible, detail::enabler> =
                          detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    double val = 0.0;
    if (lexical_cast(input, val)) {
        output = T{val};
        return true;
    }
    return from_stream(input, output);
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::other && std::is_assignable<T&, int>::value,
                      detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    int val = 0;
    if (integral_conversion(input, val)) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4800)
#endif
        output = val;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        return true;
    }
    return from_stream(input, output);
}

template <typename T, enable_if_t<classify_object<T>::value == object_category::other &&
                                      !std::is_assignable<T&, int>::value && is_istreamable<T>::value,
                                  detail::enabler> = detail::dummy>
bool lexical_cast(const std::string& input, T& output) {
    return from_stream(input, output);
}

template <typename T,
          enable_if_t<classify_object<T>::value == object_category::other && !std::is_assignable<T&, int>::value &&
                          !is_istreamable<T>::value && !adl_detail::is_lexical_castable<T>::value,
                      detail::enabler> = detail::dummy>
bool lexical_cast(const std::string&, T&) {
    static_assert(!std::is_same<T, T>::value,
                  "option object type must have a lexical cast overload or streaming input operator(>>) defined, if it "
                  "is convertible from another type use the add_option<T, XC>(...) with XC being the known type");
    return false;
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<std::is_same<AssignTo, ConvertTo>::value &&
                          (classify_object<AssignTo>::value == object_category::string_assignable ||
                           classify_object<AssignTo>::value == object_category::string_constructible ||
                           classify_object<AssignTo>::value == object_category::wstring_assignable ||
                           classify_object<AssignTo>::value == object_category::wstring_constructible),
                      detail::enabler> = detail::dummy>
bool lexical_assign(const std::string& input, AssignTo& output) {
    return lexical_cast(input, output);
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<std::is_same<AssignTo, ConvertTo>::value && std::is_assignable<AssignTo&, AssignTo>::value &&
                          classify_object<AssignTo>::value != object_category::string_assignable &&
                          classify_object<AssignTo>::value != object_category::string_constructible &&
                          classify_object<AssignTo>::value != object_category::wstring_assignable &&
                          classify_object<AssignTo>::value != object_category::wstring_constructible,
                      detail::enabler> = detail::dummy>
bool lexical_assign(const std::string& input, AssignTo& output) {
    if (input.empty()) {
        output = AssignTo{};
        return true;
    }

    return lexical_cast(input, output);
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<std::is_same<AssignTo, ConvertTo>::value && !std::is_assignable<AssignTo&, AssignTo>::value &&
                          classify_object<AssignTo>::value == object_category::wrapper_value,
                      detail::enabler> = detail::dummy>
bool lexical_assign(const std::string& input, AssignTo& output) {
    if (input.empty()) {
        typename AssignTo::value_type emptyVal{};
        output = emptyVal;
        return true;
    }
    return lexical_cast(input, output);
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<std::is_same<AssignTo, ConvertTo>::value && !std::is_assignable<AssignTo&, AssignTo>::value &&
                          classify_object<AssignTo>::value != object_category::wrapper_value &&
                          std::is_assignable<AssignTo&, int>::value,
                      detail::enabler> = detail::dummy>
bool lexical_assign(const std::string& input, AssignTo& output) {
    if (input.empty()) {
        output = 0;
        return true;
    }
    int val{0};
    if (lexical_cast(input, val)) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
        output = val;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        return true;
    }
    return false;
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<!std::is_same<AssignTo, ConvertTo>::value && std::is_assignable<AssignTo&, ConvertTo&>::value,
                      detail::enabler> = detail::dummy>
bool lexical_assign(const std::string& input, AssignTo& output) {
    ConvertTo val{};
    bool parse_result = (!input.empty()) ? lexical_cast(input, val) : true;
    if (parse_result) {
        output = val;
    }
    return parse_result;
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<!std::is_same<AssignTo, ConvertTo>::value && !std::is_assignable<AssignTo&, ConvertTo&>::value &&
                          std::is_move_assignable<AssignTo>::value,
                      detail::enabler> = detail::dummy>
bool lexical_assign(const std::string& input, AssignTo& output) {
    ConvertTo val{};
    bool parse_result = input.empty() ? true : lexical_cast(input, val);
    if (parse_result) {
        output = AssignTo(val);
    }
    return parse_result;
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<classify_object<ConvertTo>::value <= object_category::other &&
                          classify_object<AssignTo>::value <= object_category::wrapper_value,
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std ::string>& strings, AssignTo& output) {
    return lexical_assign<AssignTo, ConvertTo>(strings[0], output);
}

template <typename AssignTo, typename ConvertTo,
          enable_if_t<(type_count<AssignTo>::value <= 2) && expected_count<AssignTo>::value == 1 &&
                          is_tuple_like<ConvertTo>::value && type_count_base<ConvertTo>::value == 2,
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std ::string>& strings, AssignTo& output) {
    using FirstType = typename std::remove_const<typename std::tuple_element<0, ConvertTo>::type>::type;
    using SecondType = typename std::tuple_element<1, ConvertTo>::type;
    FirstType v1;
    SecondType v2{};
    bool retval = lexical_assign<FirstType, FirstType>(strings[0], v1);
    retval = retval && lexical_assign<SecondType, SecondType>((strings.size() > 1) ? strings[1] : std::string{}, v2);
    if (retval) {
        output = AssignTo{v1, v2};
    }
    return retval;
}

template <class AssignTo, class ConvertTo,
          enable_if_t<is_mutable_container<AssignTo>::value && is_mutable_container<ConvertTo>::value &&
                          type_count<ConvertTo>::value == 1,
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std ::string>& strings, AssignTo& output) {
    output.erase(output.begin(), output.end());
    if (strings.empty()) {
        return true;
    }
    if (strings.size() == 1 && strings[0] == "{}") {
        return true;
    }
    bool skip_remaining = false;
    if (strings.size() == 2 && strings[0] == "{}" && is_separator(strings[1])) {
        skip_remaining = true;
    }
    for (const auto& elem : strings) {
        typename AssignTo::value_type out;
        bool retval = lexical_assign<typename AssignTo::value_type, typename ConvertTo::value_type>(elem, out);
        if (!retval) {
            return false;
        }
        output.insert(output.end(), std::move(out));
        if (skip_remaining) {
            break;
        }
    }
    return (!output.empty());
}

template <class AssignTo, class ConvertTo, enable_if_t<is_complex<ConvertTo>::value, detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std::string>& strings, AssignTo& output) {
    if (strings.size() >= 2 && !strings[1].empty()) {
        using XC2 = typename wrapped_type<ConvertTo, double>::type;
        XC2 x{0.0}, y{0.0};
        auto str1 = strings[1];
        if (str1.back() == 'i' || str1.back() == 'j') {
            str1.pop_back();
        }
        auto worked = lexical_cast(strings[0], x) && lexical_cast(str1, y);
        if (worked) {
            output = ConvertTo{x, y};
        }
        return worked;
    }
    return lexical_assign<AssignTo, ConvertTo>(strings[0], output);
}

template <class AssignTo, class ConvertTo,
          enable_if_t<is_mutable_container<AssignTo>::value && (expected_count<ConvertTo>::value == 1) &&
                          (type_count<ConvertTo>::value == 1),
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std ::string>& strings, AssignTo& output) {
    bool retval = true;
    output.clear();
    output.reserve(strings.size());
    for (const auto& elem : strings) {
        output.emplace_back();
        retval = retval && lexical_assign<typename AssignTo::value_type, ConvertTo>(elem, output.back());
    }
    return (!output.empty()) && retval;
}

template <class AssignTo, class ConvertTo,
          enable_if_t<is_mutable_container<AssignTo>::value && is_mutable_container<ConvertTo>::value &&
                          type_count_base<ConvertTo>::value == 2,
                      detail::enabler> = detail::dummy>
bool lexical_conversion(std::vector<std::string> strings, AssignTo& output);

template <class AssignTo, class ConvertTo,
          enable_if_t<is_mutable_container<AssignTo>::value && is_mutable_container<ConvertTo>::value &&
                          type_count_base<ConvertTo>::value != 2 &&
                          ((type_count<ConvertTo>::value > 2) ||
                           (type_count<ConvertTo>::value > type_count_base<ConvertTo>::value)),
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std::string>& strings, AssignTo& output);

template <class AssignTo, class ConvertTo,
          enable_if_t<is_tuple_like<AssignTo>::value && is_tuple_like<ConvertTo>::value &&
                          (type_count_base<ConvertTo>::value != type_count<ConvertTo>::value ||
                           type_count<ConvertTo>::value > 2),
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std::string>& strings, AssignTo& output);

template <typename AssignTo, typename ConvertTo,
          enable_if_t<!is_tuple_like<AssignTo>::value && !is_mutable_container<AssignTo>::value &&
                          classify_object<ConvertTo>::value != object_category::wrapper_value &&
                          (is_mutable_container<ConvertTo>::value || type_count<ConvertTo>::value > 2),
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std ::string>& strings, AssignTo& output) {
    if (strings.size() > 1 || (!strings.empty() && !(strings.front().empty()))) {
        ConvertTo val;
        auto retval = lexical_conversion<ConvertTo, ConvertTo>(strings, val);
        output = AssignTo{val};
        return retval;
    }
    output = AssignTo{};
    return true;
}

template <class AssignTo, class ConvertTo, std::size_t I>
inline typename std::enable_if<(I >= type_count_base<AssignTo>::value), bool>::type tuple_conversion(
    const std::vector<std::string>&, AssignTo&) {
    return true;
}

template <class AssignTo, class ConvertTo>
inline typename std::enable_if<!is_mutable_container<ConvertTo>::value && type_count<ConvertTo>::value == 1, bool>::type
tuple_type_conversion(std::vector<std::string>& strings, AssignTo& output) {
    auto retval = lexical_assign<AssignTo, ConvertTo>(strings[0], output);
    strings.erase(strings.begin());
    return retval;
}

template <class AssignTo, class ConvertTo>
inline typename std::enable_if<!is_mutable_container<ConvertTo>::value && (type_count<ConvertTo>::value > 1) &&
                                   type_count<ConvertTo>::value == type_count_min<ConvertTo>::value,
                               bool>::type
tuple_type_conversion(std::vector<std::string>& strings, AssignTo& output) {
    auto retval = lexical_conversion<AssignTo, ConvertTo>(strings, output);
    strings.erase(strings.begin(), strings.begin() + type_count<ConvertTo>::value);
    return retval;
}

template <class AssignTo, class ConvertTo>
inline typename std::enable_if<is_mutable_container<ConvertTo>::value ||
                                   type_count<ConvertTo>::value != type_count_min<ConvertTo>::value,
                               bool>::type
tuple_type_conversion(std::vector<std::string>& strings, AssignTo& output) {
    std::size_t index{subtype_count_min<ConvertTo>::value};
    const std::size_t mx_count{subtype_count<ConvertTo>::value};
    const std::size_t mx{(std::min)(mx_count, strings.size() - 1)};

    while (index < mx) {
        if (is_separator(strings[index])) {
            break;
        }
        ++index;
    }
    bool retval = lexical_conversion<AssignTo, ConvertTo>(
        std::vector<std::string>(strings.begin(), strings.begin() + static_cast<std::ptrdiff_t>(index)), output);
    if (strings.size() > index) {
        strings.erase(strings.begin(), strings.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    } else {
        strings.clear();
    }
    return retval;
}

template <class AssignTo, class ConvertTo, std::size_t I>
inline typename std::enable_if<(I < type_count_base<AssignTo>::value), bool>::type tuple_conversion(
    std::vector<std::string> strings, AssignTo& output) {
    bool retval = true;
    using ConvertToElement =
        typename std::conditional<is_tuple_like<ConvertTo>::value, typename std::tuple_element<I, ConvertTo>::type,
                                  ConvertTo>::type;
    if (!strings.empty()) {
        retval = retval && tuple_type_conversion<typename std::tuple_element<I, AssignTo>::type, ConvertToElement>(
                               strings, std::get<I>(output));
    }
    retval = retval && tuple_conversion<AssignTo, ConvertTo, I + 1>(std::move(strings), output);
    return retval;
}

template <class AssignTo, class ConvertTo,
          enable_if_t<is_mutable_container<AssignTo>::value && is_mutable_container<ConvertTo>::value &&
                          type_count_base<ConvertTo>::value == 2,
                      detail::enabler>>
bool lexical_conversion(std::vector<std::string> strings, AssignTo& output) {
    output.clear();
    while (!strings.empty()) {
        typename std::remove_const<typename std::tuple_element<0, typename ConvertTo::value_type>::type>::type v1;
        typename std::tuple_element<1, typename ConvertTo::value_type>::type v2;
        bool retval = tuple_type_conversion<decltype(v1), decltype(v1)>(strings, v1);
        if (!strings.empty()) {
            retval = retval && tuple_type_conversion<decltype(v2), decltype(v2)>(strings, v2);
        }
        if (retval) {
            output.insert(output.end(), typename AssignTo::value_type{v1, v2});
        } else {
            return false;
        }
    }
    return (!output.empty());
}

template <class AssignTo, class ConvertTo,
          enable_if_t<is_tuple_like<AssignTo>::value && is_tuple_like<ConvertTo>::value &&
                          (type_count_base<ConvertTo>::value != type_count<ConvertTo>::value ||
                           type_count<ConvertTo>::value > 2),
                      detail::enabler>>
bool lexical_conversion(const std::vector<std ::string>& strings, AssignTo& output) {
    static_assert(
        !is_tuple_like<ConvertTo>::value || type_count_base<AssignTo>::value == type_count_base<ConvertTo>::value,
        "if the conversion type is defined as a tuple it must be the same size as the type you are converting to");
    return tuple_conversion<AssignTo, ConvertTo, 0>(strings, output);
}

template <class AssignTo, class ConvertTo,
          enable_if_t<is_mutable_container<AssignTo>::value && is_mutable_container<ConvertTo>::value &&
                          type_count_base<ConvertTo>::value != 2 &&
                          ((type_count<ConvertTo>::value > 2) ||
                           (type_count<ConvertTo>::value > type_count_base<ConvertTo>::value)),
                      detail::enabler>>
bool lexical_conversion(const std::vector<std ::string>& strings, AssignTo& output) {
    bool retval = true;
    output.clear();
    std::vector<std::string> temp;
    std::size_t ii{0};
    std::size_t icount{0};
    std::size_t xcm{type_count<ConvertTo>::value};
    auto ii_max = strings.size();
    while (ii < ii_max) {
        temp.push_back(strings[ii]);
        ++ii;
        ++icount;
        if (icount == xcm || is_separator(temp.back()) || ii == ii_max) {
            if (static_cast<int>(xcm) > type_count_min<ConvertTo>::value && is_separator(temp.back())) {
                temp.pop_back();
            }
            typename AssignTo::value_type temp_out;
            retval = retval &&
                     lexical_conversion<typename AssignTo::value_type, typename ConvertTo::value_type>(temp, temp_out);
            temp.clear();
            if (!retval) {
                return false;
            }
            output.insert(output.end(), std::move(temp_out));
            icount = 0;
        }
    }
    return retval;
}

template <typename AssignTo, class ConvertTo,
          enable_if_t<classify_object<ConvertTo>::value == object_category::wrapper_value &&
                          std::is_assignable<ConvertTo&, ConvertTo>::value,
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std::string>& strings, AssignTo& output) {
    if (strings.empty() || strings.front().empty()) {
        output = ConvertTo{};
        return true;
    }
    typename ConvertTo::value_type val;
    if (lexical_conversion<typename ConvertTo::value_type, typename ConvertTo::value_type>(strings, val)) {
        output = ConvertTo{val};
        return true;
    }
    return false;
}

template <typename AssignTo, class ConvertTo,
          enable_if_t<classify_object<ConvertTo>::value == object_category::wrapper_value &&
                          !std::is_assignable<AssignTo&, ConvertTo>::value,
                      detail::enabler> = detail::dummy>
bool lexical_conversion(const std::vector<std::string>& strings, AssignTo& output) {
    using ConvertType = typename ConvertTo::value_type;
    if (strings.empty() || strings.front().empty()) {
        output = ConvertType{};
        return true;
    }
    ConvertType val;
    if (lexical_conversion<typename ConvertTo::value_type, typename ConvertTo::value_type>(strings, val)) {
        output = val;
        return true;
    }
    return false;
}

inline std::string sum_string_vector(const std::vector<std::string>& values) {
    double val{0.0};
    bool fail{false};
    std::string output;
    for (const auto& arg : values) {
        double tv{0.0};
        auto comp = lexical_cast(arg, tv);
        if (!comp) {
            errno = 0;
            auto fv = detail::to_flag_value(arg);
            fail = (errno != 0);
            if (fail) {
                break;
            }
            tv = static_cast<double>(fv);
        }
        val += tv;
    }
    if (fail) {
        for (const auto& arg : values) {
            output.append(arg);
        }
    } else {
        std::ostringstream out;
        out.precision(16);
        out << val;
        output = out.str();
    }
    return output;
}

}  

namespace detail {

CLI11_INLINE bool split_short(const std::string& current, std::string& name, std::string& rest);

CLI11_INLINE bool split_long(const std::string& current, std::string& name, std::string& value);

CLI11_INLINE bool split_windows_style(const std::string& current, std::string& name, std::string& value);

CLI11_INLINE std::vector<std::string> split_names(std::string current);

CLI11_INLINE std::vector<std::pair<std::string, std::string>> get_default_flag_values(const std::string& str);

CLI11_INLINE std::tuple<std::vector<std::string>, std::vector<std::string>, std::string> get_names(
    const std::vector<std::string>& input, bool allow_non_standard = false);

}  

namespace detail {

CLI11_INLINE bool split_short(const std::string& current, std::string& name, std::string& rest) {
    if (current.size() > 1 && current[0] == '-' && valid_first_char(current[1])) {
        name = current.substr(1, 1);
        rest = current.substr(2);
        return true;
    }
    return false;
}

CLI11_INLINE bool split_long(const std::string& current, std::string& name, std::string& value) {
    if (current.size() > 2 && current.compare(0, 2, "--") == 0 && valid_first_char(current[2])) {
        auto loc = current.find_first_of('=');
        if (loc != std::string::npos) {
            name = current.substr(2, loc - 2);
            value = current.substr(loc + 1);
        } else {
            name = current.substr(2);
            value = "";
        }
        return true;
    }
    return false;
}

CLI11_INLINE bool split_windows_style(const std::string& current, std::string& name, std::string& value) {
    if (current.size() > 1 && current[0] == '/' && valid_first_char(current[1])) {
        auto loc = current.find_first_of(':');
        if (loc != std::string::npos) {
            name = current.substr(1, loc - 1);
            value = current.substr(loc + 1);
        } else {
            name = current.substr(1);
            value = "";
        }
        return true;
    }
    return false;
}

CLI11_INLINE std::vector<std::string> split_names(std::string current) {
    std::vector<std::string> output;
    std::size_t val = 0;
    while ((val = current.find(',')) != std::string::npos) {
        output.push_back(trim_copy(current.substr(0, val)));
        current = current.substr(val + 1);
    }
    output.push_back(trim_copy(current));
    return output;
}

CLI11_INLINE std::vector<std::pair<std::string, std::string>> get_default_flag_values(const std::string& str) {
    std::vector<std::string> flags = split_names(str);
    flags.erase(
        std::remove_if(flags.begin(), flags.end(),
                       [](const std::string& name) {
                           return ((name.empty()) ||
                                   (!(((name.find_first_of('{') != std::string::npos) && (name.back() == '}')) ||
                                      (name[0] == '!'))));
                       }),
        flags.end());
    std::vector<std::pair<std::string, std::string>> output;
    output.reserve(flags.size());
    for (auto& flag : flags) {
        auto def_start = flag.find_first_of('{');
        std::string defval = "false";
        if ((def_start != std::string::npos) && (flag.back() == '}')) {
            defval = flag.substr(def_start + 1);
            defval.pop_back();
            flag.erase(def_start, std::string::npos);  // NOLINT(readability-suspicious-call-argument)
        }
        flag.erase(0, flag.find_first_not_of("-!"));
        output.emplace_back(flag, defval);
    }
    return output;
}

CLI11_INLINE std::tuple<std::vector<std::string>, std::vector<std::string>, std::string> get_names(
    const std::vector<std::string>& input, bool allow_non_standard) {
    std::vector<std::string> short_names;
    std::vector<std::string> long_names;
    std::string pos_name;
    for (std::string name : input) {
        if (name.empty()) {
            continue;
        }
        if (name.length() > 1 && name[0] == '-' && name[1] != '-') {
            if (name.length() == 2 && valid_first_char(name[1])) {
                short_names.emplace_back(1, name[1]);
            } else if (name.length() > 2) {
                if (allow_non_standard) {
                    name = name.substr(1);
                    if (valid_name_string(name)) {
                        short_names.push_back(name);
                    } else {
                        throw BadNameString::BadLongName(name);
                    }
                } else {
                    throw BadNameString::MissingDash(name);
                }
            } else {
                throw BadNameString::OneCharName(name);
            }
        } else if (name.length() > 2 && name.substr(0, 2) == "--") {
            name = name.substr(2);
            if (valid_name_string(name)) {
                long_names.push_back(name);
            } else {
                throw BadNameString::BadLongName(name);
            }
        } else if (name == "-" || name == "--" || name == "++") {
            throw BadNameString::ReservedName(name);
        } else {
            if (!pos_name.empty()) {
                throw BadNameString::MultiPositionalNames(name);
            }
            if (valid_name_string(name)) {
                pos_name = name;
            } else {
                throw BadNameString::BadPositionalName(name);
            }
        }
    }
    return std::make_tuple(short_names, long_names, pos_name);
}

}  

class App;

struct ConfigItem {
    std::vector<std::string> parents{};

    std::string name{};
    std::vector<std::string> inputs{};
    bool multiline{false};
    CLI11_NODISCARD std::string fullname() const {
        std::vector<std::string> tmp = parents;
        tmp.emplace_back(name);
        return detail::join(tmp, ".");
        (void)multiline;
    }
};

class Config {
   protected:
    std::vector<ConfigItem> items{};

   public:
    virtual std::string to_config(const App*, bool, bool, std::string) const = 0;

    virtual std::vector<ConfigItem> from_config(std::istream&) const = 0;

    CLI11_NODISCARD virtual std::string to_flag(const ConfigItem& item) const {
        if (item.inputs.size() == 1) {
            return item.inputs.at(0);
        }
        if (item.inputs.empty()) {
            return "{}";
        }
        throw ConversionError::TooManyInputsFlag(item.fullname());
    }

    CLI11_NODISCARD std::vector<ConfigItem> from_file(const std::string& name) const {
#if defined CLI11_HAS_FILESYSTEM && CLI11_HAS_FILESYSTEM > 0
        std::ifstream input{to_path(name)};
#else
        std::ifstream input{name};
#endif

        if (!input.good())
            throw FileError::Missing(name);

        return from_config(input);
    }

    virtual ~Config() = default;
};

class ConfigBase : public Config {
   protected:
    char commentChar = '#';
    char arrayStart = '[';
    char arrayEnd = ']';
    char arraySeparator = ',';
    char valueDelimiter = '=';
    char stringQuote = '"';
    char literalQuote = '\'';
    uint8_t maximumLayers{255};
    char parentSeparatorChar{'.'};
    bool commentDefaultsBool = false;
    bool allowMultipleDuplicateFields{false};
    int16_t configIndex{-1};
    std::string configSection{};

   public:
    std::string to_config(const App*, bool default_also, bool write_description, std::string prefix) const override;

    std::vector<ConfigItem> from_config(std::istream& input) const override;
    ConfigBase* comment(char cchar) {
        commentChar = cchar;
        return this;
    }
    ConfigBase* arrayBounds(char aStart, char aEnd) {
        arrayStart = aStart;
        arrayEnd = aEnd;
        return this;
    }
    ConfigBase* arrayDelimiter(char aSep) {
        arraySeparator = aSep;
        return this;
    }
    ConfigBase* valueSeparator(char vSep) {
        valueDelimiter = vSep;
        return this;
    }
    ConfigBase* quoteCharacter(char qString, char literalChar) {
        stringQuote = qString;
        literalQuote = literalChar;
        return this;
    }
    ConfigBase* maxLayers(uint8_t layers) {
        maximumLayers = layers;
        return this;
    }
    ConfigBase* parentSeparator(char sep) {
        parentSeparatorChar = sep;
        return this;
    }
    ConfigBase* commentDefaults(bool comDef = true) {
        commentDefaultsBool = comDef;
        return this;
    }
    std::string& sectionRef() {
        return configSection;
    }
    CLI11_NODISCARD const std::string& section() const {
        return configSection;
    }
    ConfigBase* section(const std::string& sectionName) {
        configSection = sectionName;
        return this;
    }

    int16_t& indexRef() {
        return configIndex;
    }
    CLI11_NODISCARD int16_t index() const {
        return configIndex;
    }
    ConfigBase* index(int16_t sectionIndex) {
        configIndex = sectionIndex;
        return this;
    }
    ConfigBase* allowDuplicateFields(bool value = true) {
        allowMultipleDuplicateFields = value;
        return this;
    }
};

using ConfigTOML = ConfigBase;

class ConfigINI : public ConfigTOML {
   public:
    ConfigINI() {
        commentChar = ';';
        arrayStart = '\0';
        arrayEnd = '\0';
        arraySeparator = ' ';
        valueDelimiter = '=';
    }
};

class Option;

class Validator {
   protected:
    std::function<std::string()> desc_function_{[]() { return std::string{}; }};

    std::function<std::string(std::string&)> func_{[](std::string&) { return std::string{}; }};
    std::string name_{};
    int application_index_ = -1;
    bool active_{true};
    bool non_modifying_{false};

    Validator(std::string validator_desc, std::function<std::string(std::string&)> func)
        : desc_function_([validator_desc]() { return validator_desc; }), func_(std::move(func)) {}

   public:
    Validator() = default;
    explicit Validator(std::string validator_desc) : desc_function_([validator_desc]() { return validator_desc; }) {}
    Validator(std::function<std::string(std::string&)> op, std::string validator_desc, std::string validator_name = "")
        : desc_function_([validator_desc]() { return validator_desc; }),
          func_(std::move(op)),
          name_(std::move(validator_name)) {}
    Validator& operation(std::function<std::string(std::string&)> op) {
        func_ = std::move(op);
        return *this;
    }
    std::string operator()(std::string& str) const;

    std::string operator()(const std::string& str) const {
        std::string value = str;
        return (active_) ? func_(value) : std::string{};
    }

    Validator& description(std::string validator_desc) {
        desc_function_ = [validator_desc]() { return validator_desc; };
        return *this;
    }
    CLI11_NODISCARD Validator description(std::string validator_desc) const;

    CLI11_NODISCARD std::string get_description() const {
        if (active_) {
            return desc_function_();
        }
        return std::string{};
    }
    Validator& name(std::string validator_name) {
        name_ = std::move(validator_name);
        return *this;
    }
    CLI11_NODISCARD Validator name(std::string validator_name) const {
        Validator newval(*this);
        newval.name_ = std::move(validator_name);
        return newval;
    }
    CLI11_NODISCARD const std::string& get_name() const {
        return name_;
    }
    Validator& active(bool active_val = true) {
        active_ = active_val;
        return *this;
    }
    CLI11_NODISCARD Validator active(bool active_val = true) const {
        Validator newval(*this);
        newval.active_ = active_val;
        return newval;
    }

    Validator& non_modifying(bool no_modify = true) {
        non_modifying_ = no_modify;
        return *this;
    }
    Validator& application_index(int app_index) {
        application_index_ = app_index;
        return *this;
    }
    CLI11_NODISCARD Validator application_index(int app_index) const {
        Validator newval(*this);
        newval.application_index_ = app_index;
        return newval;
    }
    CLI11_NODISCARD int get_application_index() const {
        return application_index_;
    }
    CLI11_NODISCARD bool get_active() const {
        return active_;
    }

    CLI11_NODISCARD bool get_modifying() const {
        return !non_modifying_;
    }

    Validator operator&(const Validator& other) const;

    Validator operator|(const Validator& other) const;

    Validator operator!() const;

   private:
    void _merge_description(const Validator& val1, const Validator& val2, const std::string& merger);
};

using CustomValidator = Validator;

namespace detail {

enum class path_type : std::uint8_t {
    nonexistent,
    file,
    directory
};

CLI11_INLINE path_type check_path(const char* file) noexcept;

class ExistingFileValidator : public Validator {
   public:
    ExistingFileValidator();
};

class ExistingDirectoryValidator : public Validator {
   public:
    ExistingDirectoryValidator();
};

class ExistingPathValidator : public Validator {
   public:
    ExistingPathValidator();
};

class NonexistentPathValidator : public Validator {
   public:
    NonexistentPathValidator();
};

class EscapedStringTransformer : public Validator {
   public:
    EscapedStringTransformer();
};

}  

CLI11_MODULE_INLINE const detail::ExistingFileValidator ExistingFile;

CLI11_MODULE_INLINE const detail::ExistingDirectoryValidator ExistingDirectory;

CLI11_MODULE_INLINE const detail::ExistingPathValidator ExistingPath;

CLI11_MODULE_INLINE const detail::NonexistentPathValidator NonexistentPath;

CLI11_MODULE_INLINE const detail::EscapedStringTransformer EscapedString;

class FileOnDefaultPath : public Validator {
   public:
    explicit FileOnDefaultPath(std::string default_path, bool enableErrorReturn = true);
};

class Range : public Validator {
   public:
    template <typename T>
    Range(T min_val, T max_val, const std::string& validator_name = std::string{}) : Validator(validator_name) {
        if (validator_name.empty()) {
            std::stringstream out;
            out << detail::type_name<T>() << " in [" << min_val << " - " << max_val << "]";
            description(out.str());
        }

        func_ = [min_val, max_val](std::string& input) {
            using CLI::detail::lexical_cast;
            T val;
            bool converted = lexical_cast(input, val);
            if ((!converted) || (val < min_val || val > max_val)) {
                std::stringstream out;
                out << "Value " << input << " not in range [";
                out << min_val << " - " << max_val << "]";
                return out.str();
            }
            return std::string{};
        };
    }

    template <typename T>
    explicit Range(T max_val, const std::string& validator_name = std::string{})
        : Range(static_cast<T>(0), max_val, validator_name) {}
};

CLI11_MODULE_INLINE const Range NonNegativeNumber((std::numeric_limits<double>::max)(), "NONNEGATIVE");

CLI11_MODULE_INLINE const Range PositiveNumber((std::numeric_limits<double>::min)(),
                                               (std::numeric_limits<double>::max)(), "POSITIVE");

namespace detail {

template <typename T>
inline typename std::enable_if<std::is_signed<T>::value, T>::type overflowCheck(const T& a, const T& b) {
    if ((a > 0) == (b > 0)) {
        return ((std::numeric_limits<T>::max)() / (std::abs)(a) < (std::abs)(b));
    }
    return ((std::numeric_limits<T>::min)() / (std::abs)(a) > -(std::abs)(b));
}
template <typename T>
inline typename std::enable_if<!std::is_signed<T>::value, T>::type overflowCheck(const T& a, const T& b) {
    return ((std::numeric_limits<T>::max)() / a < b);
}

template <typename T>
typename std::enable_if<std::is_integral<T>::value, bool>::type checked_multiply(T& a, T b) {
    if (a == 0 || b == 0 || a == 1 || b == 1) {
        a *= b;
        return true;
    }
    if (a == (std::numeric_limits<T>::min)() || b == (std::numeric_limits<T>::min)()) {
        return false;
    }
    if (overflowCheck(a, b)) {
        return false;
    }
    a *= b;
    return true;
}

template <typename T>
typename std::enable_if<std::is_floating_point<T>::value, bool>::type checked_multiply(T& a, T b) {
    T c = a * b;
    if (std::isinf(c) && !std::isinf(a) && !std::isinf(b)) {
        return false;
    }
    a = c;
    return true;
}
CLI11_INLINE std::pair<std::string, std::string> split_program_name(std::string commandline);

}  

CLI11_INLINE std::string Validator::operator()(std::string& str) const {
    std::string retstring;
    if (active_) {
        if (non_modifying_) {
            std::string value = str;
            retstring = func_(value);
        } else {
            retstring = func_(str);
        }
    }
    return retstring;
}

CLI11_NODISCARD CLI11_INLINE Validator Validator::description(std::string validator_desc) const {
    Validator newval(*this);
    newval.desc_function_ = [validator_desc]() { return validator_desc; };
    return newval;
}

CLI11_INLINE Validator Validator::operator&(const Validator& other) const {
    Validator newval;

    newval._merge_description(*this, other, " AND ");

    const std::function<std::string(std::string & filename)>& f1 = func_;
    const std::function<std::string(std::string & filename)>& f2 = other.func_;

    newval.func_ = [f1, f2](std::string& input) {
        std::string s1 = f1(input);
        std::string s2 = f2(input);
        if (!s1.empty() && !s2.empty())
            return std::string("(") + s1 + ") AND (" + s2 + ")";
        return s1 + s2;
    };

    newval.active_ = active_ && other.active_;
    newval.application_index_ = application_index_;
    return newval;
}

CLI11_INLINE Validator Validator::operator|(const Validator& other) const {
    Validator newval;

    newval._merge_description(*this, other, " OR ");

    const std::function<std::string(std::string&)>& f1 = func_;
    const std::function<std::string(std::string&)>& f2 = other.func_;

    newval.func_ = [f1, f2](std::string& input) {
        std::string s1 = f1(input);
        std::string s2 = f2(input);
        if (s1.empty() || s2.empty())
            return std::string();

        return std::string("(") + s1 + ") OR (" + s2 + ")";
    };
    newval.active_ = active_ && other.active_;
    newval.application_index_ = application_index_;
    return newval;
}

CLI11_INLINE Validator Validator::operator!() const {
    Validator newval;
    const std::function<std::string()>& dfunc1 = desc_function_;
    newval.desc_function_ = [dfunc1]() {
        auto str = dfunc1();
        return (!str.empty()) ? std::string("NOT ") + str : std::string{};
    };
    const std::function<std::string(std::string & res)>& f1 = func_;

    newval.func_ = [f1, dfunc1](std::string& test) -> std::string {
        std::string s1 = f1(test);
        if (s1.empty()) {
            return std::string("check ") + dfunc1() + " succeeded improperly";
        }
        return std::string{};
    };
    newval.active_ = active_;
    newval.application_index_ = application_index_;
    return newval;
}

CLI11_INLINE void Validator::_merge_description(const Validator& val1, const Validator& val2,
                                                const std::string& merger) {
    const std::function<std::string()>& dfunc1 = val1.desc_function_;
    const std::function<std::string()>& dfunc2 = val2.desc_function_;

    desc_function_ = [=]() {
        std::string f1 = dfunc1();
        std::string f2 = dfunc2();
        if ((f1.empty()) || (f2.empty())) {
            return f1 + f2;
        }
        return std::string(1, '(') + f1 + ')' + merger + '(' + f2 + ')';
    };
}

namespace detail {

#if defined CLI11_HAS_FILESYSTEM && CLI11_HAS_FILESYSTEM > 0
CLI11_INLINE path_type check_path(const char* file) noexcept {
    std::error_code ec;
    auto stat = std::filesystem::status(to_path(file), ec);
    if (ec) {
        return path_type::nonexistent;
    }
    switch (stat.type()) {
        case std::filesystem::file_type::none:
        case std::filesystem::file_type::not_found:
            return path_type::nonexistent;
        case std::filesystem::file_type::directory:
            return path_type::directory;
        case std::filesystem::file_type::symlink:
        case std::filesystem::file_type::block:
        case std::filesystem::file_type::character:
        case std::filesystem::file_type::fifo:
        case std::filesystem::file_type::socket:
        case std::filesystem::file_type::regular:
        case std::filesystem::file_type::unknown:
        default:
            return path_type::file;
    }
}
#else
CLI11_INLINE path_type check_path(const char* file) noexcept {
#if defined(_MSC_VER)
    struct __stat64 buffer;
    if (_stat64(file, &buffer) == 0) {
        return ((buffer.st_mode & S_IFDIR) != 0) ? path_type::directory : path_type::file;
    }
#else
    struct stat buffer;
    if (stat(file, &buffer) == 0) {
        return ((buffer.st_mode & S_IFDIR) != 0) ? path_type::directory : path_type::file;
    }
#endif
    return path_type::nonexistent;
}
#endif

CLI11_INLINE ExistingFileValidator::ExistingFileValidator() : Validator("FILE") {
    func_ = [](std::string& filename) {
        auto path_result = check_path(filename.c_str());
        if (path_result == path_type::nonexistent) {
            return "File does not exist: " + filename;
        }
        if (path_result == path_type::directory) {
            return "File is actually a directory: " + filename;
        }
        return std::string();
    };
}

CLI11_INLINE ExistingDirectoryValidator::ExistingDirectoryValidator() : Validator("DIR") {
    func_ = [](std::string& filename) {
        auto path_result = check_path(filename.c_str());
        if (path_result == path_type::nonexistent) {
            return "Directory does not exist: " + filename;
        }
        if (path_result == path_type::file) {
            return "Directory is actually a file: " + filename;
        }
        return std::string();
    };
}

CLI11_INLINE ExistingPathValidator::ExistingPathValidator() : Validator("PATH(existing)") {
    func_ = [](std::string& filename) {
        auto path_result = check_path(filename.c_str());
        if (path_result == path_type::nonexistent) {
            return "Path does not exist: " + filename;
        }
        return std::string();
    };
}

CLI11_INLINE NonexistentPathValidator::NonexistentPathValidator() : Validator("PATH(non-existing)") {
    func_ = [](std::string& filename) {
        auto path_result = check_path(filename.c_str());
        if (path_result != path_type::nonexistent) {
            return "Path already exists: " + filename;
        }
        return std::string();
    };
}

CLI11_INLINE EscapedStringTransformer::EscapedStringTransformer() {
    func_ = [](std::string& str) {
        try {
            if (str.size() > 1 && (str.front() == '\"' || str.front() == '\'' || str.front() == '`') &&
                str.front() == str.back()) {
                process_quoted_string(str);
            } else if (str.find_first_of('\\') != std::string::npos) {
                if (detail::is_binary_escaped_string(str)) {
                    str = detail::extract_binary_string(str);
                } else {
                    str = remove_escaped_characters(str);
                }
            }
            return std::string{};
        } catch (const std::invalid_argument& ia) {
            return std::string(ia.what());
        }
    };
}
}  

CLI11_INLINE FileOnDefaultPath::FileOnDefaultPath(std::string default_path, bool enableErrorReturn)
    : Validator("FILE") {
    func_ = [default_path, enableErrorReturn](std::string& filename) {
        auto path_result = detail::check_path(filename.c_str());
        if (path_result == detail::path_type::nonexistent) {
            std::string test_file_path = default_path;
            if (default_path.back() != '/' && default_path.back() != '\\') {
                test_file_path += '/';
            }
            test_file_path.append(filename);
            path_result = detail::check_path(test_file_path.c_str());
            if (path_result == detail::path_type::file) {
                filename = test_file_path;
            } else {
                if (enableErrorReturn) {
                    return "File does not exist: " + filename;
                }
            }
        }
        return std::string{};
    };
}

namespace detail {

CLI11_INLINE std::pair<std::string, std::string> split_program_name(std::string commandline) {
    std::pair<std::string, std::string> vals;
    trim(commandline);
    auto esp = commandline.find_first_of(' ', 1);
    while (detail::check_path(commandline.substr(0, esp).c_str()) != path_type::file) {
        esp = commandline.find_first_of(' ', esp + 1);
        if (esp == std::string::npos) {
            if (commandline[0] == '"' || commandline[0] == '\'' || commandline[0] == '`') {
                bool embeddedQuote = false;
                auto keyChar = commandline[0];
                auto end = commandline.find_first_of(keyChar, 1);
                while ((end != std::string::npos) && (commandline[end - 1] == '\\')) {
                    end = commandline.find_first_of(keyChar, end + 1);
                    embeddedQuote = true;
                }
                if (end != std::string::npos) {
                    vals.first = commandline.substr(1, end - 1);
                    esp = end + 1;
                    if (embeddedQuote) {
                        vals.first = find_and_replace(vals.first, std::string("\\") + keyChar, std::string(1, keyChar));
                    }
                } else {
                    esp = commandline.find_first_of(' ', 1);
                }
            } else {
                esp = commandline.find_first_of(' ', 1);
            }

            break;
        }
    }
    if (vals.first.empty()) {
        vals.first = commandline.substr(0, esp);
        rtrim(vals.first);
    }

    vals.second = (esp < commandline.length() - 1) ? commandline.substr(esp + 1) : std::string{};
    ltrim(vals.second);
    return vals;
}

}  

namespace detail {

class IPV4Validator : public Validator {
   public:
    IPV4Validator();
};

}  

template <typename DesiredType>
class TypeValidator : public Validator {
   public:
    explicit TypeValidator(const std::string& validator_name)
        : Validator(validator_name, [](std::string& input_string) {
              using CLI::detail::lexical_cast;
              auto val = DesiredType();
              if (!lexical_cast(input_string, val)) {
                  return std::string("Failed parsing ") + input_string + " as a " + detail::type_name<DesiredType>();
              }
              return std::string{};
          }) {}
    TypeValidator() : TypeValidator(detail::type_name<DesiredType>()) {}
};

const TypeValidator<double> Number("NUMBER");

class Bound : public Validator {
   public:
    template <typename T>
    Bound(T min_val, T max_val) {
        std::stringstream out;
        out << detail::type_name<T>() << " bounded to [" << min_val << " - " << max_val << "]";
        description(out.str());

        func_ = [min_val, max_val](std::string& input) {
            using CLI::detail::lexical_cast;
            T val;
            bool converted = lexical_cast(input, val);
            if (!converted) {
                return std::string("Value ") + input + " could not be converted";
            }
            if (val < min_val)
                input = detail::to_string(min_val);
            else if (val > max_val)
                input = detail::to_string(max_val);

            return std::string{};
        };
    }

    template <typename T>
    explicit Bound(T max_val) : Bound(static_cast<T>(0), max_val) {}
};

CLI11_MODULE_INLINE const detail::IPV4Validator ValidIPV4;

namespace detail {
template <typename T,
          enable_if_t<is_copyable_ptr<typename std::remove_reference<T>::type>::value, detail::enabler> = detail::dummy>
auto smart_deref(T value) -> decltype(*value) {
    return *value;
}

template <typename T, enable_if_t<!is_copyable_ptr<typename std::remove_reference<T>::type>::value, detail::enabler> =
                          detail::dummy>
typename std::remove_reference<T>::type& smart_deref(T& value) {
    // NOLINTNEXTLINE
    return value;
}
template <typename T>
std::string generate_set(const T& set) {
    using element_t = typename detail::element_type<T>::type;
    using iteration_type_t = typename detail::pair_adaptor<element_t>::value_type;
    std::string out(1, '{');
    out.append(detail::join(
        detail::smart_deref(set), [](const iteration_type_t& v) { return detail::pair_adaptor<element_t>::first(v); },
        ","));
    out.push_back('}');
    return out;
}

template <typename T>
std::string generate_map(const T& map, bool key_only = false) {
    using element_t = typename detail::element_type<T>::type;
    using iteration_type_t = typename detail::pair_adaptor<element_t>::value_type;
    std::string out(1, '{');
    out.append(detail::join(
        detail::smart_deref(map),
        [key_only](const iteration_type_t& v) {
            std::string res{detail::to_string(detail::pair_adaptor<element_t>::first(v))};

            if (!key_only) {
                res.append("->");
                res += detail::to_string(detail::pair_adaptor<element_t>::second(v));
            }
            return res;
        },
        ","));
    out.push_back('}');
    return out;
}

template <typename C, typename V>
struct has_find {
    template <typename CC, typename VV>
    static auto test(int) -> decltype(std::declval<CC>().find(std::declval<VV>()), std::true_type());
    template <typename, typename>
    static auto test(...) -> decltype(std::false_type());

    static const auto value = decltype(test<C, V>(0))::value;
    using type = std::integral_constant<bool, value>;
};

template <typename T, typename V, enable_if_t<!has_find<T, V>::value, detail::enabler> = detail::dummy>
auto search(const T& set, const V& val) -> std::pair<bool, decltype(std::begin(detail::smart_deref(set)))> {
    using element_t = typename detail::element_type<T>::type;
    auto& setref = detail::smart_deref(set);
    auto it = std::find_if(std::begin(setref), std::end(setref), [&val](decltype(*std::begin(setref)) v) {
        return (detail::pair_adaptor<element_t>::first(v) == val);
    });
    return {(it != std::end(setref)), it};
}

template <typename T, typename V, enable_if_t<has_find<T, V>::value, detail::enabler> = detail::dummy>
auto search(const T& set, const V& val) -> std::pair<bool, decltype(std::begin(detail::smart_deref(set)))> {
    auto& setref = detail::smart_deref(set);
    auto it = setref.find(val);
    return {(it != std::end(setref)), it};
}

template <typename T, typename V>
auto search(const T& set, const V& val, const std::function<V(V)>& filter_function)
    -> std::pair<bool, decltype(std::begin(detail::smart_deref(set)))> {
    using element_t = typename detail::element_type<T>::type;
    auto res = search(set, val);
    if ((res.first) || (!(filter_function))) {
        return res;
    }
    auto& setref = detail::smart_deref(set);
    auto it = std::find_if(std::begin(setref), std::end(setref), [&](decltype(*std::begin(setref)) v) {
        V a{detail::pair_adaptor<element_t>::first(v)};
        a = filter_function(a);
        return (a == val);
    });
    return {(it != std::end(setref)), it};
}

}  
class IsMember : public Validator {
   public:
    using filter_fn_t = std::function<std::string(std::string)>;

    template <typename T, typename... Args>
    IsMember(std::initializer_list<T> values, Args&&... args)
        : IsMember(std::vector<T>(values), std::forward<Args>(args)...) {}

    template <typename T>
    explicit IsMember(T&& set) : IsMember(std::forward<T>(set), nullptr) {}

    template <typename T, typename F>
    explicit IsMember(T set, F filter_function) {
        using element_t = typename detail::element_type<T>::type;
        using item_t = typename detail::pair_adaptor<element_t>::first_type;

        using local_item_t = typename IsMemberType<item_t>::type;

        std::function<local_item_t(local_item_t)> filter_fn = filter_function;

        desc_function_ = [set]() { return detail::generate_set(detail::smart_deref(set)); };

        func_ = [set, filter_fn](std::string& input) {
            using CLI::detail::lexical_cast;
            local_item_t b;
            if (!lexical_cast(input, b)) {
                throw ValidationError(input);
            }
            if (filter_fn) {
                b = filter_fn(b);
            }
            auto res = detail::search(set, b, filter_fn);
            if (res.first) {
                if (filter_fn) {
                    input = detail::value_string(detail::pair_adaptor<element_t>::first(*(res.second)));
                }

                return std::string{};
            }

            return input + " not in " + detail::generate_set(detail::smart_deref(set));
        };
    }

    template <typename T, typename... Args>
    IsMember(T&& set, filter_fn_t filter_fn_1, filter_fn_t filter_fn_2, Args&&... other)
        : IsMember(
              std::forward<T>(set), [filter_fn_1, filter_fn_2](std::string a) { return filter_fn_2(filter_fn_1(a)); },
              other...) {}
};

template <typename T>
using TransformPairs = std::vector<std::pair<std::string, T>>;

class Transformer : public Validator {
   public:
    using filter_fn_t = std::function<std::string(std::string)>;

    template <typename... Args>
    Transformer(std::initializer_list<std::pair<std::string, std::string>> values, Args&&... args)
        : Transformer(TransformPairs<std::string>(values), std::forward<Args>(args)...) {}

    template <typename T>
    explicit Transformer(T&& mapping) : Transformer(std::forward<T>(mapping), nullptr) {}

    template <typename T, typename F>
    explicit Transformer(T mapping, F filter_function) {
        static_assert(detail::pair_adaptor<typename detail::element_type<T>::type>::value,
                      "mapping must produce value pairs");
        using element_t = typename detail::element_type<T>::type;
        using item_t = typename detail::pair_adaptor<element_t>::first_type;
        using local_item_t = typename IsMemberType<item_t>::type;

        std::function<local_item_t(local_item_t)> filter_fn = filter_function;

        desc_function_ = [mapping]() { return detail::generate_map(detail::smart_deref(mapping)); };

        func_ = [mapping, filter_fn](std::string& input) {
            using CLI::detail::lexical_cast;
            local_item_t b;
            if (!lexical_cast(input, b)) {
                return std::string();
            }
            if (filter_fn) {
                b = filter_fn(b);
            }
            auto res = detail::search(mapping, b, filter_fn);
            if (res.first) {
                input = detail::value_string(detail::pair_adaptor<element_t>::second(*res.second));
            }
            return std::string{};
        };
    }

    template <typename T, typename... Args>
    Transformer(T&& mapping, filter_fn_t filter_fn_1, filter_fn_t filter_fn_2, Args&&... other)
        : Transformer(
              std::forward<T>(mapping),
              [filter_fn_1, filter_fn_2](std::string a) { return filter_fn_2(filter_fn_1(a)); }, other...) {}
};

class CheckedTransformer : public Validator {
   public:
    using filter_fn_t = std::function<std::string(std::string)>;

    template <typename... Args>
    CheckedTransformer(std::initializer_list<std::pair<std::string, std::string>> values, Args&&... args)
        : CheckedTransformer(TransformPairs<std::string>(values), std::forward<Args>(args)...) {}

    template <typename T>
    explicit CheckedTransformer(T mapping) : CheckedTransformer(std::move(mapping), nullptr) {}

    template <typename T, typename F>
    explicit CheckedTransformer(T mapping, F filter_function) {
        static_assert(detail::pair_adaptor<typename detail::element_type<T>::type>::value,
                      "mapping must produce value pairs");
        using element_t = typename detail::element_type<T>::type;
        using item_t = typename detail::pair_adaptor<element_t>::first_type;
        using local_item_t = typename IsMemberType<item_t>::type;
        using iteration_type_t = typename detail::pair_adaptor<element_t>::value_type;

        std::function<local_item_t(local_item_t)> filter_fn = filter_function;

        auto tfunc = [mapping]() {
            std::string out("value in ");
            out += detail::generate_map(detail::smart_deref(mapping)) + " OR {";
            out += detail::join(
                detail::smart_deref(mapping),
                [](const iteration_type_t& v) {
                    return detail::value_string(detail::pair_adaptor<element_t>::second(v));
                },
                ",");
            out.push_back('}');
            return out;
        };

        desc_function_ = tfunc;

        func_ = [mapping, tfunc, filter_fn](std::string& input) {
            using CLI::detail::lexical_cast;
            local_item_t b;
            bool converted = lexical_cast(input, b);
            if (converted) {
                if (filter_fn) {
                    b = filter_fn(b);
                }
                auto res = detail::search(mapping, b, filter_fn);
                if (res.first) {
                    input = detail::value_string(detail::pair_adaptor<element_t>::second(*res.second));
                    return std::string{};
                }
            }
            for (const auto& v : detail::smart_deref(mapping)) {
                auto output_string = detail::value_string(detail::pair_adaptor<element_t>::second(v));
                if (output_string == input) {
                    return std::string();
                }
            }

            return "Check " + input + " " + tfunc() + " FAILED";
        };
    }

    template <typename T, typename... Args>
    CheckedTransformer(T&& mapping, filter_fn_t filter_fn_1, filter_fn_t filter_fn_2, Args&&... other)
        : CheckedTransformer(
              std::forward<T>(mapping),
              [filter_fn_1, filter_fn_2](std::string a) { return filter_fn_2(filter_fn_1(a)); }, other...) {}
};

inline std::string ignore_case(std::string item) {
    return detail::to_lower(item);
}

inline std::string ignore_underscore(std::string item) {
    return detail::remove_underscore(item);
}

inline std::string ignore_space(std::string item) {
    item.erase(std::remove(std::begin(item), std::end(item), ' '), std::end(item));
    item.erase(std::remove(std::begin(item), std::end(item), '\t'), std::end(item));
    return item;
}

class AsNumberWithUnit : public Validator {
   public:
    enum Options : std::uint8_t {
        CASE_SENSITIVE = 0,
        CASE_INSENSITIVE = 1,
        UNIT_OPTIONAL = 0,
        UNIT_REQUIRED = 2,
        DEFAULT = CASE_INSENSITIVE | UNIT_OPTIONAL
    };

    template <typename Number>
    explicit AsNumberWithUnit(std::map<std::string, Number> mapping, Options opts = DEFAULT,
                              const std::string& unit_name = "UNIT") {
        description(generate_description<Number>(unit_name, opts));
        validate_mapping(mapping, opts);

        func_ = [mapping, opts](std::string& input) -> std::string {
            Number num{};

            detail::rtrim(input);
            if (input.empty()) {
                throw ValidationError("Input is empty");
            }

            auto unit_begin = input.end();
            while (unit_begin > input.begin() && std::isalpha(*(unit_begin - 1), std::locale())) {
                --unit_begin;
            }

            std::string unit{unit_begin, input.end()};
            input.resize(static_cast<std::size_t>(std::distance(input.begin(), unit_begin)));
            detail::trim(input);

            if (opts & UNIT_REQUIRED && unit.empty()) {
                throw ValidationError("Missing mandatory unit");
            }
            if (opts & CASE_INSENSITIVE) {
                unit = detail::to_lower(unit);
            }
            if (unit.empty()) {
                using CLI::detail::lexical_cast;
                if (!lexical_cast(input, num)) {
                    throw ValidationError(std::string("Value ") + input + " could not be converted to " +
                                          detail::type_name<Number>());
                }
                return {};
            }

            auto it = mapping.find(unit);
            if (it == mapping.end()) {
                throw ValidationError(unit +
                                      " unit not recognized. "
                                      "Allowed values: " +
                                      detail::generate_map(mapping, true));
            }

            if (!input.empty()) {
                using CLI::detail::lexical_cast;
                bool converted = lexical_cast(input, num);
                if (!converted) {
                    throw ValidationError(std::string("Value ") + input + " could not be converted to " +
                                          detail::type_name<Number>());
                }
                bool ok = detail::checked_multiply(num, it->second);
                if (!ok) {
                    throw ValidationError(detail::to_string(num) + " multiplied by " + unit +
                                          " factor would cause number overflow. Use smaller value.");
                }
            } else {
                num = static_cast<Number>(it->second);
            }

            input = detail::to_string(num);

            return {};
        };
    }

   private:
    template <typename Number>
    static void validate_mapping(std::map<std::string, Number>& mapping, Options opts) {
        for (auto& kv : mapping) {
            if (kv.first.empty()) {
                throw ValidationError("Unit must not be empty.");
            }
            if (!detail::isalpha(kv.first)) {
                throw ValidationError("Unit must contain only letters.");
            }
        }

        if (opts & CASE_INSENSITIVE) {
            std::map<std::string, Number> lower_mapping;
            for (auto& kv : mapping) {
                auto s = detail::to_lower(kv.first);
                if (lower_mapping.count(s)) {
                    throw ValidationError(std::string("Several matching lowercase unit representations are found: ") +
                                          s);
                }
                lower_mapping[detail::to_lower(kv.first)] = kv.second;
            }
            mapping = std::move(lower_mapping);
        }
    }

    template <typename Number>
    static std::string generate_description(const std::string& name, Options opts) {
        std::stringstream out;
        out << detail::type_name<Number>() << ' ';
        if (opts & UNIT_REQUIRED) {
            out << name;
        } else {
            out << '[' << name << ']';
        }
        return out.str();
    }
};

inline AsNumberWithUnit::Options operator|(const AsNumberWithUnit::Options& a, const AsNumberWithUnit::Options& b) {
    return static_cast<AsNumberWithUnit::Options>(static_cast<int>(a) | static_cast<int>(b));
}

class AsSizeValue : public AsNumberWithUnit {
   public:
    using result_t = std::uint64_t;

    explicit AsSizeValue(bool kb_is_1000);

   private:
    static std::map<std::string, result_t> init_mapping(bool kb_is_1000);

    static std::map<std::string, result_t> get_mapping(bool kb_is_1000);
};

#if defined(CLI11_ENABLE_EXTRA_VALIDATORS) && CLI11_ENABLE_EXTRA_VALIDATORS != 0
#if CLI11_HAS_FILESYSTEM
namespace detail {
enum class Permission : std::uint8_t {
    none = 0,
    read = 1,
    write = 2,
    exec = 4
};
class PermissionValidator : public Validator {
   public:
    explicit PermissionValidator(Permission permission);
};
}  

const detail::PermissionValidator ReadPermissions(detail::Permission::read);

const detail::PermissionValidator WritePermissions(detail::Permission::write);

const detail::PermissionValidator ExecPermissions(detail::Permission::exec);
#endif

#endif

namespace detail {

CLI11_INLINE IPV4Validator::IPV4Validator() : Validator("IPV4") {
    func_ = [](std::string& ip_addr) {
        auto cdot = std::count(ip_addr.begin(), ip_addr.end(), '.');
        if (cdot != 3u) {
            return std::string("Invalid IPV4 address: must have 3 separators");
        }
        auto result = CLI::detail::split(ip_addr, '.');
        if (result.size() != 4) {
            return std::string("Invalid IPV4 address: must have four parts (") + ip_addr + ')';
        }
        int num = 0;
        for (const auto& var : result) {
            using CLI::detail::lexical_cast;
            bool retval = lexical_cast(var, num);
            if (!retval) {
                return std::string("Failed parsing number (") + var + ')';
            }
            if (num < 0 || num > 255) {
                return std::string("Each IP number must be between 0 and 255 ") + var;
            }
        }
        return std::string{};
    };
}

}  

CLI11_INLINE AsSizeValue::AsSizeValue(bool kb_is_1000) : AsNumberWithUnit(get_mapping(kb_is_1000)) {
    if (kb_is_1000) {
        description("SIZE [b, kb(=1000b), kib(=1024b), ...]");
    } else {
        description("SIZE [b, kb(=1024b), ...]");
    }
}

CLI11_INLINE std::map<std::string, AsSizeValue::result_t> AsSizeValue::init_mapping(bool kb_is_1000) {
    std::map<std::string, result_t> m;
    result_t k_factor = kb_is_1000 ? 1000 : 1024;
    result_t ki_factor = 1024;
    result_t k = 1;
    result_t ki = 1;
    m["b"] = 1;
    for (std::string p : {"k", "m", "g", "t", "p", "e"}) {
        k *= k_factor;
        ki *= ki_factor;
        m[p] = k;
        m[p + "b"] = k;
        m[p + "i"] = ki;
        m[p + "ib"] = ki;
    }
    return m;
}

CLI11_INLINE std::map<std::string, AsSizeValue::result_t> AsSizeValue::get_mapping(bool kb_is_1000) {
    if (kb_is_1000) {
        static auto m = init_mapping(true);
        return m;
    }
    static auto m = init_mapping(false);
    return m;
}

namespace detail {}

#if defined(CLI11_ENABLE_EXTRA_VALIDATORS) && CLI11_ENABLE_EXTRA_VALIDATORS != 0
namespace detail {

#if defined CLI11_HAS_FILESYSTEM && CLI11_HAS_FILESYSTEM > 0
CLI11_INLINE PermissionValidator::PermissionValidator(Permission permission) {
    std::filesystem::perms permission_code = std::filesystem::perms::none;
    std::string permission_name;
    switch (permission) {
        case Permission::read:
            permission_code = std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
                              std::filesystem::perms::others_read;
            permission_name = "read";
            break;
        case Permission::write:
            permission_code = std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                              std::filesystem::perms::others_write;
            permission_name = "write";
            break;
        case Permission::exec:
            permission_code = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                              std::filesystem::perms::others_exec;
            permission_name = "exec";
            break;
        case Permission::none:
        default:
            permission_code = std::filesystem::perms::none;
            break;
    }
    func_ = [permission_code](std::string& path) {
        std::error_code ec;
        auto p = std::filesystem::path(path);
        if (!std::filesystem::exists(p, ec)) {
            return std::string("Path does not exist: ") + path;
        }
        if (ec) {
            return std::string("Error checking path: ") + ec.message();
        }
        if (permission_code == std::filesystem::perms::none) {
            return std::string{};
        }
        auto perms = std::filesystem::status(p, ec).permissions();
        if (ec) {
            return std::string("Error checking path status: ") + ec.message();
        }
        if ((perms & permission_code) == std::filesystem::perms::none) {
            return std::string("Path does not have required permissions: ") + path;
        }
        return std::string{};
    };
    description("Path with " + permission_name + " permission");
}
#endif

}  
#endif

class Option;
class App;

enum class AppFormatMode : std::uint8_t {
    Normal,
    All,
    Sub,
};

class FormatterBase {
   protected:
    std::size_t column_width_{30};

    float long_option_alignment_ratio_{1 / 3.f};

    std::size_t right_column_width_{65};

    std::size_t description_paragraph_width_{80};

    std::size_t footer_paragraph_width_{80};

    bool enable_description_formatting_{true};
    bool enable_footer_formatting_{true};

    bool enable_option_defaults_{true};
    bool enable_option_type_names_{true};
    bool enable_default_flag_values_{true};
    std::map<std::string, std::string> labels_{};

   public:
    FormatterBase() = default;
    FormatterBase(const FormatterBase&) = default;
    FormatterBase(FormatterBase&&) = default;
    FormatterBase& operator=(const FormatterBase&) = default;
    FormatterBase& operator=(FormatterBase&&) = default;

    virtual ~FormatterBase() noexcept {}  // NOLINT(modernize-use-equals-default)

    virtual std::string make_help(const App*, std::string, AppFormatMode) const = 0;

    void label(std::string key, std::string val) {
        labels_[key] = val;
    }

    void column_width(std::size_t val) {
        column_width_ = val;
    }

    void long_option_alignment_ratio(float ratio) {
        long_option_alignment_ratio_ =
            (ratio >= 0.0f) ? ((ratio <= 1.0f) ? ratio : 1.0f / ratio) : ((ratio < -1.0f) ? 1.0f / (-ratio) : -ratio);
    }

    void right_column_width(std::size_t val) {
        right_column_width_ = val;
    }

    void description_paragraph_width(std::size_t val) {
        description_paragraph_width_ = val;
    }

    void footer_paragraph_width(std::size_t val) {
        footer_paragraph_width_ = val;
    }
    void enable_description_formatting(bool value = true) {
        enable_description_formatting_ = value;
    }
    void enable_footer_formatting(bool value = true) {
        enable_footer_formatting_ = value;
    }

    void enable_option_defaults(bool value = true) {
        enable_option_defaults_ = value;
    }
    void enable_option_type_names(bool value = true) {
        enable_option_type_names_ = value;
    }
    void enable_default_flag_values(bool value = true) {
        enable_default_flag_values_ = value;
    }

    CLI11_NODISCARD std::string get_label(std::string key) const {
        if (labels_.find(key) == labels_.end())
            return key;
        return labels_.at(key);
    }

    CLI11_NODISCARD std::size_t get_column_width() const {
        return column_width_;
    }

    CLI11_NODISCARD std::size_t get_right_column_width() const {
        return right_column_width_;
    }

    CLI11_NODISCARD std::size_t get_description_paragraph_width() const {
        return description_paragraph_width_;
    }

    CLI11_NODISCARD std::size_t get_footer_paragraph_width() const {
        return footer_paragraph_width_;
    }

    CLI11_NODISCARD float get_long_option_alignment_ratio() const {
        return long_option_alignment_ratio_;
    }

    CLI11_NODISCARD bool is_description_paragraph_formatting_enabled() const {
        return enable_description_formatting_;
    }

    CLI11_NODISCARD bool is_footer_paragraph_formatting_enabled() const {
        return enable_footer_formatting_;
    }

    CLI11_NODISCARD bool is_option_defaults_enabled() const {
        return enable_option_defaults_;
    }

    CLI11_NODISCARD bool is_option_type_names_enabled() const {
        return enable_option_type_names_;
    }

    CLI11_NODISCARD bool is_default_flag_values_enabled() const {
        return enable_default_flag_values_;
    }
};

class FormatterLambda final : public FormatterBase {
    using funct_t = std::function<std::string(const App*, std::string, AppFormatMode)>;

    funct_t lambda_;

   public:
    explicit FormatterLambda(funct_t funct) : lambda_(std::move(funct)) {}

    ~FormatterLambda() noexcept override {}  // NOLINT(modernize-use-equals-default)

    std::string make_help(const App* app, std::string name, AppFormatMode mode) const override {
        return lambda_(app, name, mode);
    }
};

class Formatter : public FormatterBase {
   public:
    Formatter() = default;
    Formatter(const Formatter&) = default;
    Formatter(Formatter&&) = default;
    Formatter& operator=(const Formatter&) = default;
    Formatter& operator=(Formatter&&) = default;

    CLI11_NODISCARD virtual std::string make_group(std::string group, bool is_positional,
                                                   std::vector<const Option*> opts) const;

    virtual std::string make_positionals(const App* app) const;

    std::string make_groups(const App* app, AppFormatMode mode) const;

    virtual std::string make_subcommands(const App* app, AppFormatMode mode) const;

    virtual std::string make_subcommand(const App* sub) const;

    virtual std::string make_expanded(const App* sub, AppFormatMode mode) const;

    virtual std::string make_footer(const App* app) const;

    virtual std::string make_description(const App* app) const;

    virtual std::string make_usage(const App* app, std::string name) const;

    std::string make_help(const App* app, std::string, AppFormatMode mode) const override;

    virtual std::string make_option(const Option*, bool) const;

    virtual std::string make_option_name(const Option*, bool) const;

    virtual std::string make_option_opts(const Option*) const;

    virtual std::string make_option_desc(const Option*) const;

    virtual std::string make_option_usage(const Option* opt) const;
};

using results_t = std::vector<std::string>;
using callback_t = std::function<bool(const results_t&)>;

class Option;
class App;
class ConfigBase;

using Option_p = std::unique_ptr<Option>;
using Validator_p = std::shared_ptr<Validator>;

enum class MultiOptionPolicy : char {
    Throw,
    TakeLast,
    TakeFirst,
    Join,
    TakeAll,
    Sum,
    Reverse,
};

enum class CallbackPriority : std::uint8_t {
    FirstPreHelp = 0,
    First = 1,
    PreRequirementsCheckPreHelp = 2,
    PreRequirementsCheck = 3,
    NormalPreHelp = 4,
    Normal = 5,
    LastPreHelp = 6,
    Last = 7
};

template <typename CRTP>
class OptionBase {
    friend App;
    friend ConfigBase;

   protected:
    std::string group_ = std::string("OPTIONS");

    bool required_{false};

    bool ignore_case_{false};

    bool ignore_underscore_{false};

    bool configurable_{true};

    bool disable_flag_override_{false};

    char delimiter_{'\0'};

    bool always_capture_default_{false};

    MultiOptionPolicy multi_option_policy_{MultiOptionPolicy::Throw};

    CallbackPriority callback_priority_{CallbackPriority::Normal};

    template <typename T>
    void copy_to(T* other) const;

   public:
    CRTP* group(const std::string& name) {
        if (!detail::valid_alias_name_string(name)) {
            throw IncorrectConstruction("Group names may not contain newlines or null characters");
        }
        group_ = name;
        return static_cast<CRTP*>(this);
    }

    CRTP* required(bool value = true) {
        required_ = value;
        return static_cast<CRTP*>(this);
    }

    CRTP* mandatory(bool value = true) {
        return required(value);
    }

    CRTP* always_capture_default(bool value = true) {
        always_capture_default_ = value;
        return static_cast<CRTP*>(this);
    }

    CLI11_NODISCARD const std::string& get_group() const {
        return group_;
    }

    CLI11_NODISCARD bool get_required() const {
        return required_;
    }

    CLI11_NODISCARD bool get_ignore_case() const {
        return ignore_case_;
    }

    CLI11_NODISCARD bool get_ignore_underscore() const {
        return ignore_underscore_;
    }

    CLI11_NODISCARD bool get_configurable() const {
        return configurable_;
    }

    CLI11_NODISCARD bool get_disable_flag_override() const {
        return disable_flag_override_;
    }

    CLI11_NODISCARD char get_delimiter() const {
        return delimiter_;
    }

    CLI11_NODISCARD bool get_always_capture_default() const {
        return always_capture_default_;
    }

    CLI11_NODISCARD MultiOptionPolicy get_multi_option_policy() const {
        return multi_option_policy_;
    }

    CLI11_NODISCARD CallbackPriority get_callback_priority() const {
        return callback_priority_;
    }

    CRTP* take_last() {
        auto* self = static_cast<CRTP*>(this);
        self->multi_option_policy(MultiOptionPolicy::TakeLast);
        return self;
    }

    CRTP* take_first() {
        auto* self = static_cast<CRTP*>(this);
        self->multi_option_policy(MultiOptionPolicy::TakeFirst);
        return self;
    }

    CRTP* take_all() {
        auto self = static_cast<CRTP*>(this);
        self->multi_option_policy(MultiOptionPolicy::TakeAll);
        return self;
    }

    CRTP* join() {
        auto* self = static_cast<CRTP*>(this);
        self->multi_option_policy(MultiOptionPolicy::Join);
        return self;
    }

    CRTP* join(char delim) {
        auto self = static_cast<CRTP*>(this);
        self->delimiter_ = delim;
        self->multi_option_policy(MultiOptionPolicy::Join);
        return self;
    }

    CRTP* configurable(bool value = true) {
        configurable_ = value;
        return static_cast<CRTP*>(this);
    }

    CRTP* delimiter(char value = '\0') {
        delimiter_ = value;
        return static_cast<CRTP*>(this);
    }
};

class OptionDefaults : public OptionBase<OptionDefaults> {
   public:
    OptionDefaults() = default;

    OptionDefaults* callback_priority(CallbackPriority value = CallbackPriority::Normal) {
        callback_priority_ = value;
        return this;
    }

    OptionDefaults* multi_option_policy(MultiOptionPolicy value = MultiOptionPolicy::Throw) {
        multi_option_policy_ = value;
        return this;
    }

    OptionDefaults* ignore_case(bool value = true) {
        ignore_case_ = value;
        return this;
    }

    OptionDefaults* ignore_underscore(bool value = true) {
        ignore_underscore_ = value;
        return this;
    }

    OptionDefaults* disable_flag_override(bool value = true) {
        disable_flag_override_ = value;
        return this;
    }

    OptionDefaults* delimiter(char value = '\0') {
        delimiter_ = value;
        return this;
    }
};

class Option : public OptionBase<Option> {
    friend App;
    friend ConfigBase;

   protected:
    std::vector<std::string> snames_{};

    std::vector<std::string> lnames_{};

    std::vector<std::pair<std::string, std::string>> default_flag_values_{};

    std::vector<std::string> fnames_{};

    std::string pname_{};

    std::string envname_{};

    std::string description_{};

    std::string default_str_{};

    std::string option_text_{};

    std::function<std::string()> type_name_{[]() { return std::string(); }};

    std::function<std::string()> default_function_{};

    int type_size_max_{1};
    int type_size_min_{1};

    int expected_min_{1};
    int expected_max_{1};

    std::vector<Validator_p> validators_{};

    std::set<Option*> needs_{};

    std::set<Option*> excludes_{};

    App* parent_{nullptr};

    callback_t callback_{};

    results_t results_{};
    mutable results_t proc_results_{};
    enum class option_state : char {
        parsing = 0,
        validated = 2,
        reduced = 4,
        callback_run = 6,
    };
    option_state current_option_state_{option_state::parsing};
    bool allow_extra_args_{false};
    bool flag_like_{false};
    bool run_callback_for_default_{false};
    bool inject_separator_{false};
    bool trigger_on_result_{false};
    bool force_callback_{false};

    Option(std::string option_name, std::string option_description, callback_t callback, App* parent,
           bool allow_non_standard = false)
        : description_(std::move(option_description)), parent_(parent), callback_(std::move(callback)) {
        std::tie(snames_, lnames_, pname_) = detail::get_names(detail::split_names(option_name), allow_non_standard);
    }

   public:
    Option(const Option&) = delete;
    Option& operator=(const Option&) = delete;

    CLI11_NODISCARD std::size_t count() const {
        return results_.size();
    }

    CLI11_NODISCARD bool empty() const {
        return results_.empty();
    }

    explicit operator bool() const {
        return !empty() || force_callback_;
    }

    void clear() {
        results_.clear();
        current_option_state_ = option_state::parsing;
    }

    Option* expected(int value);

    Option* expected(int value_min, int value_max);

    Option* allow_extra_args(bool value = true) {
        allow_extra_args_ = value;
        return this;
    }
    CLI11_NODISCARD bool get_allow_extra_args() const {
        return allow_extra_args_;
    }
    Option* trigger_on_parse(bool value = true) {
        trigger_on_result_ = value;
        return this;
    }
    CLI11_NODISCARD bool get_trigger_on_parse() const {
        return trigger_on_result_;
    }

    Option* force_callback(bool value = true) {
        force_callback_ = value;
        return this;
    }
    CLI11_NODISCARD bool get_force_callback() const {
        return force_callback_;
    }

    Option* run_callback_for_default(bool value = true) {
        run_callback_for_default_ = value;
        return this;
    }
    CLI11_NODISCARD bool get_run_callback_for_default() const {
        return run_callback_for_default_;
    }

    Option* callback_priority(CallbackPriority value = CallbackPriority::Normal) {
        callback_priority_ = value;
        return this;
    }

    Option* check(Validator_p validator);

    Option* check(Validator validator, const std::string& validator_name = "");

    Option* check(std::function<std::string(const std::string&)> validator_func, std::string validator_description = "",
                  std::string validator_name = "");

    Option* transform(Validator_p validator);

    Option* transform(Validator validator, const std::string& transform_name = "");

    Option* transform(const std::function<std::string(std::string)>& transform_func,
                      std::string transform_description = "", std::string transform_name = "");

    Option* each(const std::function<void(std::string)>& func);

    Validator* get_validator(const std::string& validator_name = "");

    Validator* get_validator(int index);

    Option* needs(Option* opt) {
        if (opt != this) {
            needs_.insert(opt);
        }
        return this;
    }

    template <typename T = App>
    Option* needs(std::string opt_name) {
        auto opt = static_cast<T*>(parent_)->get_option_no_throw(opt_name);
        if (opt == nullptr) {
            throw IncorrectConstruction::MissingOption(opt_name);
        }
        return needs(opt);
    }

    template <typename A, typename B, typename... ARG>
    Option* needs(A opt, B opt1, ARG... args) {
        needs(opt);
        return needs(opt1, args...);  // NOLINT(readability-suspicious-call-argument)
    }

    bool remove_needs(Option* opt);

    Option* excludes(Option* opt);

    template <typename T = App>
    Option* excludes(std::string opt_name) {
        auto opt = static_cast<T*>(parent_)->get_option_no_throw(opt_name);
        if (opt == nullptr) {
            throw IncorrectConstruction::MissingOption(opt_name);
        }
        return excludes(opt);
    }

    template <typename A, typename B, typename... ARG>
    Option* excludes(A opt, B opt1, ARG... args) {
        excludes(opt);
        return excludes(opt1, args...);
    }

    bool remove_excludes(Option* opt);

    Option* envname(std::string name) {
        envname_ = std::move(name);
        return this;
    }

    template <typename T = App>
    Option* ignore_case(bool value = true);

    template <typename T = App>
    Option* ignore_underscore(bool value = true);

    Option* multi_option_policy(MultiOptionPolicy value = MultiOptionPolicy::Throw);

    Option* disable_flag_override(bool value = true) {
        disable_flag_override_ = value;
        return this;
    }

    CLI11_NODISCARD int get_type_size() const {
        return type_size_min_;
    }

    CLI11_NODISCARD int get_type_size_min() const {
        return type_size_min_;
    }
    CLI11_NODISCARD int get_type_size_max() const {
        return type_size_max_;
    }

    CLI11_NODISCARD bool get_inject_separator() const {
        return inject_separator_;
    }

    CLI11_NODISCARD std::string get_envname() const {
        return envname_;
    }

    CLI11_NODISCARD std::set<Option*> get_needs() const {
        return needs_;
    }

    CLI11_NODISCARD std::set<Option*> get_excludes() const {
        return excludes_;
    }

    CLI11_NODISCARD std::string get_default_str() const {
        return default_str_;
    }

    CLI11_NODISCARD callback_t get_callback() const {
        return callback_;
    }

    CLI11_NODISCARD const std::vector<std::string>& get_lnames() const {
        return lnames_;
    }

    CLI11_NODISCARD const std::vector<std::string>& get_snames() const {
        return snames_;
    }

    CLI11_NODISCARD const std::vector<std::string>& get_fnames() const {
        return fnames_;
    }
    CLI11_NODISCARD const std::string& get_single_name() const {
        if (!lnames_.empty()) {
            return lnames_[0];
        }
        if (!snames_.empty()) {
            return snames_[0];
        }
        if (!pname_.empty()) {
            return pname_;
        }
        return envname_;
    }
    CLI11_NODISCARD int get_expected() const {
        return expected_min_;
    }

    CLI11_NODISCARD int get_expected_min() const {
        return expected_min_;
    }
    CLI11_NODISCARD int get_expected_max() const {
        return expected_max_;
    }

    CLI11_NODISCARD int get_items_expected_min() const {
        return type_size_min_ * expected_min_;
    }

    CLI11_NODISCARD int get_items_expected_max() const {
        int t = type_size_max_;
        return detail::checked_multiply(t, expected_max_) ? t : detail::expected_max_vector_size;
    }
    CLI11_NODISCARD int get_items_expected() const {
        return get_items_expected_min();
    }

    CLI11_NODISCARD bool get_positional() const {
        return !pname_.empty();
    }

    CLI11_NODISCARD bool nonpositional() const {
        return (!lnames_.empty() || !snames_.empty());
    }

    CLI11_NODISCARD bool has_description() const {
        return !description_.empty();
    }

    CLI11_NODISCARD const std::string& get_description() const {
        return description_;
    }

    Option* description(std::string option_description) {
        description_ = std::move(option_description);
        return this;
    }

    Option* option_text(std::string text) {
        option_text_ = std::move(text);
        return this;
    }

    CLI11_NODISCARD const std::string& get_option_text() const {
        return option_text_;
    }

    CLI11_NODISCARD std::string get_name(bool positional = false, bool all_options = false,
                                         bool disable_default_flag_values = false) const;

    void run_callback();

    CLI11_NODISCARD const std::string& matching_name(const Option& other) const;

    bool operator==(const Option& other) const {
        return !matching_name(other).empty();
    }

    CLI11_NODISCARD bool check_name(const std::string& name) const;

    CLI11_NODISCARD bool check_sname(std::string name) const {
        return (detail::find_member(std::move(name), snames_, ignore_case_) >= 0);
    }

    CLI11_NODISCARD bool check_lname(std::string name) const {
        return (detail::find_member(std::move(name), lnames_, ignore_case_, ignore_underscore_) >= 0);
    }

    CLI11_NODISCARD bool check_fname(std::string name) const {
        if (fnames_.empty()) {
            return false;
        }
        return (detail::find_member(std::move(name), fnames_, ignore_case_, ignore_underscore_) >= 0);
    }

    CLI11_NODISCARD std::string get_flag_value(const std::string& name, std::string input_value) const;

    Option* add_result(std::string s);

    Option* add_result(std::string s, int& results_added);

    Option* add_result(std::vector<std::string> s);

    CLI11_NODISCARD const results_t& results() const {
        return results_;
    }

    CLI11_NODISCARD results_t reduced_results() const;

    template <typename T>
    void results(T& output) const {
        bool retval = false;
        if (current_option_state_ >= option_state::reduced || (results_.size() == 1 && validators_.empty())) {
            const results_t& res = (proc_results_.empty()) ? results_ : proc_results_;
            if (!res.empty()) {
                retval = detail::lexical_conversion<T, T>(res, output);
            } else {
                results_t res2;
                res2.emplace_back();
                proc_results_ = std::move(res2);
                retval = detail::lexical_conversion<T, T>(proc_results_, output);
            }

        } else {
            results_t res;
            if (results_.empty()) {
                if (!default_str_.empty()) {
                    _add_result(std::string(default_str_), res);
                    _validate_results(res);
                    results_t extra;
                    _reduce_results(extra, res);
                    if (!extra.empty()) {
                        res = std::move(extra);
                    }
                } else {
                    res.emplace_back();
                }
            } else {
                res = reduced_results();
            }
            proc_results_ = std::move(res);
            retval = detail::lexical_conversion<T, T>(proc_results_, output);
        }
        if (!retval) {
            throw ConversionError(get_name(), results_);
        }
    }

    template <typename T>
    CLI11_NODISCARD T as() const {
        T output;
        results(output);
        return output;
    }

    CLI11_NODISCARD bool get_callback_run() const {
        return (current_option_state_ == option_state::callback_run);
    }

    Option* type_name_fn(std::function<std::string()> typefun) {
        type_name_ = std::move(typefun);
        return this;
    }

    Option* type_name(std::string typeval) {
        type_name_fn([typeval]() { return typeval; });
        return this;
    }

    Option* type_size(int option_type_size);

    Option* type_size(int option_type_size_min, int option_type_size_max);

    void inject_separator(bool value = true) {
        inject_separator_ = value;
    }

    Option* default_function(const std::function<std::string()>& func) {
        default_function_ = func;
        return this;
    }

    Option* capture_default_str() {
        if (default_function_) {
            default_str_ = default_function_();
        }
        return this;
    }

    Option* default_str(std::string val) {
        default_str_ = std::move(val);
        return this;
    }

    template <typename X>
    Option* default_val(const X& val) {
        std::string val_str = detail::value_string(val);
        auto old_option_state = current_option_state_;
        results_t old_results{std::move(results_)};
        results_.clear();
        try {
            add_result(val_str);
            if (run_callback_for_default_ && !trigger_on_result_) {
                run_callback();
                current_option_state_ = option_state::parsing;
            } else {
                _validate_results(results_);
                current_option_state_ = old_option_state;
            }
        } catch (const ConversionError& err) {
            results_ = std::move(old_results);
            current_option_state_ = old_option_state;

            throw ConversionError(
                get_name(), std::string("given default value(\"") + val_str + "\") produces an error : " + err.what());
        } catch (const CLI::Error&) {
            results_ = std::move(old_results);
            current_option_state_ = old_option_state;
            throw;
        }
        results_ = std::move(old_results);
        default_str_ = std::move(val_str);
        return this;
    }

    CLI11_NODISCARD std::string get_type_name() const;

   private:
    void _validate_results(results_t& res) const;

    void _reduce_results(results_t& out, const results_t& original) const;

    std::string _validate(std::string& result, int index) const;

    int _add_result(std::string&& result, std::vector<std::string>& res) const;
};

template <typename CRTP>
template <typename T>
void OptionBase<CRTP>::copy_to(T* other) const {
    other->group(group_);
    other->required(required_);
    other->ignore_case(ignore_case_);
    other->ignore_underscore(ignore_underscore_);
    other->configurable(configurable_);
    other->disable_flag_override(disable_flag_override_);
    other->delimiter(delimiter_);
    other->always_capture_default(always_capture_default_);
    other->multi_option_policy(multi_option_policy_);
    other->callback_priority(callback_priority_);
}

CLI11_INLINE Option* Option::expected(int value) {
    if (value < 0) {
        expected_min_ = -value;
        if (expected_max_ < expected_min_) {
            expected_max_ = expected_min_;
        }
        allow_extra_args_ = true;
        flag_like_ = false;
    } else if (value == detail::expected_max_vector_size) {
        expected_min_ = 1;
        expected_max_ = detail::expected_max_vector_size;
        allow_extra_args_ = true;
        flag_like_ = false;
    } else {
        expected_min_ = value;
        expected_max_ = value;
        flag_like_ = (expected_min_ == 0);
    }
    return this;
}

CLI11_INLINE Option* Option::expected(int value_min, int value_max) {
    if (value_min < 0) {
        value_min = -value_min;
    }

    if (value_max < 0) {
        value_max = detail::expected_max_vector_size;
    }
    if (value_max < value_min) {
        expected_min_ = value_max;
        expected_max_ = value_min;
    } else {
        expected_max_ = value_max;
        expected_min_ = value_min;
    }

    return this;
}

CLI11_INLINE Option* Option::check(Validator_p validator) {
    validator->non_modifying();
    validators_.push_back(std::move(validator));

    return this;
}

CLI11_INLINE Option* Option::check(Validator validator, const std::string& validator_name) {
    validator.non_modifying();
    auto vp = std::make_shared<Validator>(std::move(validator));
    if (!validator_name.empty()) {
        vp->name(validator_name);
    }
    validators_.push_back(std::move(vp));

    return this;
}

CLI11_INLINE Option* Option::check(std::function<std::string(const std::string&)> validator_func,
                                   std::string validator_description, std::string validator_name) {
    auto vp = std::make_shared<Validator>(std::move(validator_func), std::move(validator_description),
                                          std::move(validator_name));
    vp->non_modifying();
    validators_.push_back(std::move(vp));
    return this;
}

CLI11_INLINE Option* Option::transform(Validator_p validator) {
    validators_.insert(validators_.begin(), std::move(validator));

    return this;
}

CLI11_INLINE Option* Option::transform(Validator validator, const std::string& transform_name) {
    auto vp = std::make_shared<Validator>(std::move(validator));
    if (!transform_name.empty()) {
        vp->name(transform_name);
    }
    validators_.insert(validators_.begin(), std::move(vp));
    return this;
}

CLI11_INLINE Option* Option::transform(const std::function<std::string(std::string)>& transform_func,
                                       std::string transform_description, std::string transform_name) {
    auto vp = std::make_shared<Validator>(
        [transform_func](std::string& val) {
            val = transform_func(val);
            return std::string{};
        },
        std::move(transform_description), std::move(transform_name));
    validators_.insert(validators_.begin(), std::move(vp));

    return this;
}

CLI11_INLINE Option* Option::each(const std::function<void(std::string)>& func) {
    auto vp = std::make_shared<Validator>(
        [func](std::string& inout) {
            func(inout);
            return std::string{};
        },
        std::string{});
    validators_.push_back(std::move(vp));
    return this;
}

CLI11_INLINE Validator* Option::get_validator(const std::string& validator_name) {
    for (auto& validator : validators_) {
        if (validator_name == validator->get_name()) {
            return validator.get();
        }
    }
    if ((validator_name.empty()) && (!validators_.empty())) {
        return validators_.front().get();
    }
    throw OptionNotFound(std::string{"Validator "} + validator_name + " Not Found");
}

CLI11_INLINE Validator* Option::get_validator(int index) {
    if (index >= 0 && index < static_cast<int>(validators_.size())) {
        return validators_[static_cast<decltype(validators_)::size_type>(index)].get();
    }
    throw OptionNotFound("Validator index is not valid");
}

CLI11_INLINE bool Option::remove_needs(Option* opt) {
    auto iterator = std::find(std::begin(needs_), std::end(needs_), opt);

    if (iterator == std::end(needs_)) {
        return false;
    }
    needs_.erase(iterator);
    return true;
}

CLI11_INLINE Option* Option::excludes(Option* opt) {
    if (opt == this) {
        throw(IncorrectConstruction("and option cannot exclude itself"));
    }
    excludes_.insert(opt);

    opt->excludes_.insert(this);

    return this;
}

CLI11_INLINE bool Option::remove_excludes(Option* opt) {
    auto iterator = std::find(std::begin(excludes_), std::end(excludes_), opt);

    if (iterator == std::end(excludes_)) {
        return false;
    }
    excludes_.erase(iterator);
    return true;
}

template <typename T>
Option* Option::ignore_case(bool value) {
    if (!ignore_case_ && value) {
        ignore_case_ = value;
        auto* parent = static_cast<T*>(parent_);
        for (const Option_p& opt : parent->options_) {
            if (opt.get() == this) {
                continue;
            }
            const auto& omatch = opt->matching_name(*this);
            if (!omatch.empty()) {
                ignore_case_ = false;
                throw OptionAlreadyAdded("adding ignore case caused a name conflict with " + omatch);
            }
        }
    } else {
        ignore_case_ = value;
    }
    return this;
}

template <typename T>
Option* Option::ignore_underscore(bool value) {
    if (!ignore_underscore_ && value) {
        ignore_underscore_ = value;
        auto* parent = static_cast<T*>(parent_);
        for (const Option_p& opt : parent->options_) {
            if (opt.get() == this) {
                continue;
            }
            const auto& omatch = opt->matching_name(*this);
            if (!omatch.empty()) {
                ignore_underscore_ = false;
                throw OptionAlreadyAdded("adding ignore underscore caused a name conflict with " + omatch);
            }
        }
    } else {
        ignore_underscore_ = value;
    }
    return this;
}

CLI11_INLINE Option* Option::multi_option_policy(MultiOptionPolicy value) {
    if (value != multi_option_policy_) {
        if (multi_option_policy_ == MultiOptionPolicy::Throw && expected_max_ == detail::expected_max_vector_size &&
            expected_min_ > 1) {
            expected_max_ = expected_min_;
        }
        multi_option_policy_ = value;
        current_option_state_ = option_state::parsing;
    }
    return this;
}

CLI11_NODISCARD CLI11_INLINE std::string Option::get_name(bool positional, bool all_options,
                                                          bool disable_default_flag_values) const {
    if (get_group().empty())
        return {};

    if (all_options) {
        std::vector<std::string> name_list;

        if ((positional && (!pname_.empty())) || (snames_.empty() && lnames_.empty())) {
            name_list.push_back(pname_);
        }
        if ((get_items_expected() == 0) && (!fnames_.empty())) {
            for (const std::string& sname : snames_) {
                name_list.push_back("-" + sname);
                if (!disable_default_flag_values && check_fname(sname)) {
                    name_list.back() += "{" + get_flag_value(sname, "") + "}";
                }
            }

            for (const std::string& lname : lnames_) {
                name_list.push_back("--" + lname);
                if (!disable_default_flag_values && check_fname(lname)) {
                    name_list.back() += "{" + get_flag_value(lname, "") + "}";
                }
            }
        } else {
            for (const std::string& sname : snames_)
                name_list.push_back("-" + sname);

            for (const std::string& lname : lnames_)
                name_list.push_back("--" + lname);
        }

        return detail::join(name_list);
    }

    if (positional)
        return pname_;

    if (!lnames_.empty())
        return std::string(2, '-') + lnames_[0];

    if (!snames_.empty())
        return std::string(1, '-') + snames_[0];

    return pname_;
}

CLI11_INLINE void Option::run_callback() {
    bool used_default_str = false;
    if (force_callback_ && results_.empty()) {
        used_default_str = true;
        add_result(default_str_);
    }
    if (current_option_state_ == option_state::parsing) {
        _validate_results(results_);
        current_option_state_ = option_state::validated;
    }

    if (current_option_state_ < option_state::reduced) {
        _reduce_results(proc_results_, results_);
    }

    current_option_state_ = option_state::callback_run;
    if (callback_) {
        const results_t& send_results = proc_results_.empty() ? results_ : proc_results_;
        if (send_results.empty()) {
            return;
        }
        bool local_result = callback_(send_results);
        if (used_default_str) {
            results_.clear();
            proc_results_.clear();
        }
        if (!local_result)
            throw ConversionError(get_name(), results_);
    }
}

CLI11_NODISCARD CLI11_INLINE const std::string& Option::matching_name(const Option& other) const {
    static const std::string estring;
    bool bothConfigurable = configurable_ && other.configurable_;
    for (const std::string& sname : snames_) {
        if (other.check_sname(sname))
            return sname;
        if (bothConfigurable && other.check_lname(sname))
            return sname;
    }
    for (const std::string& lname : lnames_) {
        if (other.check_lname(lname))
            return lname;
        if (lname.size() == 1 && bothConfigurable) {
            if (other.check_sname(lname)) {
                return lname;
            }
        }
    }
    if (bothConfigurable && snames_.empty() && lnames_.empty() && !pname_.empty()) {
        if (other.check_sname(pname_) || other.check_lname(pname_) || pname_ == other.pname_)
            return pname_;
    }
    if (bothConfigurable && other.snames_.empty() && other.fnames_.empty() && !other.pname_.empty()) {
        if (check_sname(other.pname_) || check_lname(other.pname_) || (pname_ == other.pname_))
            return other.pname_;
    }
    if (ignore_case_ || ignore_underscore_) {
        for (const std::string& sname : other.snames_)
            if (check_sname(sname))
                return sname;
        for (const std::string& lname : other.lnames_)
            if (check_lname(lname))
                return lname;
    }
    return estring;
}

CLI11_NODISCARD CLI11_INLINE bool Option::check_name(const std::string& name) const {
    if (name.length() > 2 && name[0] == '-' && name[1] == '-')
        return check_lname(name.substr(2));
    if (name.length() > 1 && name.front() == '-')
        return check_sname(name.substr(1));
    if (!pname_.empty()) {
        std::string local_pname = pname_;
        std::string local_name = name;
        if (ignore_underscore_) {
            local_pname = detail::remove_underscore(local_pname);
            local_name = detail::remove_underscore(local_name);
        }
        if (ignore_case_) {
            local_pname = detail::to_lower(local_pname);
            local_name = detail::to_lower(local_name);
        }
        if (local_name == local_pname) {
            return true;
        }
    }

    if (!envname_.empty()) {
        return (name == envname_);
    }
    return false;
}

CLI11_NODISCARD CLI11_INLINE std::string Option::get_flag_value(const std::string& name,
                                                                std::string input_value) const {
    static const std::string trueString{"true"};
    static const std::string falseString{"false"};
    static const std::string emptyString{"{}"};
    if (disable_flag_override_) {
        if (!((input_value.empty()) || (input_value == emptyString))) {
            auto default_ind = detail::find_member(name, fnames_, ignore_case_, ignore_underscore_);
            if (default_ind >= 0) {
                if (default_flag_values_[static_cast<std::size_t>(default_ind)].second != input_value) {
                    if (input_value == default_str_ && force_callback_) {
                        return input_value;
                    }
                    throw(ArgumentMismatch::FlagOverride(name));
                }
            } else {
                if (input_value != trueString) {
                    throw(ArgumentMismatch::FlagOverride(name));
                }
            }
        }
    }
    auto ind = detail::find_member(name, fnames_, ignore_case_, ignore_underscore_);
    if ((input_value.empty()) || (input_value == emptyString)) {
        if (flag_like_) {
            return (ind < 0) ? trueString : default_flag_values_[static_cast<std::size_t>(ind)].second;
        }
        return (ind < 0) ? default_str_ : default_flag_values_[static_cast<std::size_t>(ind)].second;
    }
    if (ind < 0) {
        return input_value;
    }
    if (default_flag_values_[static_cast<std::size_t>(ind)].second == falseString) {
        errno = 0;
        auto val = detail::to_flag_value(input_value);
        if (errno != 0) {
            errno = 0;
            return input_value;
        }
        return (val == 1) ? falseString : (val == (-1) ? trueString : std::to_string(-val));
    }
    return input_value;
}

CLI11_INLINE Option* Option::add_result(std::string s) {
    _add_result(std::move(s), results_);
    current_option_state_ = option_state::parsing;
    return this;
}

CLI11_INLINE Option* Option::add_result(std::string s, int& results_added) {
    results_added = _add_result(std::move(s), results_);
    current_option_state_ = option_state::parsing;
    return this;
}

CLI11_INLINE Option* Option::add_result(std::vector<std::string> s) {
    current_option_state_ = option_state::parsing;
    for (auto& str : s) {
        _add_result(std::move(str), results_);
    }
    return this;
}

CLI11_NODISCARD CLI11_INLINE results_t Option::reduced_results() const {
    results_t res = proc_results_.empty() ? results_ : proc_results_;
    if (current_option_state_ < option_state::reduced) {
        if (current_option_state_ == option_state::parsing) {
            res = results_;
            _validate_results(res);
        }
        if (!res.empty()) {
            results_t extra;
            _reduce_results(extra, res);
            if (!extra.empty()) {
                res = std::move(extra);
            }
        }
    }
    return res;
}

CLI11_INLINE Option* Option::type_size(int option_type_size) {
    if (option_type_size < 0) {
        type_size_max_ = -option_type_size;
        type_size_min_ = -option_type_size;
        expected_max_ = detail::expected_max_vector_size;
    } else {
        type_size_max_ = option_type_size;
        if (type_size_max_ < detail::expected_max_vector_size) {
            type_size_min_ = option_type_size;
        } else {
            inject_separator_ = true;
        }
        if (type_size_max_ == 0)
            required_ = false;
    }
    return this;
}

CLI11_INLINE Option* Option::type_size(int option_type_size_min, int option_type_size_max) {
    if (option_type_size_min < 0 || option_type_size_max < 0) {
        expected_max_ = detail::expected_max_vector_size;
        option_type_size_min = (std::abs)(option_type_size_min);
        option_type_size_max = (std::abs)(option_type_size_max);
    }

    if (option_type_size_min > option_type_size_max) {
        type_size_max_ = option_type_size_min;
        type_size_min_ = option_type_size_max;
    } else {
        type_size_min_ = option_type_size_min;
        type_size_max_ = option_type_size_max;
    }
    if (type_size_max_ == 0) {
        required_ = false;
    }
    if (type_size_max_ >= detail::expected_max_vector_size) {
        inject_separator_ = true;
    }
    return this;
}

CLI11_NODISCARD CLI11_INLINE std::string Option::get_type_name() const {
    std::string full_type_name = type_name_();
    if (!validators_.empty()) {
        for (const auto& validator : validators_) {
            std::string vtype = validator->get_description();
            if (!vtype.empty()) {
                full_type_name += ":" + vtype;
            }
        }
    }
    return full_type_name;
}

CLI11_INLINE void Option::_validate_results(results_t& res) const {
    if (!validators_.empty()) {
        if (type_size_max_ > 1) {
            int index = 0;
            if (get_items_expected_max() < static_cast<int>(res.size()) &&
                (multi_option_policy_ == CLI::MultiOptionPolicy::TakeLast ||
                 multi_option_policy_ == CLI::MultiOptionPolicy::Reverse)) {
                index = get_items_expected_max() - static_cast<int>(res.size());
            }

            for (std::string& result : res) {
                if (detail::is_separator(result) && type_size_max_ != type_size_min_ && index >= 0) {
                    index = 0;
                    continue;
                }
                auto err_msg = _validate(result, (index >= 0) ? (index % type_size_max_) : index);
                if (!err_msg.empty())
                    throw ValidationError(get_name(), err_msg);
                ++index;
            }
        } else {
            int index = 0;
            if (expected_max_ < static_cast<int>(res.size()) &&
                (multi_option_policy_ == CLI::MultiOptionPolicy::TakeLast ||
                 multi_option_policy_ == CLI::MultiOptionPolicy::Reverse)) {
                index = expected_max_ - static_cast<int>(res.size());
            }
            for (std::string& result : res) {
                auto err_msg = _validate(result, index);
                ++index;
                if (!err_msg.empty())
                    throw ValidationError(get_name(), err_msg);
            }
        }
    }
}

CLI11_INLINE void Option::_reduce_results(results_t& out, const results_t& original) const {
    out.clear();
    switch (multi_option_policy_) {
        case MultiOptionPolicy::TakeAll:
            break;
        case MultiOptionPolicy::TakeLast: {
            std::size_t trim_size = std::min<std::size_t>(
                static_cast<std::size_t>(std::max<int>(get_items_expected_max(), 1)), original.size());
            if (original.size() != trim_size) {
                out.assign(original.end() - static_cast<results_t::difference_type>(trim_size), original.end());
            }
        } break;
        case MultiOptionPolicy::Reverse: {
            std::size_t trim_size = std::min<std::size_t>(
                static_cast<std::size_t>(std::max<int>(get_items_expected_max(), 1)), original.size());
            if (original.size() != trim_size || trim_size > 1) {
                out.assign(original.end() - static_cast<results_t::difference_type>(trim_size), original.end());
            }
            std::reverse(out.begin(), out.end());
        } break;
        case MultiOptionPolicy::TakeFirst: {
            std::size_t trim_size = std::min<std::size_t>(
                static_cast<std::size_t>(std::max<int>(get_items_expected_max(), 1)), original.size());
            if (original.size() != trim_size) {
                out.assign(original.begin(), original.begin() + static_cast<results_t::difference_type>(trim_size));
            }
        } break;
        case MultiOptionPolicy::Join:
            if (results_.size() > 1) {
                out.push_back(detail::join(original, std::string(1, (delimiter_ == '\0') ? '\n' : delimiter_)));
            }
            break;
        case MultiOptionPolicy::Sum:
            out.push_back(detail::sum_string_vector(original));
            break;
        case MultiOptionPolicy::Throw:
        default: {
            auto num_min = static_cast<std::size_t>(get_items_expected_min());
            auto num_max = static_cast<std::size_t>(get_items_expected_max());
            if (num_min == 0) {
                num_min = 1;
            }
            if (num_max == 0) {
                num_max = 1;
            }
            if (original.size() < num_min) {
                throw ArgumentMismatch::AtLeast(get_name(), static_cast<int>(num_min), original.size());
            }
            if (original.size() > num_max) {
                if (original.size() == 2 && num_max == 1 && original[1] == "%%" && original[0] == "{}") {
                    out = original;
                } else {
                    throw ArgumentMismatch::AtMost(get_name(), static_cast<int>(num_max), original.size());
                }
            }
            break;
        }
    }
    if (out.empty()) {
        if (original.size() == 1 && original[0] == "{}" && get_items_expected_min() > 0) {
            out.emplace_back("{}");
            out.emplace_back("%%");
        }
    } else if (out.size() == 1 && out[0] == "{}" && get_items_expected_min() > 0) {
        out.emplace_back("%%");
    }
}

CLI11_INLINE std::string Option::_validate(std::string& result, int index) const {
    std::string err_msg;
    if (result.empty() && expected_min_ == 0) {
        return err_msg;
    }
    for (const auto& vali : validators_) {
        auto v = vali->get_application_index();
        if (v == -1 || v == index) {
            try {
                err_msg = (*vali)(result);
            } catch (const ValidationError& err) {
                err_msg = err.what();
            }
            if (!err_msg.empty())
                break;
        }
    }

    return err_msg;
}

CLI11_INLINE int Option::_add_result(std::string&& result, std::vector<std::string>& res) const {
    int result_count = 0;

    if (result.size() >= 4 && result[0] == '[' && result[1] == '[' && result.back() == ']' &&
        (*(result.end() - 2) == ']')) {
        std::string nstrs{'['};
        bool duplicated{true};
        for (std::size_t ii = 2; ii < result.size() - 2; ii += 2) {
            if (result[ii] == result[ii + 1]) {
                nstrs.push_back(result[ii]);
            } else {
                duplicated = false;
                break;
            }
        }
        if (duplicated) {
            nstrs.push_back(']');
            res.push_back(std::move(nstrs));
            ++result_count;
            return result_count;
        }
    }

    if ((allow_extra_args_ || get_expected_max() > 1 || get_type_size() > 1) && !result.empty() &&
        result.front() == '[' && result.back() == ']') {
        result.pop_back();
        result.erase(result.begin());
        bool skipSection{false};
        for (auto& var : CLI::detail::split_up(result, ',')) {
            if (!var.empty()) {
                result_count += _add_result(std::move(var), res);
            }
        }
        if (!skipSection) {
            return result_count;
        }
    }
    if (delimiter_ == '\0') {
        res.push_back(std::move(result));
        ++result_count;
    } else {
        if ((result.find_first_of(delimiter_) != std::string::npos)) {
            for (const auto& var : CLI::detail::split(result, delimiter_)) {
                if (!var.empty()) {
                    res.push_back(var);
                    ++result_count;
                }
            }
        } else {
            res.push_back(std::move(result));
            ++result_count;
        }
    }
    return result_count;
}

#ifndef CLI11_PARSE
#define CLI11_PARSE(app, ...)            \
    try {                                \
        (app).parse(__VA_ARGS__);        \
    } catch (const CLI::ParseError& e) { \
        return (app).exit(e);            \
    }
#endif

namespace detail {
enum class Classifier : std::uint8_t {
    NONE,
    POSITIONAL_MARK,
    SHORT,
    LONG,
    WINDOWS_STYLE,
    SUBCOMMAND,
    SUBCOMMAND_TERMINATOR
};
struct AppFriend;
}  

namespace FailureMessage {
CLI11_INLINE std::string simple(const App* app, const Error& e);

CLI11_INLINE std::string help(const App* app, const Error& e);
}  

enum class ExtrasMode : std::uint8_t {
    Error = 0,
    ErrorImmediately,
    Ignore,
    AssumeSingleArgument,
    AssumeMultipleArguments,
    Capture
};

enum class ConfigExtrasMode : std::uint8_t {
    Error = 0,
    Ignore,
    IgnoreAll,
    Capture
};

enum class config_extras_mode : std::uint8_t {
    error = 0,
    ignore,
    ignore_all,
    capture
};

enum class PrefixCommandMode : std::uint8_t {
    Off = 0,
    SeparatorOnly = 1,
    On = 2
};

class App;

using App_p = std::shared_ptr<App>;

namespace detail {

template <typename T, enable_if_t<!std::is_integral<T>::value || (sizeof(T) <= 1U), detail::enabler> = detail::dummy>
Option* default_flag_modifiers(Option* opt) {
    return opt->always_capture_default();
}

template <typename T, enable_if_t<std::is_integral<T>::value && (sizeof(T) > 1U), detail::enabler> = detail::dummy>
Option* default_flag_modifiers(Option* opt) {
    return opt->multi_option_policy(MultiOptionPolicy::Sum)->default_str("0")->force_callback();
}

}  

class Option_group;
class App {
    friend Option;
    friend detail::AppFriend;

   protected:
    std::string name_{};

    std::string description_{};

    ExtrasMode allow_extras_{ExtrasMode::Error};

    ConfigExtrasMode allow_config_extras_{ConfigExtrasMode::Ignore};

    PrefixCommandMode prefix_command_{PrefixCommandMode::Off};

    bool has_automatic_name_{false};

    bool required_{false};

    bool disabled_{false};

    bool pre_parse_called_{false};

    bool immediate_callback_{false};

    std::function<void(std::size_t)> pre_parse_callback_{};

    std::function<void()> parse_complete_callback_{};

    std::function<void()> final_callback_{};

    OptionDefaults option_defaults_{};

    std::vector<Option_p> options_{};

    std::string usage_{};

    std::function<std::string()> usage_callback_{};

    std::string footer_{};

    std::function<std::string()> footer_callback_{};

    Option* help_ptr_{nullptr};

    Option* help_all_ptr_{nullptr};

    Option* version_ptr_{nullptr};

    std::shared_ptr<FormatterBase> formatter_{new Formatter()};

    std::function<std::string(const App*, const Error& e)> failure_message_{FailureMessage::simple};

    using missing_t = std::vector<std::pair<detail::Classifier, std::string>>;

    missing_t missing_{};

    std::vector<Option*> parse_order_{};

    std::vector<App*> parsed_subcommands_{};

    std::set<App*> exclude_subcommands_{};

    std::set<Option*> exclude_options_{};

    std::set<App*> need_subcommands_{};

    std::set<Option*> need_options_{};

    std::vector<App_p> subcommands_{};

    bool ignore_case_{false};

    bool ignore_underscore_{false};

    bool fallthrough_{false};

    bool subcommand_fallthrough_{true};

    bool allow_windows_style_options_{
#ifdef _WIN32
        true
#else
        false
#endif
    };
    bool positionals_at_end_{false};

    enum class startup_mode : std::uint8_t {
        stable,
        enabled,
        disabled
    };
    startup_mode default_startup{startup_mode::stable};

    bool configurable_{false};

    bool validate_positionals_{false};

    bool validate_optional_arguments_{false};

    bool silent_{false};

    bool allow_non_standard_options_{false};

    bool allow_prefix_matching_{false};

    std::uint32_t parsed_{0U};

    std::size_t require_subcommand_min_{0};

    std::size_t require_subcommand_max_{0};

    std::size_t require_option_min_{0};

    std::size_t require_option_max_{0};

    App* parent_{nullptr};

    std::string group_{"SUBCOMMANDS"};

    std::vector<std::string> aliases_{};

    Option* config_ptr_{nullptr};

    std::shared_ptr<Config> config_formatter_{new ConfigTOML()};

#ifdef _WIN32
    std::vector<std::string> normalized_argv_{};

    std::vector<char*> normalized_argv_view_{};
#endif

    App(std::string app_description, std::string app_name, App* parent);

   public:
    explicit App(std::string app_description = "", std::string app_name = "")
        : App(app_description, app_name, nullptr) {
        set_help_flag("-h,--help", "Print this help message and exit");
    }

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    virtual ~App() = default;

    CLI11_NODISCARD char** ensure_utf8(char** argv);

    App* callback(std::function<void()> app_callback) {
        if (immediate_callback_) {
            parse_complete_callback_ = std::move(app_callback);
        } else {
            final_callback_ = std::move(app_callback);
        }
        return this;
    }

    App* final_callback(std::function<void()> app_callback) {
        final_callback_ = std::move(app_callback);
        return this;
    }

    App* parse_complete_callback(std::function<void()> pc_callback) {
        parse_complete_callback_ = std::move(pc_callback);
        return this;
    }

    App* preparse_callback(std::function<void(std::size_t)> pp_callback) {
        pre_parse_callback_ = std::move(pp_callback);
        return this;
    }

    App* name(std::string app_name = "");

    App* alias(std::string app_name);

    App* allow_extras(bool allow = true) {
        allow_extras_ = allow ? ExtrasMode::Capture : ExtrasMode::Error;
        return this;
    }

    App* allow_extras(ExtrasMode allow) {
        allow_extras_ = allow;
        return this;
    }

    App* required(bool require = true) {
        required_ = require;
        return this;
    }

    App* disabled(bool disable = true) {
        disabled_ = disable;
        return this;
    }

    App* silent(bool silence = true) {
        silent_ = silence;
        return this;
    }

    App* allow_non_standard_option_names(bool allowed = true) {
        allow_non_standard_options_ = allowed;
        return this;
    }

    App* allow_subcommand_prefix_matching(bool allowed = true) {
        allow_prefix_matching_ = allowed;
        return this;
    }
    App* disabled_by_default(bool disable = true) {
        if (disable) {
            default_startup = startup_mode::disabled;
        } else {
            default_startup = (default_startup == startup_mode::enabled) ? startup_mode::enabled : startup_mode::stable;
        }
        return this;
    }

    App* enabled_by_default(bool enable = true) {
        if (enable) {
            default_startup = startup_mode::enabled;
        } else {
            default_startup =
                (default_startup == startup_mode::disabled) ? startup_mode::disabled : startup_mode::stable;
        }
        return this;
    }

    App* immediate_callback(bool immediate = true);

    App* validate_positionals(bool validate = true) {
        validate_positionals_ = validate;
        return this;
    }

    App* validate_optional_arguments(bool validate = true) {
        validate_optional_arguments_ = validate;
        return this;
    }

    App* allow_config_extras(bool allow = true) {
        if (allow) {
            allow_config_extras_ = ConfigExtrasMode::Capture;
            allow_extras_ = ExtrasMode::Capture;
        } else {
            allow_config_extras_ = ConfigExtrasMode::Error;
        }
        return this;
    }

    App* allow_config_extras(config_extras_mode mode) {
        allow_config_extras_ = static_cast<ConfigExtrasMode>(mode);
        return this;
    }

    App* allow_config_extras(ConfigExtrasMode mode) {
        allow_config_extras_ = mode;
        return this;
    }

    App* prefix_command(bool is_prefix = true) {
        prefix_command_ = is_prefix ? PrefixCommandMode::On : PrefixCommandMode::Off;
        return this;
    }

    App* prefix_command(PrefixCommandMode mode) {
        prefix_command_ = mode;
        return this;
    }

    App* ignore_case(bool value = true);

    App* allow_windows_style_options(bool value = true) {
        allow_windows_style_options_ = value;
        return this;
    }

    App* positionals_at_end(bool value = true) {
        positionals_at_end_ = value;
        return this;
    }

    App* configurable(bool value = true) {
        configurable_ = value;
        return this;
    }

    App* ignore_underscore(bool value = true);

    App* formatter(std::shared_ptr<FormatterBase> fmt) {
        formatter_ = fmt;
        return this;
    }

    App* formatter_fn(std::function<std::string(const App*, std::string, AppFormatMode)> fmt) {
        formatter_ = std::make_shared<FormatterLambda>(fmt);
        return this;
    }

    App* config_formatter(std::shared_ptr<Config> fmt) {
        config_formatter_ = fmt;
        return this;
    }

    CLI11_NODISCARD bool parsed() const {
        return parsed_ > 0;
    }

    OptionDefaults* option_defaults() {
        return &option_defaults_;
    }

    Option* add_option(std::string option_name, callback_t option_callback, std::string option_description = "",
                       bool defaulted = false, std::function<std::string()> func = {});

    template <typename AssignTo, typename ConvertTo = AssignTo,
              enable_if_t<!std::is_const<ConvertTo>::value, detail::enabler> = detail::dummy>
    Option* add_option(std::string option_name, AssignTo& variable, std::string option_description = "") {
        auto fun = [&variable](const CLI::results_t& res) {
            return detail::lexical_conversion<AssignTo, ConvertTo>(res, variable);
        };

        Option* opt = add_option(option_name, fun, option_description, false, [&variable]() {
            return CLI::detail::checked_to_string<AssignTo, ConvertTo>(variable);
        });
        opt->type_name(detail::type_name<ConvertTo>());
        auto Tcount = detail::type_count<AssignTo>::value;
        auto XCcount = detail::type_count<ConvertTo>::value;
        opt->type_size(detail::type_count_min<ConvertTo>::value, (std::max)(Tcount, XCcount));
        opt->expected(detail::expected_count<ConvertTo>::value);
        opt->run_callback_for_default();
        return opt;
    }

    template <typename AssignTo, enable_if_t<!std::is_const<AssignTo>::value, detail::enabler> = detail::dummy>
    Option* add_option_no_stream(std::string option_name, AssignTo& variable, std::string option_description = "") {
        auto fun = [&variable](const CLI::results_t& res) {
            return detail::lexical_conversion<AssignTo, AssignTo>(res, variable);
        };

        Option* opt = add_option(option_name, fun, option_description, false, []() { return std::string{}; });
        opt->type_name(detail::type_name<AssignTo>());
        opt->type_size(detail::type_count_min<AssignTo>::value, detail::type_count<AssignTo>::value);
        opt->expected(detail::expected_count<AssignTo>::value);
        opt->run_callback_for_default();
        return opt;
    }

    template <typename ArgType>
    Option* add_option_function(std::string option_name, const std::function<void(const ArgType&)>& func,
                                std::string option_description = "") {
        auto fun = [func](const CLI::results_t& res) {
            ArgType variable;
            bool result = detail::lexical_conversion<ArgType, ArgType>(res, variable);
            if (result) {
                func(variable);
            }
            return result;
        };

        Option* opt = add_option(option_name, std::move(fun), option_description, false);
        opt->type_name(detail::type_name<ArgType>());
        opt->type_size(detail::type_count_min<ArgType>::value, detail::type_count<ArgType>::value);
        opt->expected(detail::expected_count<ArgType>::value);
        return opt;
    }

    Option* add_option(std::string option_name) {
        return add_option(option_name, CLI::callback_t{}, std::string{}, false);
    }

    template <typename T, enable_if_t<std::is_const<T>::value && std::is_constructible<std::string, T>::value,
                                      detail::enabler> = detail::dummy>
    Option* add_option(std::string option_name, T& option_description) {
        return add_option(option_name, CLI::callback_t(), option_description, false);
    }

    Option* set_help_flag(std::string flag_name = "", const std::string& help_description = "");

    Option* set_help_all_flag(std::string help_name = "", const std::string& help_description = "");

    Option* set_version_flag(std::string flag_name = "", const std::string& versionString = "",
                             const std::string& version_help = "Display program version information and exit");

    Option* set_version_flag(std::string flag_name, std::function<std::string()> vfunc,
                             const std::string& version_help = "Display program version information and exit");

   private:
    Option* _add_flag_internal(std::string flag_name, CLI::callback_t fun, std::string flag_description);

   public:
    Option* add_flag(std::string flag_name) {
        return _add_flag_internal(flag_name, CLI::callback_t(), std::string{});
    }

    template <typename T,
              enable_if_t<(std::is_const<typename std::remove_reference<T>::type>::value ||
                           std::is_rvalue_reference<T&&>::value) &&
                              std::is_constructible<std::string, typename std::remove_reference<T>::type>::value,
                          detail::enabler> = detail::dummy>
    Option* add_flag(std::string flag_name, T&& flag_description) {
        return _add_flag_internal(flag_name, CLI::callback_t(), std::forward<T>(flag_description));
    }

    template <typename T, enable_if_t<!detail::is_mutable_container<T>::value && !std::is_const<T>::value &&
                                          !std::is_constructible<std::function<void(int)>, T>::value,
                                      detail::enabler> = detail::dummy>
    Option* add_flag(std::string flag_name, T& flag_result, std::string flag_description = "") {
        CLI::callback_t fun = [&flag_result](const CLI::results_t& res) {
            using CLI::detail::lexical_cast;
            return lexical_cast(res[0], flag_result);
        };
        auto* opt = _add_flag_internal(flag_name, std::move(fun), std::move(flag_description));
        return detail::default_flag_modifiers<T>(opt);
    }

    template <typename T, enable_if_t<!std::is_assignable<std::function<void(std::int64_t)>&, T>::value,
                                      detail::enabler> = detail::dummy>
    Option* add_flag(std::string flag_name, std::vector<T>& flag_results, std::string flag_description = "") {
        CLI::callback_t fun = [&flag_results](const CLI::results_t& res) {
            bool retval = true;
            for (const auto& elem : res) {
                using CLI::detail::lexical_cast;
                flag_results.emplace_back();
                retval &= lexical_cast(elem, flag_results.back());
            }
            return retval;
        };
        return _add_flag_internal(flag_name, std::move(fun), std::move(flag_description))
            ->multi_option_policy(MultiOptionPolicy::TakeAll)
            ->run_callback_for_default();
    }

    Option* add_flag_callback(std::string flag_name, std::function<void(void)> function,
                              std::string flag_description = "");

    Option* add_flag_function(std::string flag_name, std::function<void(std::int64_t)> function,
                              std::string flag_description = "");

#ifdef CLI11_CPP14
    Option* add_flag(std::string flag_name, std::function<void(std::int64_t)> function,
                     std::string flag_description = "") {
        return add_flag_function(std::move(flag_name), std::move(function), std::move(flag_description));
    }
#endif

    Option* set_config(std::string option_name = "", std::string default_filename = "",
                       const std::string& help_message = "Read an ini file", bool config_required = false);

    bool remove_option(Option* opt);

    template <typename T = Option_group>
    T* add_option_group(std::string group_name, std::string group_description = "") {
        if (!detail::valid_alias_name_string(group_name)) {
            throw IncorrectConstruction("option group names may not contain newlines or null characters");
        }
        auto option_group = std::make_shared<T>(std::move(group_description), group_name, this);
        auto* ptr = option_group.get();
        App_p app_ptr = std::static_pointer_cast<App>(option_group);
        app_ptr->footer_ = "";
        app_ptr->set_help_flag();
        add_subcommand(std::move(app_ptr));
        return ptr;
    }

    App* add_subcommand(std::string subcommand_name = "", std::string subcommand_description = "");

    App* add_subcommand(CLI::App_p subcom);

    bool remove_subcommand(App* subcom);

    App* get_subcommand(const App* subcom) const;

    CLI11_NODISCARD App* get_subcommand(std::string subcom) const;

    CLI11_NODISCARD App* get_subcommand_no_throw(std::string subcom) const noexcept;

    CLI11_NODISCARD App* get_subcommand(int index = 0) const;

    CLI::App_p get_subcommand_ptr(App* subcom) const;

    CLI11_NODISCARD CLI::App_p get_subcommand_ptr(std::string subcom) const;

    CLI11_NODISCARD CLI::App_p get_subcommand_ptr(int index = 0) const;

    CLI11_NODISCARD App* get_option_group(std::string group_name) const;

    CLI11_NODISCARD std::size_t count() const {
        return parsed_;
    }

    CLI11_NODISCARD std::size_t count_all() const;

    App* group(std::string group_name) {
        group_ = group_name;
        return this;
    }

    App* require_subcommand() {
        require_subcommand_min_ = 1;
        require_subcommand_max_ = 0;
        return this;
    }

    App* require_subcommand(int value) {
        if (value < 0) {
            require_subcommand_min_ = 0;
            require_subcommand_max_ = static_cast<std::size_t>(-value);
        } else {
            require_subcommand_min_ = static_cast<std::size_t>(value);
            require_subcommand_max_ = static_cast<std::size_t>(value);
        }
        return this;
    }

    App* require_subcommand(std::size_t min, std::size_t max) {
        require_subcommand_min_ = min;
        require_subcommand_max_ = max;
        return this;
    }

    App* require_option() {
        require_option_min_ = 1;
        require_option_max_ = 0;
        return this;
    }

    App* require_option(int value) {
        if (value < 0) {
            require_option_min_ = 0;
            require_option_max_ = static_cast<std::size_t>(-value);
        } else {
            require_option_min_ = static_cast<std::size_t>(value);
            require_option_max_ = static_cast<std::size_t>(value);
        }
        return this;
    }

    App* require_option(std::size_t min, std::size_t max) {
        require_option_min_ = min;
        require_option_max_ = max;
        return this;
    }

    App* fallthrough(bool value = true) {
        fallthrough_ = value;
        return this;
    }

    App* subcommand_fallthrough(bool value = true) {
        subcommand_fallthrough_ = value;
        return this;
    }

    explicit operator bool() const {
        return parsed_ > 0;
    }

    virtual void pre_callback() {}

    void clear();

    void parse(int argc, const char* const* argv);
    void parse(int argc, const wchar_t* const* argv);

   private:
    void prepare_for_parse(bool clear_existing_parse);
    template <typename TArgs>
    void parse_vector_common(TArgs&& args);
    template <class CharT>
    void parse_char_t(int argc, const CharT* const* argv);

   public:
    void parse(std::string commandline, bool program_name_included = false);
    void parse(std::wstring commandline, bool program_name_included = false);

    void parse(std::vector<std::string>& args);

    void parse(std::vector<std::string>&& args);

    void parse_from_stream(std::istream& input);

    void failure_message(std::function<std::string(const App*, const Error& e)> function) {
        failure_message_ = function;
    }

    int exit(const Error& e, std::ostream& out = std::cout, std::ostream& err = std::cerr) const;

    CLI11_NODISCARD std::size_t count(std::string option_name) const {
        return get_option(option_name)->count();
    }

    CLI11_NODISCARD std::vector<App*> get_subcommands() const {
        return parsed_subcommands_;
    }

    std::vector<const App*> get_subcommands(const std::function<bool(const App*)>& filter) const;

    std::vector<App*> get_subcommands(const std::function<bool(App*)>& filter);

    bool got_subcommand(const App* subcom) const {
        return get_subcommand(subcom)->parsed_ > 0;
    }

    CLI11_NODISCARD bool got_subcommand(std::string subcommand_name) const noexcept {
        App* sub = get_subcommand_no_throw(subcommand_name);
        return (sub != nullptr) ? (sub->parsed_ > 0) : false;
    }

    App* excludes(Option* opt) {
        if (opt == nullptr) {
            throw OptionNotFound("nullptr passed");
        }
        exclude_options_.insert(opt);
        return this;
    }

    App* excludes(App* app) {
        if (app == nullptr) {
            throw OptionNotFound("nullptr passed");
        }
        if (app == this) {
            throw OptionNotFound("cannot self reference in needs");
        }
        auto res = exclude_subcommands_.insert(app);
        if (res.second) {
            app->exclude_subcommands_.insert(this);
        }
        return this;
    }

    App* needs(Option* opt) {
        if (opt == nullptr) {
            throw OptionNotFound("nullptr passed");
        }
        need_options_.insert(opt);
        return this;
    }

    App* needs(App* app) {
        if (app == nullptr) {
            throw OptionNotFound("nullptr passed");
        }
        if (app == this) {
            throw OptionNotFound("cannot self reference in needs");
        }
        need_subcommands_.insert(app);
        return this;
    }

    bool remove_excludes(Option* opt);

    bool remove_excludes(App* app);

    bool remove_needs(Option* opt);

    bool remove_needs(App* app);

    App* usage(std::string usage_string) {
        usage_ = std::move(usage_string);
        return this;
    }
    App* usage(std::function<std::string()> usage_function) {
        usage_callback_ = std::move(usage_function);
        return this;
    }
    App* footer(std::string footer_string) {
        footer_ = std::move(footer_string);
        return this;
    }
    App* footer(std::function<std::string()> footer_function) {
        footer_callback_ = std::move(footer_function);
        return this;
    }
    CLI11_NODISCARD std::string config_to_str(bool default_also = false, bool write_description = false) const {
        return config_formatter_->to_config(this, default_also, write_description, "");
    }

    CLI11_NODISCARD std::string help(std::string prev = "", AppFormatMode mode = AppFormatMode::Normal) const;

    CLI11_NODISCARD std::string version() const;

    CLI11_NODISCARD std::shared_ptr<FormatterBase> get_formatter() const {
        return formatter_;
    }

    CLI11_NODISCARD std::shared_ptr<Config> get_config_formatter() const {
        return config_formatter_;
    }

    CLI11_NODISCARD std::shared_ptr<ConfigBase> get_config_formatter_base() const {
#if CLI11_USE_STATIC_RTTI == 0
        return std::dynamic_pointer_cast<ConfigBase>(config_formatter_);
#else
        return std::static_pointer_cast<ConfigBase>(config_formatter_);
#endif
    }

    CLI11_NODISCARD std::string get_description() const {
        return description_;
    }

    App* description(std::string app_description) {
        description_ = std::move(app_description);
        return this;
    }

    std::vector<const Option*> get_options(const std::function<bool(const Option*)> filter = {}) const;

    std::vector<Option*> get_options(const std::function<bool(Option*)> filter = {});

    CLI11_NODISCARD Option* get_option_no_throw(std::string option_name) noexcept;

    CLI11_NODISCARD const Option* get_option_no_throw(std::string option_name) const noexcept;

    CLI11_NODISCARD const Option* get_option(std::string option_name) const {
        const auto* opt = get_option_no_throw(option_name);
        if (opt == nullptr) {
            throw OptionNotFound(option_name);
        }
        return opt;
    }

    CLI11_NODISCARD Option* get_option(std::string option_name) {
        auto* opt = get_option_no_throw(option_name);
        if (opt == nullptr) {
            throw OptionNotFound(option_name);
        }
        return opt;
    }

    const Option* operator[](const std::string& option_name) const {
        return get_option(option_name);
    }

    const Option* operator[](const char* option_name) const {
        return get_option(option_name);
    }

    CLI11_NODISCARD bool get_ignore_case() const {
        return ignore_case_;
    }

    CLI11_NODISCARD bool get_ignore_underscore() const {
        return ignore_underscore_;
    }

    CLI11_NODISCARD bool get_fallthrough() const {
        return fallthrough_;
    }

    CLI11_NODISCARD bool get_subcommand_fallthrough() const {
        return subcommand_fallthrough_;
    }

    CLI11_NODISCARD bool get_allow_windows_style_options() const {
        return allow_windows_style_options_;
    }

    CLI11_NODISCARD bool get_positionals_at_end() const {
        return positionals_at_end_;
    }

    CLI11_NODISCARD bool get_configurable() const {
        return configurable_;
    }

    CLI11_NODISCARD const std::string& get_group() const {
        return group_;
    }

    CLI11_NODISCARD std::string get_usage() const {
        return (usage_callback_) ? usage_callback_() + '\n' + usage_ : usage_;
    }

    CLI11_NODISCARD std::string get_footer() const {
        return (footer_callback_) ? footer_callback_() + '\n' + footer_ : footer_;
    }

    CLI11_NODISCARD std::size_t get_require_subcommand_min() const {
        return require_subcommand_min_;
    }

    CLI11_NODISCARD std::size_t get_require_subcommand_max() const {
        return require_subcommand_max_;
    }

    CLI11_NODISCARD std::size_t get_require_option_min() const {
        return require_option_min_;
    }

    CLI11_NODISCARD std::size_t get_require_option_max() const {
        return require_option_max_;
    }

    CLI11_NODISCARD bool get_prefix_command() const {
        return static_cast<bool>(prefix_command_);
    }

    CLI11_NODISCARD PrefixCommandMode get_prefix_command_mode() const {
        return prefix_command_;
    }

    CLI11_NODISCARD bool get_allow_extras() const {
        return allow_extras_ > ExtrasMode::Ignore;
    }

    CLI11_NODISCARD ExtrasMode get_allow_extras_mode() const {
        return allow_extras_;
    }

    CLI11_NODISCARD bool get_required() const {
        return required_;
    }

    CLI11_NODISCARD bool get_disabled() const {
        return disabled_;
    }

    CLI11_NODISCARD bool get_silent() const {
        return silent_;
    }

    CLI11_NODISCARD bool get_allow_non_standard_option_names() const {
        return allow_non_standard_options_;
    }

    CLI11_NODISCARD bool get_allow_subcommand_prefix_matching() const {
        return allow_prefix_matching_;
    }

    CLI11_NODISCARD bool get_immediate_callback() const {
        return immediate_callback_;
    }

    CLI11_NODISCARD bool get_disabled_by_default() const {
        return (default_startup == startup_mode::disabled);
    }

    CLI11_NODISCARD bool get_enabled_by_default() const {
        return (default_startup == startup_mode::enabled);
    }
    CLI11_NODISCARD bool get_validate_positionals() const {
        return validate_positionals_;
    }
    CLI11_NODISCARD bool get_validate_optional_arguments() const {
        return validate_optional_arguments_;
    }

    CLI11_NODISCARD config_extras_mode get_allow_config_extras() const {
        return static_cast<config_extras_mode>(allow_config_extras_);
    }

    Option* get_help_ptr() {
        return help_ptr_;
    }

    CLI11_NODISCARD const Option* get_help_ptr() const {
        return help_ptr_;
    }

    CLI11_NODISCARD const Option* get_help_all_ptr() const {
        return help_all_ptr_;
    }

    Option* get_config_ptr() {
        return config_ptr_;
    }

    CLI11_NODISCARD const Option* get_config_ptr() const {
        return config_ptr_;
    }

    Option* get_version_ptr() {
        return version_ptr_;
    }

    CLI11_NODISCARD const Option* get_version_ptr() const {
        return version_ptr_;
    }

    App* get_parent() {
        return parent_;
    }

    CLI11_NODISCARD const App* get_parent() const {
        return parent_;
    }

    CLI11_NODISCARD const std::string& get_name() const {
        return name_;
    }

    CLI11_NODISCARD const std::vector<std::string>& get_aliases() const {
        return aliases_;
    }

    App* clear_aliases() {
        aliases_.clear();
        return this;
    }

    CLI11_NODISCARD std::string get_display_name(bool with_aliases = false) const;

    CLI11_NODISCARD bool check_name(std::string name_to_check) const;

    enum class NameMatch : std::uint8_t {
        none = 0,
        exact = 1,
        prefix = 2
    };

    CLI11_NODISCARD NameMatch check_name_detail(std::string name_to_check) const;

    CLI11_NODISCARD std::vector<std::string> get_groups() const;

    CLI11_NODISCARD const std::vector<Option*>& parse_order() const {
        return parse_order_;
    }

    CLI11_NODISCARD std::vector<std::string> remaining(bool recurse = false) const;

    CLI11_NODISCARD std::vector<std::string> remaining_for_passthrough(bool recurse = false) const;

    CLI11_NODISCARD std::size_t remaining_size(bool recurse = false) const;

   protected:
    void _validate() const;

    void _configure();

    void run_callback(bool final_mode = false, bool suppress_final_callback = false);

    CLI11_NODISCARD bool _valid_subcommand(const std::string& current, bool ignore_used = true) const;

    CLI11_NODISCARD detail::Classifier _recognize(const std::string& current,
                                                  bool ignore_used_subcommands = true) const;

    void _process_config_file();

    bool _process_config_file(const std::string& config_file, bool throw_error);

    void _process_env();

    void _process_callbacks(CallbackPriority priority);

    void _process_help_flags(CallbackPriority priority, bool trigger_help = false, bool trigger_all_help = false) const;

    void _process_requirements();

    void _process();

    void _process_extras();

    void increment_parsed();

    void _parse(std::vector<std::string>& args);

    void _parse(std::vector<std::string>&& args);

    void _parse_stream(std::istream& input);

    void _parse_config(const std::vector<ConfigItem>& args);

    bool _parse_single_config(const ConfigItem& item, std::size_t level = 0);

    bool _add_flag_like_result(Option* op, const ConfigItem& item, const std::vector<std::string>& inputs);

    bool _parse_single(std::vector<std::string>& args, bool& positional_only);

    CLI11_NODISCARD std::size_t _count_remaining_positionals(bool required_only = false) const;

    CLI11_NODISCARD bool _has_remaining_positionals() const;

    bool _parse_positional(std::vector<std::string>& args, bool haltOnSubcommand);

    CLI11_NODISCARD App* _find_subcommand(const std::string& subc_name, bool ignore_disabled,
                                          bool ignore_used) const noexcept;

    bool _parse_subcommand(std::vector<std::string>& args);

    bool _parse_arg(std::vector<std::string>& args, detail::Classifier current_type, bool local_processing_only);

    void _trigger_pre_parse(std::size_t remaining_args);

    CLI11_NODISCARD App* _get_fallthrough_parent() noexcept;

    CLI11_NODISCARD const App* _get_fallthrough_parent() const noexcept;

    CLI11_NODISCARD const std::string& _compare_subcommand_names(const App& subcom, const App& base) const;

    void _move_to_missing(detail::Classifier val_type, const std::string& val);

   public:
    void _move_option(Option* opt, App* app);
};

class Option_group : public App {
   public:
    Option_group(std::string group_description, std::string group_name, App* parent)
        : App(std::move(group_description), "", parent) {
        group(group_name);
        if (group_name.empty() || group_name.front() == '+') {
            set_help_flag("");
            set_help_all_flag("");
        }
    }
    using App::add_option;
    Option* add_option(Option* opt) {
        if (get_parent() == nullptr) {
            throw OptionNotFound("Unable to locate the specified option");
        }
        get_parent()->_move_option(opt, this);
        return opt;
    }
    void add_options(Option* opt) {
        add_option(opt);
    }
    template <typename... Args>
    void add_options(Option* opt, Args... args) {
        add_option(opt);
        add_options(args...);
    }
    using App::add_subcommand;
    App* add_subcommand(App* subcom) {
        App_p subc = subcom->get_parent()->get_subcommand_ptr(subcom);
        subc->get_parent()->remove_subcommand(subcom);
        add_subcommand(std::move(subc));
        return subcom;
    }
};

CLI11_INLINE void TriggerOn(App* trigger_app, App* app_to_enable);

CLI11_INLINE void TriggerOn(App* trigger_app, std::vector<App*> apps_to_enable);

CLI11_INLINE void TriggerOff(App* trigger_app, App* app_to_enable);

CLI11_INLINE void TriggerOff(App* trigger_app, std::vector<App*> apps_to_enable);

CLI11_INLINE void deprecate_option(Option* opt, const std::string& replacement = "");

inline void deprecate_option(App* app, const std::string& option_name, const std::string& replacement = "") {
    auto* opt = app->get_option(option_name);
    deprecate_option(opt, replacement);
}

inline void deprecate_option(App& app, const std::string& option_name, const std::string& replacement = "") {
    auto* opt = app.get_option(option_name);
    deprecate_option(opt, replacement);
}

CLI11_INLINE void retire_option(App* app, Option* opt);

CLI11_INLINE void retire_option(App& app, Option* opt);

CLI11_INLINE void retire_option(App* app, const std::string& option_name);

CLI11_INLINE void retire_option(App& app, const std::string& option_name);

namespace detail {
struct AppFriend {
#ifdef CLI11_CPP14

    template <typename... Args>
    static decltype(auto) parse_arg(App* app, Args&&... args) {
        return app->_parse_arg(std::forward<Args>(args)...);
    }

    template <typename... Args>
    static decltype(auto) parse_subcommand(App* app, Args&&... args) {
        return app->_parse_subcommand(std::forward<Args>(args)...);
    }
#else
    template <typename... Args>
    static auto parse_arg(App* app, Args&&... args) ->
        typename std::result_of<decltype (&App::_parse_arg)(App, Args...)>::type {
        return app->_parse_arg(std::forward<Args>(args)...);
    }

    template <typename... Args>
    static auto parse_subcommand(App* app, Args&&... args) ->
        typename std::result_of<decltype (&App::_parse_subcommand)(App, Args...)>::type {
        return app->_parse_subcommand(std::forward<Args>(args)...);
    }
#endif
    static App* get_fallthrough_parent(App* app) {
        return app->_get_fallthrough_parent();
    }

    static const App* get_fallthrough_parent(const App* app) {
        return app->_get_fallthrough_parent();
    }
};
}  

CLI11_INLINE App::App(std::string app_description, std::string app_name, App* parent)
    : name_(std::move(app_name)), description_(std::move(app_description)), parent_(parent) {
    if (parent_ != nullptr) {
        if (parent_->help_ptr_ != nullptr)
            set_help_flag(parent_->help_ptr_->get_name(false, true), parent_->help_ptr_->get_description());
        if (parent_->help_all_ptr_ != nullptr)
            set_help_all_flag(parent_->help_all_ptr_->get_name(false, true), parent_->help_all_ptr_->get_description());

        option_defaults_ = parent_->option_defaults_;

        failure_message_ = parent_->failure_message_;
        allow_extras_ = parent_->allow_extras_;
        allow_config_extras_ = parent_->allow_config_extras_;
        prefix_command_ = parent_->prefix_command_;
        immediate_callback_ = parent_->immediate_callback_;
        ignore_case_ = parent_->ignore_case_;
        ignore_underscore_ = parent_->ignore_underscore_;
        fallthrough_ = parent_->fallthrough_;
        validate_positionals_ = parent_->validate_positionals_;
        validate_optional_arguments_ = parent_->validate_optional_arguments_;
        configurable_ = parent_->configurable_;
        allow_windows_style_options_ = parent_->allow_windows_style_options_;
        group_ = parent_->group_;
        usage_ = parent_->usage_;
        footer_ = parent_->footer_;
        formatter_ = parent_->formatter_;
        config_formatter_ = parent_->config_formatter_;
        require_subcommand_max_ = parent_->require_subcommand_max_;
        allow_prefix_matching_ = parent_->allow_prefix_matching_;
    }
}

CLI11_NODISCARD CLI11_INLINE char** App::ensure_utf8(char** argv) {
#ifdef _WIN32
    (void)argv;

    normalized_argv_ = detail::compute_win32_argv();

    if (!normalized_argv_view_.empty()) {
        normalized_argv_view_.clear();
    }

    normalized_argv_view_.reserve(normalized_argv_.size());
    for (auto& arg : normalized_argv_) {
        normalized_argv_view_.push_back(const_cast<char*>(arg.data()));
    }

    return normalized_argv_view_.data();
#else
    return argv;
#endif
}

CLI11_INLINE App* App::name(std::string app_name) {
    if (parent_ != nullptr) {
        std::string oname = name_;
        name_ = app_name;
        const auto& res = _compare_subcommand_names(*this, *_get_fallthrough_parent());
        if (!res.empty()) {
            name_ = oname;
            throw(OptionAlreadyAdded(app_name + " conflicts with existing subcommand names"));
        }
    } else {
        name_ = app_name;
    }
    has_automatic_name_ = false;
    return this;
}

CLI11_INLINE App* App::alias(std::string app_name) {
    if (app_name.empty() || !detail::valid_alias_name_string(app_name)) {
        throw IncorrectConstruction("Aliases may not be empty or contain newlines or null characters");
    }
    if (parent_ != nullptr) {
        aliases_.push_back(app_name);
        const auto& res = _compare_subcommand_names(*this, *_get_fallthrough_parent());
        if (!res.empty()) {
            aliases_.pop_back();
            throw(OptionAlreadyAdded("alias already matches an existing subcommand: " + app_name));
        }
    } else {
        aliases_.push_back(app_name);
    }

    return this;
}

CLI11_INLINE App* App::immediate_callback(bool immediate) {
    immediate_callback_ = immediate;
    if (immediate_callback_) {
        if (final_callback_ && !(parse_complete_callback_)) {
            std::swap(final_callback_, parse_complete_callback_);
        }
    } else if (!(final_callback_) && parse_complete_callback_) {
        std::swap(final_callback_, parse_complete_callback_);
    }
    return this;
}

CLI11_INLINE App* App::ignore_case(bool value) {
    if (value && !ignore_case_) {
        ignore_case_ = true;
        auto* p = (parent_ != nullptr) ? _get_fallthrough_parent() : this;
        const auto& match = _compare_subcommand_names(*this, *p);
        if (!match.empty()) {
            ignore_case_ = false;
            throw OptionAlreadyAdded("ignore case would cause subcommand name conflicts: " + match);
        }
    }
    ignore_case_ = value;
    return this;
}

CLI11_INLINE App* App::ignore_underscore(bool value) {
    if (value && !ignore_underscore_) {
        ignore_underscore_ = true;
        auto* p = (parent_ != nullptr) ? _get_fallthrough_parent() : this;
        const auto& match = _compare_subcommand_names(*this, *p);
        if (!match.empty()) {
            ignore_underscore_ = false;
            throw OptionAlreadyAdded("ignore underscore would cause subcommand name conflicts: " + match);
        }
    }
    ignore_underscore_ = value;
    return this;
}

CLI11_INLINE Option* App::add_option(std::string option_name, callback_t option_callback,
                                     std::string option_description, bool defaulted,
                                     std::function<std::string()> func) {
    Option myopt{option_name, option_description, option_callback, this, allow_non_standard_options_};

    auto res =
        std::find_if(std::begin(options_), std::end(options_), [&myopt](const Option_p& v) { return *v == myopt; });
    if (res != options_.end()) {
        const auto& matchname = (*res)->matching_name(myopt);
        throw(OptionAlreadyAdded("added option matched existing option name: " + matchname));
    }
    const App* top_level_parent = this;
    while (top_level_parent->name_.empty() && top_level_parent->parent_ != nullptr) {
        top_level_parent = top_level_parent->parent_;
    }

    if (myopt.lnames_.empty() && myopt.snames_.empty()) {
        std::string test_name = "--" + myopt.get_single_name();
        if (test_name.size() == 3) {
            test_name.erase(0, 1);
        }
        const auto* op = top_level_parent->get_option_no_throw(test_name);
        if (op != nullptr && op->get_configurable()) {
            throw(OptionAlreadyAdded("added option positional name matches existing option: " + test_name));
        }
        op = top_level_parent->get_option_no_throw(myopt.get_single_name());
        if (op != nullptr && op->lnames_.empty() && op->snames_.empty()) {
            throw(OptionAlreadyAdded("unable to disambiguate with existing option: " + test_name));
        }
    } else if (top_level_parent != this) {
        for (auto& ln : myopt.lnames_) {
            const auto* op = top_level_parent->get_option_no_throw(ln);
            if (op != nullptr && op->get_configurable()) {
                throw(OptionAlreadyAdded("added option matches existing positional option: " + ln));
            }
            op = top_level_parent->get_option_no_throw("--" + ln);
            if (op != nullptr && op->get_configurable()) {
                throw(OptionAlreadyAdded("added option matches existing option: --" + ln));
            }
            if (ln.size() == 1 || top_level_parent->get_allow_non_standard_option_names()) {
                op = top_level_parent->get_option_no_throw("-" + ln);
                if (op != nullptr && op->get_configurable()) {
                    throw(OptionAlreadyAdded("added option matches existing option: -" + ln));
                }
            }
        }
        for (auto& sn : myopt.snames_) {
            const auto* op = top_level_parent->get_option_no_throw(sn);
            if (op != nullptr && op->get_configurable()) {
                throw(OptionAlreadyAdded("added option matches existing positional option: " + sn));
            }
            op = top_level_parent->get_option_no_throw("-" + sn);
            if (op != nullptr && op->get_configurable()) {
                throw(OptionAlreadyAdded("added option matches existing option: -" + sn));
            }
            op = top_level_parent->get_option_no_throw("--" + sn);
            if (op != nullptr && op->get_configurable()) {
                throw(OptionAlreadyAdded("added option matches existing option: --" + sn));
            }
        }
    }
    if (allow_non_standard_options_ && !myopt.snames_.empty()) {
        for (auto& sname : myopt.snames_) {
            if (sname.length() > 1) {
                std::string test_name;
                test_name.push_back('-');
                test_name.push_back(sname.front());
                const auto* op = top_level_parent->get_option_no_throw(test_name);
                if (op != nullptr) {
                    throw(OptionAlreadyAdded("added option interferes with existing short option: " + sname));
                }
            }
        }
        for (auto& opt : top_level_parent->get_options()) {
            for (const auto& osn : opt->snames_) {
                if (osn.size() > 1) {
                    std::string test_name;
                    test_name.push_back(osn.front());
                    if (myopt.check_sname(test_name)) {
                        throw(OptionAlreadyAdded("added option interferes with existing non standard option: " + osn));
                    }
                }
            }
        }
    }
    options_.emplace_back();
    Option_p& option = options_.back();
    option.reset(new Option(option_name, option_description, option_callback, this, allow_non_standard_options_));

    option->default_function(func);

    if (defaulted)
        option->capture_default_str();

    option_defaults_.copy_to(option.get());

    if (!defaulted && option->get_always_capture_default())
        option->capture_default_str();

    return option.get();
}

CLI11_INLINE Option* App::set_help_flag(std::string flag_name, const std::string& help_description) {
    if (help_ptr_ != nullptr) {
        remove_option(help_ptr_);
        help_ptr_ = nullptr;
    }

    if (!flag_name.empty()) {
        help_ptr_ = add_flag(flag_name, help_description);
        help_ptr_->configurable(false)->callback_priority(CallbackPriority::First);
    }

    return help_ptr_;
}

CLI11_INLINE Option* App::set_help_all_flag(std::string help_name, const std::string& help_description) {
    if (help_all_ptr_ != nullptr) {
        remove_option(help_all_ptr_);
        help_all_ptr_ = nullptr;
    }

    if (!help_name.empty()) {
        help_all_ptr_ = add_flag(help_name, help_description);
        help_all_ptr_->configurable(false)->callback_priority(CallbackPriority::First);
    }

    return help_all_ptr_;
}

CLI11_INLINE Option* App::set_version_flag(std::string flag_name, const std::string& versionString,
                                           const std::string& version_help) {
    if (version_ptr_ != nullptr) {
        remove_option(version_ptr_);
        version_ptr_ = nullptr;
    }

    if (!flag_name.empty()) {
        version_ptr_ = add_flag_callback(
            flag_name, [versionString]() { throw(CLI::CallForVersion(versionString, 0)); }, version_help);
        version_ptr_->configurable(false)->callback_priority(CallbackPriority::First);
    }

    return version_ptr_;
}

CLI11_INLINE Option* App::set_version_flag(std::string flag_name, std::function<std::string()> vfunc,
                                           const std::string& version_help) {
    if (version_ptr_ != nullptr) {
        remove_option(version_ptr_);
        version_ptr_ = nullptr;
    }

    if (!flag_name.empty()) {
        version_ptr_ =
            add_flag_callback(flag_name, [vfunc]() { throw(CLI::CallForVersion(vfunc(), 0)); }, version_help);
        version_ptr_->configurable(false)->callback_priority(CallbackPriority::First);
    }

    return version_ptr_;
}

CLI11_INLINE Option* App::_add_flag_internal(std::string flag_name, CLI::callback_t fun, std::string flag_description) {
    Option* opt = nullptr;
    if (detail::has_default_flag_values(flag_name)) {
        auto flag_defaults = detail::get_default_flag_values(flag_name);
        detail::remove_default_flag_values(flag_name);
        opt = add_option(std::move(flag_name), std::move(fun), std::move(flag_description), false);
        for (const auto& fname : flag_defaults)
            opt->fnames_.push_back(fname.first);
        opt->default_flag_values_ = std::move(flag_defaults);
    } else {
        opt = add_option(std::move(flag_name), std::move(fun), std::move(flag_description), false);
    }
    if (opt->get_positional()) {
        auto pos_name = opt->get_name(true);
        remove_option(opt);
        throw IncorrectConstruction::PositionalFlag(pos_name);
    }
    opt->multi_option_policy(MultiOptionPolicy::TakeLast);
    opt->expected(0);
    opt->required(false);
    return opt;
}

CLI11_INLINE Option* App::add_flag_callback(std::string flag_name, std::function<void(void)> function,
                                            std::string flag_description) {
    CLI::callback_t fun = [function](const CLI::results_t& res) {
        using CLI::detail::lexical_cast;
        bool trigger{false};
        auto result = lexical_cast(res[0], trigger);
        if (result && trigger) {
            function();
        }
        return result;
    };
    return _add_flag_internal(flag_name, std::move(fun), std::move(flag_description));
}

CLI11_INLINE Option* App::add_flag_function(std::string flag_name, std::function<void(std::int64_t)> function,
                                            std::string flag_description) {
    CLI::callback_t fun = [function](const CLI::results_t& res) {
        using CLI::detail::lexical_cast;
        std::int64_t flag_count{0};
        lexical_cast(res[0], flag_count);
        function(flag_count);
        return true;
    };
    return _add_flag_internal(flag_name, std::move(fun), std::move(flag_description))
        ->multi_option_policy(MultiOptionPolicy::Sum);
}

CLI11_INLINE Option* App::set_config(std::string option_name, std::string default_filename,
                                     const std::string& help_message, bool config_required) {
    if (config_ptr_ != nullptr) {
        remove_option(config_ptr_);
        config_ptr_ = nullptr;
    }

    if (!option_name.empty()) {
        config_ptr_ = add_option(option_name, help_message);
        if (config_required) {
            config_ptr_->required();
        }
        if (!default_filename.empty()) {
            config_ptr_->default_str(std::move(default_filename));
            config_ptr_->force_callback_ = true;
        }
        config_ptr_->configurable(false);
        config_ptr_->multi_option_policy(MultiOptionPolicy::Reverse);
    }

    return config_ptr_;
}

CLI11_INLINE bool App::remove_option(Option* opt) {
    for (Option_p& op : options_) {
        op->remove_needs(opt);
        op->remove_excludes(opt);
    }

    if (help_ptr_ == opt)
        help_ptr_ = nullptr;
    if (help_all_ptr_ == opt)
        help_all_ptr_ = nullptr;
    if (config_ptr_ == opt)
        config_ptr_ = nullptr;

    auto iterator =
        std::find_if(std::begin(options_), std::end(options_), [opt](const Option_p& v) { return v.get() == opt; });
    if (iterator != std::end(options_)) {
        options_.erase(iterator);
        return true;
    }
    return false;
}

CLI11_INLINE App* App::add_subcommand(std::string subcommand_name, std::string subcommand_description) {
    if (!subcommand_name.empty() && !detail::valid_name_string(subcommand_name)) {
        if (!detail::valid_first_char(subcommand_name[0])) {
            throw IncorrectConstruction(
                "Subcommand name starts with invalid character, '!' and '-' and control characters");
        }
        for (auto c : subcommand_name) {
            if (!detail::valid_later_char(c)) {
                throw IncorrectConstruction(std::string("Subcommand name contains invalid character ('") + c +
                                            "'), all characters are allowed except"
                                            "'=',':','{','}', ' ', and control characters");
            }
        }
    }
    CLI::App_p subcom = std::shared_ptr<App>(new App(std::move(subcommand_description), subcommand_name, this));
    return add_subcommand(std::move(subcom));
}

CLI11_INLINE App* App::add_subcommand(CLI::App_p subcom) {
    if (!subcom)
        throw IncorrectConstruction("passed App is not valid");
    auto* ckapp = (name_.empty() && parent_ != nullptr) ? _get_fallthrough_parent() : this;
    const auto& mstrg = _compare_subcommand_names(*subcom, *ckapp);
    if (!mstrg.empty()) {
        throw(OptionAlreadyAdded("subcommand name or alias matches existing subcommand: " + mstrg));
    }
    subcom->parent_ = this;
    subcommands_.push_back(std::move(subcom));
    return subcommands_.back().get();
}

CLI11_INLINE bool App::remove_subcommand(App* subcom) {
    for (App_p& sub : subcommands_) {
        sub->remove_excludes(subcom);
        sub->remove_needs(subcom);
    }

    auto iterator = std::find_if(std::begin(subcommands_), std::end(subcommands_),
                                 [subcom](const App_p& v) { return v.get() == subcom; });
    if (iterator != std::end(subcommands_)) {
        subcommands_.erase(iterator);
        return true;
    }
    return false;
}

CLI11_INLINE App* App::get_subcommand(const App* subcom) const {
    if (subcom == nullptr)
        throw OptionNotFound("nullptr passed");
    for (const App_p& subcomptr : subcommands_)
        if (subcomptr.get() == subcom)
            return subcomptr.get();
    throw OptionNotFound(subcom->get_name());
}

CLI11_NODISCARD CLI11_INLINE App* App::get_subcommand(std::string subcom) const {
    auto* subc = _find_subcommand(subcom, false, false);
    if (subc == nullptr)
        throw OptionNotFound(subcom);
    return subc;
}

CLI11_NODISCARD CLI11_INLINE App* App::get_subcommand_no_throw(std::string subcom) const noexcept {
    return _find_subcommand(subcom, false, false);
}

CLI11_NODISCARD CLI11_INLINE App* App::get_subcommand(int index) const {
    if (index >= 0) {
        auto uindex = static_cast<unsigned>(index);
        if (uindex < subcommands_.size())
            return subcommands_[uindex].get();
    }
    throw OptionNotFound(std::to_string(index));
}

CLI11_INLINE CLI::App_p App::get_subcommand_ptr(App* subcom) const {
    if (subcom == nullptr)
        throw OptionNotFound("nullptr passed");
    for (const App_p& subcomptr : subcommands_)
        if (subcomptr.get() == subcom)
            return subcomptr;
    throw OptionNotFound(subcom->get_name());
}

CLI11_NODISCARD CLI11_INLINE CLI::App_p App::get_subcommand_ptr(std::string subcom) const {
    for (const App_p& subcomptr : subcommands_)
        if (subcomptr->check_name(subcom))
            return subcomptr;
    throw OptionNotFound(subcom);
}

CLI11_NODISCARD CLI11_INLINE CLI::App_p App::get_subcommand_ptr(int index) const {
    if (index >= 0) {
        auto uindex = static_cast<unsigned>(index);
        if (uindex < subcommands_.size())
            return subcommands_[uindex];
    }
    throw OptionNotFound(std::to_string(index));
}

CLI11_NODISCARD CLI11_INLINE CLI::App* App::get_option_group(std::string group_name) const {
    for (const App_p& app : subcommands_) {
        if (app->name_.empty() && app->group_ == group_name) {
            return app.get();
        }
    }
    throw OptionNotFound(group_name);
}

CLI11_NODISCARD CLI11_INLINE std::size_t App::count_all() const {
    std::size_t cnt{0};
    for (const auto& opt : options_) {
        cnt += opt->count();
    }
    for (const auto& sub : subcommands_) {
        cnt += sub->count_all();
    }
    if (!get_name().empty()) {
        cnt += parsed_;
    }
    return cnt;
}

CLI11_INLINE void App::clear() {
    parsed_ = 0;
    pre_parse_called_ = false;

    missing_.clear();
    parsed_subcommands_.clear();
    parse_order_.clear();
    for (const Option_p& opt : options_) {
        opt->clear();
    }
    for (const App_p& subc : subcommands_) {
        subc->clear();
    }
}

CLI11_INLINE void App::parse(int argc, const char* const* argv) {
    parse_char_t(argc, argv);
}
CLI11_INLINE void App::parse(int argc, const wchar_t* const* argv) {
    parse_char_t(argc, argv);
}

namespace detail {

CLI11_INLINE const char* maybe_narrow(const char* str) {
    return str;
}
CLI11_INLINE std::string maybe_narrow(const wchar_t* str) {
    return narrow(str);
}

}  

template <class CharT>
CLI11_INLINE void App::parse_char_t(int argc, const CharT* const* argv) {
    if (name_.empty() || has_automatic_name_) {
        has_automatic_name_ = true;
        name_ = detail::maybe_narrow(argv[0]);
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc) - 1U);
    for (auto i = static_cast<std::size_t>(argc) - 1U; i > 0U; --i)
        args.emplace_back(detail::maybe_narrow(argv[i]));

    parse(std::move(args));
}

CLI11_INLINE void App::parse(std::string commandline, bool program_name_included) {
    if (program_name_included) {
        auto nstr = detail::split_program_name(commandline);
        if ((name_.empty()) || (has_automatic_name_)) {
            has_automatic_name_ = true;
            name_ = nstr.first;
        }
        commandline = std::move(nstr.second);
    } else {
        detail::trim(commandline);
    }
    if (!commandline.empty()) {
        commandline = detail::find_and_modify(commandline, "=", detail::escape_detect);
        if (allow_windows_style_options_)
            commandline = detail::find_and_modify(commandline, ":", detail::escape_detect);
    }

    auto args = detail::split_up(std::move(commandline));
    args.erase(std::remove(args.begin(), args.end(), std::string{}), args.end());
    try {
        detail::remove_quotes(args);
    } catch (const std::invalid_argument& arg) {
        throw CLI::ParseError(arg.what(), CLI::ExitCodes::InvalidError);
    }
    std::reverse(args.begin(), args.end());
    parse(std::move(args));
}

CLI11_INLINE void App::parse(std::wstring commandline, bool program_name_included) {
    parse(narrow(commandline), program_name_included);
}

CLI11_INLINE void App::prepare_for_parse(bool clear_existing_parse) {
    if (clear_existing_parse && parsed_ > 0)
        clear();

    if (parsed_ == 0) {
        if (clear_existing_parse) {
            parsed_ = 1;
        }

        _validate();
        _configure();

        if (clear_existing_parse) {
            parent_ = nullptr;
            parsed_ = 0;
        }
    }
}

CLI11_INLINE void App::parse(std::vector<std::string>& args) {
    parse_vector_common(args);
}

CLI11_INLINE void App::parse(std::vector<std::string>&& args) {
    parse_vector_common(std::move(args));
}

template <typename TArgs>
CLI11_INLINE void App::parse_vector_common(TArgs&& args) {
    prepare_for_parse(true);
    _parse(std::forward<TArgs>(args));
    run_callback();
}

CLI11_INLINE void App::parse_from_stream(std::istream& input) {
    prepare_for_parse(false);
    _parse_stream(input);
    run_callback();
}

CLI11_INLINE int App::exit(const Error& e, std::ostream& out, std::ostream& err) const {
    if (e.get_name() == "RuntimeError")
        return e.get_exit_code();

    if (e.get_name() == "CallForHelp") {
        out << help();
        return e.get_exit_code();
    }

    if (e.get_name() == "CallForAllHelp") {
        out << help("", AppFormatMode::All);
        return e.get_exit_code();
    }

    if (e.get_name() == "CallForVersion") {
        out << e.what() << '\n';
        return e.get_exit_code();
    }

    if (e.get_exit_code() != static_cast<int>(ExitCodes::Success)) {
        if (failure_message_)
            err << failure_message_(this, e) << std::flush;
    }

    return e.get_exit_code();
}

CLI11_INLINE std::vector<const App*> App::get_subcommands(const std::function<bool(const App*)>& filter) const {
    std::vector<const App*> subcomms(subcommands_.size());
    std::transform(std::begin(subcommands_), std::end(subcommands_), std::begin(subcomms),
                   [](const App_p& v) { return v.get(); });

    if (filter) {
        subcomms.erase(std::remove_if(std::begin(subcomms), std::end(subcomms),
                                      [&filter](const App* app) { return !filter(app); }),
                       std::end(subcomms));
    }

    return subcomms;
}

CLI11_INLINE std::vector<App*> App::get_subcommands(const std::function<bool(App*)>& filter) {
    std::vector<App*> subcomms(subcommands_.size());
    std::transform(std::begin(subcommands_), std::end(subcommands_), std::begin(subcomms),
                   [](const App_p& v) { return v.get(); });

    if (filter) {
        subcomms.erase(
            std::remove_if(std::begin(subcomms), std::end(subcomms), [&filter](App* app) { return !filter(app); }),
            std::end(subcomms));
    }

    return subcomms;
}

CLI11_INLINE bool App::remove_excludes(Option* opt) {
    auto iterator = std::find(std::begin(exclude_options_), std::end(exclude_options_), opt);
    if (iterator == std::end(exclude_options_)) {
        return false;
    }
    exclude_options_.erase(iterator);
    return true;
}

CLI11_INLINE bool App::remove_excludes(App* app) {
    auto iterator = std::find(std::begin(exclude_subcommands_), std::end(exclude_subcommands_), app);
    if (iterator == std::end(exclude_subcommands_)) {
        return false;
    }
    auto* other_app = *iterator;
    exclude_subcommands_.erase(iterator);
    other_app->remove_excludes(this);
    return true;
}

CLI11_INLINE bool App::remove_needs(Option* opt) {
    auto iterator = std::find(std::begin(need_options_), std::end(need_options_), opt);
    if (iterator == std::end(need_options_)) {
        return false;
    }
    need_options_.erase(iterator);
    return true;
}

CLI11_INLINE bool App::remove_needs(App* app) {
    auto iterator = std::find(std::begin(need_subcommands_), std::end(need_subcommands_), app);
    if (iterator == std::end(need_subcommands_)) {
        return false;
    }
    need_subcommands_.erase(iterator);
    return true;
}

CLI11_NODISCARD CLI11_INLINE std::string App::help(std::string prev, AppFormatMode mode) const {
    if (prev.empty())
        prev = get_name();
    else
        prev += " " + get_name();

    auto selected_subcommands = get_subcommands();
    if (!selected_subcommands.empty()) {
        return selected_subcommands.back()->help(prev, mode);
    }
    return formatter_->make_help(this, prev, mode);
}

CLI11_NODISCARD CLI11_INLINE std::string App::version() const {
    std::string val;
    if (version_ptr_ != nullptr) {
        results_t rv = version_ptr_->results();
        version_ptr_->clear();
        version_ptr_->add_result("true");
        try {
            version_ptr_->run_callback();
        } catch (const CLI::CallForVersion& cfv) {
            val = cfv.what();
        }
        version_ptr_->clear();
        version_ptr_->add_result(rv);
    }
    return val;
}

CLI11_INLINE std::vector<const Option*> App::get_options(const std::function<bool(const Option*)> filter) const {
    std::vector<const Option*> options(options_.size());
    std::transform(std::begin(options_), std::end(options_), std::begin(options),
                   [](const Option_p& val) { return val.get(); });

    if (filter) {
        options.erase(std::remove_if(std::begin(options), std::end(options),
                                     [&filter](const Option* opt) { return !filter(opt); }),
                      std::end(options));
    }
    for (const auto& subcp : subcommands_) {
        const App* subc = subcp.get();
        if (subc->get_name().empty() && !subc->get_group().empty() && subc->get_group().front() == '+') {
            std::vector<const Option*> subcopts = subc->get_options(filter);
            options.insert(options.end(), subcopts.begin(), subcopts.end());
        }
    }
    if (fallthrough_ && parent_ != nullptr) {
        const auto* fallthrough_parent = _get_fallthrough_parent();
        std::vector<const Option*> subcopts = fallthrough_parent->get_options(filter);
        for (const auto* opt : subcopts) {
            if (std::find_if(options.begin(), options.end(), [opt](const Option* opt2) {
                    return opt->check_name(opt2->get_name());
                }) == options.end()) {
                options.push_back(opt);
            }
        }
    }
    return options;
}

CLI11_INLINE std::vector<Option*> App::get_options(const std::function<bool(Option*)> filter) {
    std::vector<Option*> options(options_.size());
    std::transform(std::begin(options_), std::end(options_), std::begin(options),
                   [](const Option_p& val) { return val.get(); });

    if (filter) {
        options.erase(
            std::remove_if(std::begin(options), std::end(options), [&filter](Option* opt) { return !filter(opt); }),
            std::end(options));
    }
    for (auto& subc : subcommands_) {
        if (subc->get_name().empty() || (!subc->get_group().empty() && subc->get_group().front() == '+')) {
            auto subcopts = subc->get_options(filter);
            options.insert(options.end(), subcopts.begin(), subcopts.end());
        }
    }
    if (fallthrough_ && parent_ != nullptr) {
        auto* fallthrough_parent = _get_fallthrough_parent();
        std::vector<Option*> subcopts = fallthrough_parent->get_options(filter);
        for (auto* opt : subcopts) {
            if (std::find_if(options.begin(), options.end(),
                             [opt](Option* opt2) { return opt->check_name(opt2->get_name()); }) == options.end()) {
                options.push_back(opt);
            }
        }
    }
    return options;
}

CLI11_NODISCARD CLI11_INLINE Option* App::get_option_no_throw(std::string option_name) noexcept {
    for (Option_p& opt : options_) {
        if (opt->check_name(option_name)) {
            return opt.get();
        }
    }
    for (auto& subc : subcommands_) {
        if (subc->get_name().empty()) {
            auto* opt = subc->get_option_no_throw(option_name);
            if (opt != nullptr) {
                return opt;
            }
        }
    }
    if (fallthrough_ && parent_ != nullptr) {
        return _get_fallthrough_parent()->get_option_no_throw(option_name);
    }
    return nullptr;
}

CLI11_NODISCARD CLI11_INLINE const Option* App::get_option_no_throw(std::string option_name) const noexcept {
    for (const Option_p& opt : options_) {
        if (opt->check_name(option_name)) {
            return opt.get();
        }
    }
    for (const auto& subc : subcommands_) {
        if (subc->get_name().empty()) {
            auto* opt = subc->get_option_no_throw(option_name);
            if (opt != nullptr) {
                return opt;
            }
        }
    }
    if (fallthrough_ && parent_ != nullptr) {
        return _get_fallthrough_parent()->get_option_no_throw(option_name);
    }
    return nullptr;
}

CLI11_NODISCARD CLI11_INLINE std::string App::get_display_name(bool with_aliases) const {
    if (name_.empty()) {
        return std::string("[Option Group: ") + get_group() + "]";
    }
    if (aliases_.empty() || !with_aliases) {
        return name_;
    }
    std::string dispname = name_;
    for (const auto& lalias : aliases_) {
        dispname.push_back(',');
        dispname.push_back(' ');
        dispname.append(lalias);
    }
    return dispname;
}

CLI11_NODISCARD CLI11_INLINE bool App::check_name(std::string name_to_check) const {
    auto result = check_name_detail(std::move(name_to_check));
    return (result != NameMatch::none);
}

CLI11_NODISCARD CLI11_INLINE App::NameMatch App::check_name_detail(std::string name_to_check) const {
    std::string local_name = name_;
    if (ignore_underscore_) {
        local_name = detail::remove_underscore(name_);
        name_to_check = detail::remove_underscore(name_to_check);
    }
    if (ignore_case_) {
        local_name = detail::to_lower(name_);
        name_to_check = detail::to_lower(name_to_check);
    }

    if (local_name == name_to_check) {
        return App::NameMatch::exact;
    }
    if (allow_prefix_matching_ && name_to_check.size() < local_name.size()) {
        if (local_name.compare(0, name_to_check.size(), name_to_check) == 0) {
            return App::NameMatch::prefix;
        }
    }
    for (std::string les : aliases_) {  // NOLINT(performance-for-range-copy)
        if (ignore_underscore_) {
            les = detail::remove_underscore(les);
        }
        if (ignore_case_) {
            les = detail::to_lower(les);
        }
        if (les == name_to_check) {
            return App::NameMatch::exact;
        }
        if (allow_prefix_matching_ && name_to_check.size() < les.size()) {
            if (les.compare(0, name_to_check.size(), name_to_check) == 0) {
                return App::NameMatch::prefix;
            }
        }
    }
    return App::NameMatch::none;
}

CLI11_NODISCARD CLI11_INLINE std::vector<std::string> App::get_groups() const {
    std::vector<std::string> groups;

    for (const Option_p& opt : options_) {
        if (std::find(groups.begin(), groups.end(), opt->get_group()) == groups.end()) {
            groups.push_back(opt->get_group());
        }
    }

    return groups;
}

CLI11_NODISCARD CLI11_INLINE std::vector<std::string> App::remaining(bool recurse) const {
    std::vector<std::string> miss_list;
    for (const std::pair<detail::Classifier, std::string>& miss : missing_) {
        miss_list.push_back(std::get<1>(miss));
    }
    if (recurse) {
        if (allow_extras_ == ExtrasMode::Error || allow_extras_ == ExtrasMode::Ignore) {
            for (const auto& sub : subcommands_) {
                if (sub->name_.empty() && !sub->missing_.empty()) {
                    for (const std::pair<detail::Classifier, std::string>& miss : sub->missing_) {
                        miss_list.push_back(std::get<1>(miss));
                    }
                }
            }
        }

        for (const App* sub : parsed_subcommands_) {
            std::vector<std::string> output = sub->remaining(recurse);
            std::copy(std::begin(output), std::end(output), std::back_inserter(miss_list));
        }
    }
    return miss_list;
}

CLI11_NODISCARD CLI11_INLINE std::vector<std::string> App::remaining_for_passthrough(bool recurse) const {
    std::vector<std::string> miss_list = remaining(recurse);
    std::reverse(std::begin(miss_list), std::end(miss_list));
    return miss_list;
}

CLI11_NODISCARD CLI11_INLINE std::size_t App::remaining_size(bool recurse) const {
    auto remaining_options = static_cast<std::size_t>(std::count_if(
        std::begin(missing_), std::end(missing_), [](const std::pair<detail::Classifier, std::string>& val) {
            return val.first != detail::Classifier::POSITIONAL_MARK;
        }));

    if (recurse) {
        for (const App_p& sub : subcommands_) {
            remaining_options += sub->remaining_size(recurse);
        }
    }
    return remaining_options;
}

CLI11_INLINE void App::_validate() const {
    auto pcount = std::count_if(std::begin(options_), std::end(options_), [](const Option_p& opt) {
        return opt->get_items_expected_max() >= detail::expected_max_vector_size && !opt->nonpositional();
    });
    if (pcount > 1) {
        auto pcount_req = std::count_if(std::begin(options_), std::end(options_), [](const Option_p& opt) {
            return opt->get_items_expected_max() >= detail::expected_max_vector_size && !opt->nonpositional() &&
                   opt->get_required();
        });
        if (pcount - pcount_req > 1) {
            throw InvalidError(name_);
        }
    }

    std::size_t nameless_subs{0};
    for (const App_p& app : subcommands_) {
        app->_validate();
        if (app->get_name().empty())
            ++nameless_subs;
    }

    if (require_option_min_ > 0) {
        if (require_option_max_ > 0) {
            if (require_option_max_ < require_option_min_) {
                throw(InvalidError("Required min options greater than required max options", ExitCodes::InvalidError));
            }
        }
        if (require_option_min_ > (options_.size() + nameless_subs)) {
            throw(
                InvalidError("Required min options greater than number of available options", ExitCodes::InvalidError));
        }
    }
}

CLI11_INLINE void App::_configure() {
    if (default_startup == startup_mode::enabled) {
        disabled_ = false;
    } else if (default_startup == startup_mode::disabled) {
        disabled_ = true;
    }
    for (const App_p& app : subcommands_) {
        if (app->has_automatic_name_) {
            app->name_.clear();
        }
        if (app->name_.empty()) {
            app->fallthrough_ = false;
            app->prefix_command_ = PrefixCommandMode::Off;
        }
        app->parent_ = this;
        app->_configure();
    }
}

CLI11_INLINE void App::run_callback(bool final_mode, bool suppress_final_callback) {
    pre_callback();
    if (!final_mode && parse_complete_callback_) {
        parse_complete_callback_();
    }
    for (App* subc : get_subcommands()) {
        if (subc->parent_ == this) {
            subc->run_callback(true, suppress_final_callback);
        }
    }
    for (auto& subc : subcommands_) {
        if (subc->name_.empty() && subc->count_all() > 0) {
            subc->run_callback(true, suppress_final_callback);
        }
    }

    if (final_callback_ && (parsed_ > 0) && (!suppress_final_callback)) {
        if (!name_.empty() || count_all() > 0 || parent_ == nullptr) {
            final_callback_();
        }
    }
}

CLI11_NODISCARD CLI11_INLINE bool App::_valid_subcommand(const std::string& current, bool ignore_used) const {
    if (require_subcommand_max_ != 0 && parsed_subcommands_.size() >= require_subcommand_max_ &&
        subcommand_fallthrough_) {
        return parent_ != nullptr && parent_->_valid_subcommand(current, ignore_used);
    }
    auto* com = _find_subcommand(current, true, ignore_used);
    if (com != nullptr) {
        return true;
    }
    if (subcommand_fallthrough_) {
        return parent_ != nullptr && parent_->_valid_subcommand(current, ignore_used);
    }
    return false;
}

CLI11_NODISCARD CLI11_INLINE detail::Classifier App::_recognize(const std::string& current,
                                                                bool ignore_used_subcommands) const {
    std::string dummy1, dummy2;

    if (current == "--")
        return detail::Classifier::POSITIONAL_MARK;
    if (_valid_subcommand(current, ignore_used_subcommands))
        return detail::Classifier::SUBCOMMAND;
    if (detail::split_long(current, dummy1, dummy2))
        return detail::Classifier::LONG;
    if (detail::split_short(current, dummy1, dummy2)) {
        if ((dummy1[0] >= '0' && dummy1[0] <= '9') ||
            (dummy1[0] == '.' && !dummy2.empty() && (dummy2[0] >= '0' && dummy2[0] <= '9'))) {
            if (get_option_no_throw(std::string{'-', dummy1[0]}) == nullptr) {
                return detail::Classifier::NONE;
            }
        }
        return detail::Classifier::SHORT;
    }
    if ((allow_windows_style_options_) && (detail::split_windows_style(current, dummy1, dummy2)))
        return detail::Classifier::WINDOWS_STYLE;
    if ((current == "++") && !name_.empty() && parent_ != nullptr)
        return detail::Classifier::SUBCOMMAND_TERMINATOR;
    auto dotloc = current.find_first_of('.');
    if (dotloc != std::string::npos) {
        auto* cm = _find_subcommand(current.substr(0, dotloc), true, ignore_used_subcommands);
        if (cm != nullptr) {
            auto res = cm->_recognize(current.substr(dotloc + 1), ignore_used_subcommands);
            if (res == detail::Classifier::SUBCOMMAND) {
                return res;
            }
        }
    }
    return detail::Classifier::NONE;
}

CLI11_INLINE bool App::_process_config_file(const std::string& config_file, bool throw_error) {
    auto path_result = detail::check_path(config_file.c_str());
    if (path_result == detail::path_type::file) {
        try {
            std::vector<ConfigItem> values = config_formatter_->from_file(config_file);
            _parse_config(values);
            return true;
        } catch (const FileError&) {
            if (throw_error) {
                throw;
            }
            return false;
        }
    } else if (throw_error) {
        throw FileError::Missing(config_file);
    } else {
        return false;
    }
}

CLI11_INLINE void App::_process_config_file() {
    if (config_ptr_ != nullptr) {
        bool config_required = config_ptr_->get_required();
        auto file_given = config_ptr_->count() > 0;
        if (!(file_given || config_ptr_->envname_.empty())) {
            std::string ename_string = detail::get_environment_value(config_ptr_->envname_);
            if (!ename_string.empty()) {
                config_ptr_->add_result(ename_string);
            }
        }
        config_ptr_->run_callback();

        auto config_files = config_ptr_->as<std::vector<std::string>>();
        bool files_used{file_given};
        if (config_files.empty() || config_files.front().empty()) {
            if (config_required) {
                throw FileError("config file is required but none was given");
            }
            return;
        }
        for (const auto& config_file : config_files) {
            if (_process_config_file(config_file, config_required || file_given)) {
                files_used = true;
            }
        }
        if (!files_used) {
            config_ptr_->clear();
            bool force = config_ptr_->force_callback_;
            config_ptr_->force_callback_ = false;
            config_ptr_->run_callback();
            config_ptr_->force_callback_ = force;
        }
    }
}

CLI11_INLINE void App::_process_env() {
    for (const Option_p& opt : options_) {
        if (opt->count() == 0 && !opt->envname_.empty()) {
            std::string ename_string = detail::get_environment_value(opt->envname_);
            if (!ename_string.empty()) {
                std::string result = ename_string;
                result = opt->_validate(result, 0);
                if (result.empty()) {
                    opt->add_result(ename_string);
                }
            }
        }
    }

    for (App_p& sub : subcommands_) {
        if (sub->get_name().empty() || (sub->count_all() > 0 && !sub->parse_complete_callback_)) {
            sub->_process_env();
        }
    }
}

CLI11_INLINE void App::_process_callbacks(CallbackPriority priority) {
    for (App_p& sub : subcommands_) {
        if (sub->get_name().empty() && sub->parse_complete_callback_) {
            if (sub->count_all() > 0) {
                sub->_process_callbacks(priority);
                if (priority == CallbackPriority::Normal) {
                    sub->run_callback();
                }
            }
        }
    }

    for (const Option_p& opt : options_) {
        if (opt->get_callback_priority() == priority) {
            if ((*opt) && !opt->get_callback_run()) {
                opt->run_callback();
            }
        }
    }
    for (App_p& sub : subcommands_) {
        if (!sub->parse_complete_callback_) {
            sub->_process_callbacks(priority);
        }
    }
}

CLI11_INLINE void App::_process_help_flags(CallbackPriority priority, bool trigger_help, bool trigger_all_help) const {
    const Option* help_ptr = get_help_ptr();
    const Option* help_all_ptr = get_help_all_ptr();

    if (help_ptr != nullptr && help_ptr->count() > 0 && help_ptr->get_callback_priority() == priority) {
        trigger_help = true;
    }
    if (help_all_ptr != nullptr && help_all_ptr->count() > 0 && help_all_ptr->get_callback_priority() == priority) {
        trigger_all_help = true;
    }

    if (!parsed_subcommands_.empty()) {
        for (const App* sub : parsed_subcommands_) {
            sub->_process_help_flags(priority, trigger_help, trigger_all_help);
        }

    } else if (trigger_all_help) {
        throw CallForAllHelp();
    } else if (trigger_help) {
        throw CallForHelp();
    }
}

CLI11_INLINE void App::_process_requirements() {
    bool excluded{false};
    std::string excluder;
    for (const auto& opt : exclude_options_) {
        if (opt->count() > 0) {
            excluded = true;
            excluder = opt->get_name();
        }
    }
    for (const auto& subc : exclude_subcommands_) {
        if (subc->count_all() > 0) {
            excluded = true;
            excluder = subc->get_display_name();
        }
    }
    if (excluded) {
        if (count_all() > 0) {
            throw ExcludesError(get_display_name(), excluder);
        }
        return;
    }

    bool missing_needed{false};
    std::string missing_need;
    for (const auto& opt : need_options_) {
        if (opt->count() == 0) {
            missing_needed = true;
            missing_need = opt->get_name();
        }
    }
    for (const auto& subc : need_subcommands_) {
        if (subc->count_all() == 0) {
            missing_needed = true;
            missing_need = subc->get_display_name();
        }
    }
    if (missing_needed) {
        if (count_all() > 0) {
            throw RequiresError(get_display_name(), missing_need);
        }
        return;
    }

    std::size_t used_options = 0;
    for (const Option_p& opt : options_) {
        if (opt->count() != 0) {
            ++used_options;
        }
        if (opt->get_required() && opt->count() == 0) {
            throw RequiredError(opt->get_name());
        }
        for (const Option* opt_req : opt->needs_)
            if (opt->count() > 0 && opt_req->count() == 0)
                throw RequiresError(opt->get_name(), opt_req->get_name());
        for (const Option* opt_ex : opt->excludes_)
            if (opt->count() > 0 && opt_ex->count() != 0)
                throw ExcludesError(opt->get_name(), opt_ex->get_name());
    }
    if (require_subcommand_min_ > 0) {
        auto selected_subcommands = get_subcommands();
        if (require_subcommand_min_ > selected_subcommands.size())
            throw RequiredError::Subcommand(require_subcommand_min_);
    }

    for (App_p& sub : subcommands_) {
        if (sub->disabled_)
            continue;
        if (sub->name_.empty() && sub->count_all() > 0) {
            ++used_options;
        }
    }

    if (require_option_min_ > used_options || (require_option_max_ > 0 && require_option_max_ < used_options)) {
        auto option_list = detail::join(options_, [this](const Option_p& ptr) {
            if (ptr.get() == help_ptr_ || ptr.get() == help_all_ptr_) {
                return std::string{};
            }
            return ptr->get_name(false, true);
        });

        auto subc_list = get_subcommands([](App* app) { return ((app->get_name().empty()) && (!app->disabled_)); });
        if (!subc_list.empty()) {
            option_list += "," + detail::join(subc_list, [](const App* app) { return app->get_display_name(); });
        }
        throw RequiredError::Option(require_option_min_, require_option_max_, used_options, option_list);
    }

    for (App_p& sub : subcommands_) {
        if (sub->disabled_)
            continue;
        if (sub->name_.empty() && sub->required_ == false) {
            if (sub->count_all() == 0) {
                if (require_option_min_ > 0 && require_option_min_ <= used_options) {
                    continue;
                }
                if (require_option_max_ > 0 && used_options >= require_option_min_) {
                    continue;
                }
            }
        }
        if (sub->count() > 0 || sub->name_.empty()) {
            sub->_process_requirements();
        }

        if (sub->required_ && sub->count_all() == 0) {
            throw(CLI::RequiredError(sub->get_display_name()));
        }
    }
}

CLI11_INLINE void App::_process() {
    _process_callbacks(CallbackPriority::FirstPreHelp);
    _process_help_flags(CallbackPriority::First);
    _process_callbacks(CallbackPriority::First);

    std::exception_ptr config_exception;
    try {
        _process_config_file();

        _process_env();
    } catch (const CLI::FileError&) {
        config_exception = std::current_exception();
    }
    _process_callbacks(CallbackPriority::PreRequirementsCheckPreHelp);
    _process_help_flags(CallbackPriority::PreRequirementsCheck);
    _process_callbacks(CallbackPriority::PreRequirementsCheck);

    _process_requirements();

    _process_callbacks(CallbackPriority::NormalPreHelp);
    _process_help_flags(CallbackPriority::Normal);
    _process_callbacks(CallbackPriority::Normal);

    if (config_exception) {
        std::rethrow_exception(config_exception);
    }

    _process_callbacks(CallbackPriority::LastPreHelp);
    _process_help_flags(CallbackPriority::Last);
    _process_callbacks(CallbackPriority::Last);
}

CLI11_INLINE void App::_process_extras() {
    if (allow_extras_ == ExtrasMode::Error && prefix_command_ == PrefixCommandMode::Off) {
        std::size_t num_left_over = remaining_size();
        if (num_left_over > 0) {
            throw ExtrasError(name_, remaining(false));
        }
    }
    if (allow_extras_ == ExtrasMode::Error && prefix_command_ == PrefixCommandMode::SeparatorOnly) {
        std::size_t num_left_over = remaining_size();
        if (num_left_over > 0) {
            if (remaining(false).front() != "--") {
                throw ExtrasError(name_, remaining(false));
            }
        }
    }
    for (App_p& sub : subcommands_) {
        if (sub->count() > 0)
            sub->_process_extras();
    }
}

CLI11_INLINE void App::increment_parsed() {
    ++parsed_;
    for (App_p& sub : subcommands_) {
        if (sub->get_name().empty())
            sub->increment_parsed();
    }
}

CLI11_INLINE void App::_parse(std::vector<std::string>& args) {
    increment_parsed();
    _trigger_pre_parse(args.size());
    bool positional_only = false;

    while (!args.empty()) {
        if (!_parse_single(args, positional_only)) {
            break;
        }
    }

    if (parent_ == nullptr) {
        _process();

        _process_extras();
        args = remaining_for_passthrough(false);
    } else if (parse_complete_callback_) {
        _process_callbacks(CallbackPriority::FirstPreHelp);
        _process_help_flags(CallbackPriority::First);
        _process_callbacks(CallbackPriority::First);
        _process_env();
        _process_callbacks(CallbackPriority::PreRequirementsCheckPreHelp);
        _process_help_flags(CallbackPriority::PreRequirementsCheck);
        _process_callbacks(CallbackPriority::PreRequirementsCheck);
        _process_requirements();
        _process_callbacks(CallbackPriority::NormalPreHelp);
        _process_help_flags(CallbackPriority::Normal);
        _process_callbacks(CallbackPriority::Normal);
        _process_callbacks(CallbackPriority::LastPreHelp);
        _process_help_flags(CallbackPriority::Last);
        _process_callbacks(CallbackPriority::Last);
        run_callback(false, true);
    }
}

CLI11_INLINE void App::_parse(std::vector<std::string>&& args) {
    increment_parsed();
    _trigger_pre_parse(args.size());
    bool positional_only = false;

    while (!args.empty()) {
        _parse_single(args, positional_only);
    }
    _process();

    _process_extras();
}

CLI11_INLINE void App::_parse_stream(std::istream& input) {
    auto values = config_formatter_->from_config(input);
    _parse_config(values);
    increment_parsed();
    _trigger_pre_parse(values.size());
    _process();

    _process_extras();
}

CLI11_INLINE void App::_parse_config(const std::vector<ConfigItem>& args) {
    for (const ConfigItem& item : args) {
        if (!_parse_single_config(item) && allow_config_extras_ == ConfigExtrasMode::Error)
            throw ConfigError::Extras(item.fullname());
    }
}

CLI11_INLINE bool App::_add_flag_like_result(Option* op, const ConfigItem& item,
                                             const std::vector<std::string>& inputs) {
    if (item.inputs.size() <= 1) {
        auto res = config_formatter_->to_flag(item);
        bool converted{false};
        if (op->get_disable_flag_override()) {
            auto val = detail::to_flag_value(res);
            if (val == 1) {
                res = op->get_flag_value(item.name, "{}");
                converted = true;
            }
        }

        if (!converted) {
            errno = 0;
            if (res != "{}" || op->get_expected_max() <= 1) {
                res = op->get_flag_value(item.name, res);
            }
        }

        op->add_result(res);
        return true;
    }
    if (static_cast<int>(inputs.size()) > op->get_items_expected_max() &&
        op->get_multi_option_policy() != MultiOptionPolicy::TakeAll &&
        op->get_multi_option_policy() != MultiOptionPolicy::Join) {
        if (op->get_items_expected_max() > 1) {
            throw ArgumentMismatch::AtMost(item.fullname(), op->get_items_expected_max(), inputs.size());
        }

        if (!op->get_disable_flag_override()) {
            throw ConversionError::TooManyInputsFlag(item.fullname());
        }
        for (const auto& res : inputs) {
            bool valid_value{false};
            if (op->default_flag_values_.empty()) {
                if (res == "true" || res == "false" || res == "1" || res == "0") {
                    valid_value = true;
                }
            } else {
                for (const auto& valid_res : op->default_flag_values_) {
                    if (valid_res.second == res) {
                        valid_value = true;
                        break;
                    }
                }
            }

            if (valid_value) {
                op->add_result(res);
            } else {
                throw InvalidError("invalid flag argument given");
            }
        }
        return true;
    }
    return false;
}

CLI11_INLINE bool App::_parse_single_config(const ConfigItem& item, std::size_t level) {
    if (level < item.parents.size()) {
        auto* subcom = get_subcommand_no_throw(item.parents.at(level));
        return (subcom != nullptr) ? subcom->_parse_single_config(item, level + 1) : false;
    }
    if (item.name == "++") {
        if (configurable_) {
            increment_parsed();
            _trigger_pre_parse(2);
            if (parent_ != nullptr) {
                parent_->parsed_subcommands_.push_back(this);
            }
        }
        return true;
    }
    if (item.name == "--") {
        if (configurable_ && parse_complete_callback_) {
            _process_callbacks(CallbackPriority::FirstPreHelp);
            _process_callbacks(CallbackPriority::First);
            _process_callbacks(CallbackPriority::PreRequirementsCheckPreHelp);
            _process_callbacks(CallbackPriority::PreRequirementsCheck);
            _process_requirements();
            _process_callbacks(CallbackPriority::NormalPreHelp);
            _process_callbacks(CallbackPriority::Normal);
            _process_callbacks(CallbackPriority::LastPreHelp);
            _process_callbacks(CallbackPriority::Last);
            run_callback();
        }
        return true;
    }
    Option* op = get_option_no_throw("--" + item.name);
    if (op == nullptr) {
        if (item.name.size() == 1) {
            op = get_option_no_throw("-" + item.name);
        }
        if (op == nullptr) {
            op = get_option_no_throw(item.name);
        }
    } else if (!op->get_configurable()) {
        if (item.name.size() == 1) {
            auto* testop = get_option_no_throw("-" + item.name);
            if (testop != nullptr && testop->get_configurable()) {
                op = testop;
            }
        }
    }
    if (op == nullptr || !op->get_configurable()) {
        std::string iname = item.name;
        auto options = get_options([iname](const CLI::Option* opt) {
            return (opt->get_configurable() &&
                    (opt->check_name(iname) || opt->check_lname(iname) || opt->check_sname(iname)));
        });
        if (!options.empty()) {
            op = options[0];
        }
    }
    if (op == nullptr) {
        if (get_allow_config_extras() == config_extras_mode::capture) {
            missing_.emplace_back(detail::Classifier::NONE, item.fullname());
            for (const auto& input : item.inputs) {
                missing_.emplace_back(detail::Classifier::NONE, input);
            }
        }
        return false;
    }

    if (!op->get_configurable()) {
        if (get_allow_config_extras() == config_extras_mode::ignore_all) {
            return false;
        }
        throw ConfigError::NotConfigurable(item.fullname());
    }
    if (op->empty()) {
        std::vector<std::string> buffer;
        bool useBuffer{false};
        if (item.multiline) {
            if (!op->get_inject_separator()) {
                buffer = item.inputs;
                buffer.erase(std::remove(buffer.begin(), buffer.end(), "%%"), buffer.end());
                useBuffer = true;
            }
        }
        const std::vector<std::string>& inputs = (useBuffer) ? buffer : item.inputs;
        if (op->get_expected_min() == 0) {
            if (_add_flag_like_result(op, item, inputs)) {
                return true;
            }
        }
        op->add_result(inputs);
        op->run_callback();
    }

    return true;
}

CLI11_INLINE bool App::_parse_single(std::vector<std::string>& args, bool& positional_only) {
    bool retval = true;
    detail::Classifier classifier = positional_only ? detail::Classifier::NONE : _recognize(args.back());
    switch (classifier) {
        case detail::Classifier::POSITIONAL_MARK:
            args.pop_back();
            positional_only = true;
            if (get_prefix_command()) {
                missing_.emplace_back(classifier, "--");
                while (!args.empty()) {
                    missing_.emplace_back(detail::Classifier::NONE, args.back());
                    args.pop_back();
                }
            } else if ((!_has_remaining_positionals()) && (parent_ != nullptr)) {
                retval = false;
            } else {
                _move_to_missing(classifier, "--");
            }
            break;
        case detail::Classifier::SUBCOMMAND_TERMINATOR:
            args.pop_back();
            retval = false;
            break;
        case detail::Classifier::SUBCOMMAND:
            retval = _parse_subcommand(args);
            break;
        case detail::Classifier::LONG:
        case detail::Classifier::SHORT:
        case detail::Classifier::WINDOWS_STYLE:
            retval = _parse_arg(args, classifier, false);
            break;
        case detail::Classifier::NONE:
            retval = _parse_positional(args, false);
            if (retval && positionals_at_end_) {
                positional_only = true;
            }
            break;
        default:
            throw HorribleError("unrecognized classifier (you should not see this!)");
    }
    return retval;
}

CLI11_NODISCARD CLI11_INLINE std::size_t App::_count_remaining_positionals(bool required_only) const {
    std::size_t retval = 0;
    for (const Option_p& opt : options_) {
        if (opt->get_positional() && (!required_only || opt->get_required())) {
            if (opt->get_items_expected_min() > 0 && static_cast<int>(opt->count()) < opt->get_items_expected_min()) {
                retval += static_cast<std::size_t>(opt->get_items_expected_min()) - opt->count();
            }
        }
    }
    return retval;
}

CLI11_NODISCARD CLI11_INLINE bool App::_has_remaining_positionals() const {
    for (const Option_p& opt : options_) {
        if (opt->get_positional() && ((static_cast<int>(opt->count()) < opt->get_items_expected_min()))) {
            return true;
        }
    }

    return false;
}

CLI11_INLINE bool App::_parse_positional(std::vector<std::string>& args, bool haltOnSubcommand) {
    const std::string& positional = args.back();
    Option* posOpt{nullptr};

    if (positionals_at_end_) {
        auto arg_rem = args.size();
        auto remreq = _count_remaining_positionals(true);
        if (arg_rem <= remreq) {
            for (const Option_p& opt : options_) {
                if (opt->get_positional() && opt->required_) {
                    if (static_cast<int>(opt->count()) < opt->get_items_expected_min()) {
                        if (validate_positionals_) {
                            std::string pos = positional;
                            pos = opt->_validate(pos, 0);
                            if (!pos.empty()) {
                                continue;
                            }
                        }
                        posOpt = opt.get();
                        break;
                    }
                }
            }
        }
    }
    if (posOpt == nullptr) {
        for (const Option_p& opt : options_) {
            if (opt->get_positional() &&
                (static_cast<int>(opt->count()) < opt->get_items_expected_max() || opt->get_allow_extra_args())) {
                if (validate_positionals_) {
                    std::string pos = positional;
                    pos = opt->_validate(pos, 0);
                    if (!pos.empty()) {
                        continue;
                    }
                }
                posOpt = opt.get();
                break;
            }
        }
    }
    if (posOpt != nullptr) {
        parse_order_.push_back(posOpt);
        if (posOpt->get_inject_separator()) {
            if (!posOpt->results().empty() && !posOpt->results().back().empty()) {
                posOpt->add_result(std::string{});
            }
        }
        results_t prev;
        if (posOpt->get_trigger_on_parse() && posOpt->current_option_state_ == Option::option_state::callback_run) {
            prev = posOpt->results();
            posOpt->clear();
        }
        if (posOpt->get_expected_min() == 0) {
            ConfigItem item;
            item.name = posOpt->pname_;
            item.inputs.push_back(positional);
            _add_flag_like_result(posOpt, item, item.inputs);
        } else {
            posOpt->add_result(positional);
        }

        if (posOpt->get_trigger_on_parse()) {
            if (!posOpt->empty()) {
                posOpt->run_callback();
            } else {
                if (!prev.empty()) {
                    posOpt->add_result(prev);
                }
            }
        }

        args.pop_back();
        return true;
    }

    for (auto& subc : subcommands_) {
        if ((subc->name_.empty()) && (!subc->disabled_)) {
            if (subc->_parse_positional(args, false)) {
                if (!subc->pre_parse_called_) {
                    subc->_trigger_pre_parse(args.size());
                }
                return true;
            }
        }
    }
    if (parent_ != nullptr && fallthrough_) {
        return _get_fallthrough_parent()->_parse_positional(args, static_cast<bool>(parse_complete_callback_));
    }
    auto* com = _find_subcommand(args.back(), true, false);
    if (com != nullptr && (require_subcommand_max_ == 0 || require_subcommand_max_ > parsed_subcommands_.size())) {
        if (haltOnSubcommand) {
            return false;
        }
        args.pop_back();
        com->_parse(args);
        return true;
    }
    if (subcommand_fallthrough_) {
        auto* parent_app = (parent_ != nullptr) ? _get_fallthrough_parent() : this;
        com = parent_app->_find_subcommand(args.back(), true, false);
        if (com != nullptr && (com->parent_->require_subcommand_max_ == 0 ||
                               com->parent_->require_subcommand_max_ > com->parent_->parsed_subcommands_.size())) {
            return false;
        }
    }
    if (positionals_at_end_) {
        std::vector<std::string> rargs;
        rargs.resize(args.size());
        std::reverse_copy(args.begin(), args.end(), rargs.begin());
        throw CLI::ExtrasError(name_, rargs);
    }
    if (parent_ != nullptr && name_.empty()) {
        return false;
    }
    _move_to_missing(detail::Classifier::NONE, positional);
    args.pop_back();
    if (get_prefix_command()) {
        while (!args.empty()) {
            missing_.emplace_back(detail::Classifier::NONE, args.back());
            args.pop_back();
        }
    }

    return true;
}

CLI11_NODISCARD CLI11_INLINE App* App::_find_subcommand(const std::string& subc_name, bool ignore_disabled,
                                                        bool ignore_used) const noexcept {
    App* bcom{nullptr};
    for (const App_p& com : subcommands_) {
        if (com->disabled_ && ignore_disabled)
            continue;
        if (com->get_name().empty()) {
            auto* subc = com->_find_subcommand(subc_name, ignore_disabled, ignore_used);
            if (subc != nullptr) {
                if (bcom != nullptr) {
                    return nullptr;
                }
                bcom = subc;
                if (!allow_prefix_matching_) {
                    return bcom;
                }
            }
        }
        auto res = com->check_name_detail(subc_name);
        if (res != NameMatch::none) {
            if ((!*com) || !ignore_used) {
                if (res == NameMatch::exact) {
                    return com.get();
                }
                if (bcom != nullptr) {
                    return nullptr;
                }
                bcom = com.get();
                if (!allow_prefix_matching_) {
                    return bcom;
                }
            }
        }
    }
    return bcom;
}

CLI11_INLINE bool App::_parse_subcommand(std::vector<std::string>& args) {
    if (_count_remaining_positionals(true) > 0) {
        _parse_positional(args, false);
        return true;
    }
    auto* com = _find_subcommand(args.back(), true, true);
    if (com == nullptr) {
        auto dotloc = args.back().find_first_of('.');
        if (dotloc != std::string::npos) {
            com = _find_subcommand(args.back().substr(0, dotloc), true, true);
            if (com != nullptr) {
                args.back() = args.back().substr(dotloc + 1);
                args.push_back(com->get_display_name());
            }
        }
    }
    if (com != nullptr) {
        args.pop_back();
        if (!com->silent_) {
            parsed_subcommands_.push_back(com);
        }
        com->_parse(args);
        auto* parent_app = com->parent_;
        while (parent_app != this) {
            parent_app->_trigger_pre_parse(args.size());
            if (!com->silent_) {
                parent_app->parsed_subcommands_.push_back(com);
            }
            parent_app = parent_app->parent_;
        }
        return true;
    }

    if (parent_ == nullptr)
        throw HorribleError("Subcommand " + args.back() + " missing");
    return false;
}

CLI11_INLINE bool App::_parse_arg(std::vector<std::string>& args, detail::Classifier current_type,
                                  bool local_processing_only) {
    std::string current = args.back();

    std::string arg_name;
    std::string value;
    std::string rest;

    switch (current_type) {
        case detail::Classifier::LONG:
            if (!detail::split_long(current, arg_name, value))
                throw HorribleError("Long parsed but missing (you should not see this):" + args.back());
            break;
        case detail::Classifier::SHORT:
            if (!detail::split_short(current, arg_name, rest))
                throw HorribleError("Short parsed but missing! You should not see this");
            break;
        case detail::Classifier::WINDOWS_STYLE:
            if (!detail::split_windows_style(current, arg_name, value))
                throw HorribleError("windows option parsed but missing! You should not see this");
            break;
        case detail::Classifier::SUBCOMMAND:
        case detail::Classifier::SUBCOMMAND_TERMINATOR:
        case detail::Classifier::POSITIONAL_MARK:
        case detail::Classifier::NONE:
        default:
            throw HorribleError("parsing got called with invalid option! You should not see this");
    }

    auto op_ptr = std::find_if(std::begin(options_), std::end(options_), [arg_name, current_type](const Option_p& opt) {
        if (current_type == detail::Classifier::LONG)
            return opt->check_lname(arg_name);
        if (current_type == detail::Classifier::SHORT)
            return opt->check_sname(arg_name);
        return opt->check_lname(arg_name) || opt->check_sname(arg_name);
    });

    while (op_ptr == std::end(options_)) {
        for (auto& subc : subcommands_) {
            if (subc->name_.empty() && !subc->disabled_) {
                if (subc->_parse_arg(args, current_type, local_processing_only)) {
                    if (!subc->pre_parse_called_) {
                        subc->_trigger_pre_parse(args.size());
                    }
                    return true;
                }
            }
        }
        if (allow_non_standard_options_ && current_type == detail::Classifier::SHORT && current.size() > 2) {
            std::string narg_name;
            std::string nvalue;
            detail::split_long(std::string{'-'} + current, narg_name, nvalue);
            op_ptr = std::find_if(std::begin(options_), std::end(options_),
                                  [narg_name](const Option_p& opt) { return opt->check_sname(narg_name); });
            if (op_ptr != std::end(options_)) {
                arg_name = narg_name;
                value = nvalue;
                rest.clear();
                break;
            }
        }

        if (parent_ != nullptr && name_.empty()) {
            return false;
        }

        auto dotloc = arg_name.find_first_of('.', 1);
        if (dotloc != std::string::npos && dotloc < arg_name.size() - 1) {
            auto* sub = _find_subcommand(arg_name.substr(0, dotloc), true, false);
            if (sub != nullptr) {
                std::string v = args.back();
                args.pop_back();
                arg_name = arg_name.substr(dotloc + 1);
                if (arg_name.size() > 1) {
                    args.push_back(std::string("--") + v.substr(dotloc + 3));
                    current_type = detail::Classifier::LONG;
                } else {
                    auto nval = v.substr(dotloc + 2);
                    nval.front() = '-';
                    if (nval.size() > 2) {
                        args.push_back(nval.substr(3));
                        nval.resize(2);
                    }
                    args.push_back(nval);
                    current_type = detail::Classifier::SHORT;
                }
                std::string dummy1, dummy2;
                bool val = false;
                if ((current_type == detail::Classifier::SHORT && detail::valid_first_char(args.back()[1])) ||
                    detail::split_long(args.back(), dummy1, dummy2)) {
                    val = sub->_parse_arg(args, current_type, true);
                }

                if (val) {
                    if (!sub->silent_) {
                        parsed_subcommands_.push_back(sub);
                    }
                    increment_parsed();
                    _trigger_pre_parse(args.size());
                    if (sub->parse_complete_callback_) {
                        sub->_process_callbacks(CallbackPriority::FirstPreHelp);
                        sub->_process_help_flags(CallbackPriority::First);
                        sub->_process_callbacks(CallbackPriority::First);
                        sub->_process_env();
                        sub->_process_callbacks(CallbackPriority::PreRequirementsCheckPreHelp);
                        sub->_process_help_flags(CallbackPriority::PreRequirementsCheck);
                        sub->_process_callbacks(CallbackPriority::PreRequirementsCheck);
                        sub->_process_requirements();
                        sub->_process_callbacks(CallbackPriority::NormalPreHelp);
                        sub->_process_help_flags(CallbackPriority::Normal);
                        sub->_process_callbacks(CallbackPriority::Normal);
                        sub->_process_callbacks(CallbackPriority::LastPreHelp);
                        sub->_process_help_flags(CallbackPriority::Last);
                        sub->_process_callbacks(CallbackPriority::Last);
                        sub->run_callback(false, true);
                    }
                    return true;
                }
                args.pop_back();
                args.push_back(v);
            }
        }
        if (local_processing_only) {
            return false;
        }
        if (parent_ != nullptr && fallthrough_)
            return _get_fallthrough_parent()->_parse_arg(args, current_type, false);

        args.pop_back();
        _move_to_missing(current_type, current);
        if (get_prefix_command_mode() == PrefixCommandMode::On) {
            while (!args.empty()) {
                missing_.emplace_back(detail::Classifier::NONE, args.back());
                args.pop_back();
            }
        } else if (allow_extras_ == ExtrasMode::AssumeSingleArgument) {
            if (!args.empty() && _recognize(args.back(), false) == detail::Classifier::NONE) {
                _move_to_missing(detail::Classifier::NONE, args.back());
                args.pop_back();
            }
        } else if (allow_extras_ == ExtrasMode::AssumeMultipleArguments) {
            while (!args.empty() && _recognize(args.back(), false) == detail::Classifier::NONE) {
                _move_to_missing(detail::Classifier::NONE, args.back());
                args.pop_back();
            }
        }
        return true;
    }

    args.pop_back();

    Option_p& op = *op_ptr;
    if (op->get_inject_separator()) {
        if (!op->results().empty() && !op->results().back().empty()) {
            op->add_result(std::string{});
        }
    }
    if (op->get_trigger_on_parse() && op->current_option_state_ == Option::option_state::callback_run) {
        op->clear();
    }
    int min_num = (std::min)(op->get_type_size_min(), op->get_items_expected_min());
    int max_num = op->get_items_expected_max();
    if (max_num >= detail::expected_max_vector_size / 16 && !op->get_allow_extra_args()) {
        auto tmax = op->get_type_size_max();
        max_num = detail::checked_multiply(tmax, op->get_expected_min()) ? tmax : detail::expected_max_vector_size;
    }
    int collected = 0;
    int result_count = 0;
    if (max_num == 0) {
        auto res = op->get_flag_value(arg_name, value);
        op->add_result(res);
        parse_order_.push_back(op.get());
    } else if (!value.empty()) {
        op->add_result(value, result_count);
        parse_order_.push_back(op.get());
        collected += result_count;
    } else if (!rest.empty()) {
        op->add_result(rest, result_count);
        parse_order_.push_back(op.get());
        rest = "";
        collected += result_count;
    }

    while (min_num > collected && !args.empty()) {
        std::string current_ = args.back();
        args.pop_back();
        op->add_result(current_, result_count);
        parse_order_.push_back(op.get());
        collected += result_count;
    }

    if (min_num > collected) {
        throw ArgumentMismatch::TypedAtLeast(op->get_name(), min_num, op->get_type_name());
    }

    if (max_num > collected || op->get_allow_extra_args()) {
        auto remreqpos = _count_remaining_positionals(true);
        while ((collected < max_num || op->get_allow_extra_args()) && !args.empty() &&
               _recognize(args.back(), false) == detail::Classifier::NONE) {
            if (remreqpos >= args.size()) {
                break;
            }
            if (validate_optional_arguments_) {
                std::string arg = args.back();
                arg = op->_validate(arg, 0);
                if (!arg.empty()) {
                    break;
                }
            }
            op->add_result(args.back(), result_count);
            parse_order_.push_back(op.get());
            args.pop_back();
            collected += result_count;
        }

        if (!args.empty() && _recognize(args.back()) == detail::Classifier::POSITIONAL_MARK)
            args.pop_back();
        if (min_num == 0 && max_num > 0 && collected == 0) {
            auto res = op->get_flag_value(arg_name, std::string{});
            op->add_result(res);
            parse_order_.push_back(op.get());
        }
    }
    if (min_num > 0 && (collected % op->get_type_size_max()) != 0) {
        if (op->get_type_size_max() != op->get_type_size_min()) {
            op->add_result(std::string{});
        } else {
            throw ArgumentMismatch::PartialType(op->get_name(), op->get_type_size_min(), op->get_type_name());
        }
    }
    if (op->get_trigger_on_parse()) {
        op->run_callback();
    }
    if (!rest.empty()) {
        rest = "-" + rest;
        args.push_back(rest);
    }
    return true;
}

CLI11_INLINE void App::_trigger_pre_parse(std::size_t remaining_args) {
    if (!pre_parse_called_) {
        pre_parse_called_ = true;
        if (pre_parse_callback_) {
            pre_parse_callback_(remaining_args);
        }
    } else if (immediate_callback_) {
        if (!name_.empty()) {
            auto pcnt = parsed_;
            missing_t extras = std::move(missing_);
            clear();
            parsed_ = pcnt;
            pre_parse_called_ = true;
            missing_ = std::move(extras);
        }
    }
}

CLI11_INLINE App* App::_get_fallthrough_parent() noexcept {
    if (parent_ == nullptr) {
        return nullptr;
    }
    auto* fallthrough_parent = parent_;
    while ((fallthrough_parent->parent_ != nullptr) && (fallthrough_parent->get_name().empty())) {
        fallthrough_parent = fallthrough_parent->parent_;
    }
    return fallthrough_parent;
}

CLI11_INLINE const App* App::_get_fallthrough_parent() const noexcept {
    if (parent_ == nullptr) {
        return nullptr;
    }
    const auto* fallthrough_parent = parent_;
    while ((fallthrough_parent->parent_ != nullptr) && (fallthrough_parent->get_name().empty())) {
        fallthrough_parent = fallthrough_parent->parent_;
    }
    return fallthrough_parent;
}

CLI11_NODISCARD CLI11_INLINE const std::string& App::_compare_subcommand_names(const App& subcom,
                                                                               const App& base) const {
    static const std::string estring;
    if (subcom.disabled_) {
        return estring;
    }
    for (const auto& subc : base.subcommands_) {
        if (subc.get() != &subcom) {
            if (subc->disabled_) {
                continue;
            }
            if (!subcom.get_name().empty()) {
                if (subc->check_name(subcom.get_name())) {
                    return subcom.get_name();
                }
            }
            if (!subc->get_name().empty()) {
                if (subcom.check_name(subc->get_name())) {
                    return subc->get_name();
                }
            }
            for (const auto& les : subcom.aliases_) {
                if (subc->check_name(les)) {
                    return les;
                }
            }
            for (const auto& les : subc->aliases_) {
                if (subcom.check_name(les)) {
                    return les;
                }
            }
            if (subc->get_name().empty()) {
                const auto& cmpres = _compare_subcommand_names(subcom, *subc);
                if (!cmpres.empty()) {
                    return cmpres;
                }
            }
            if (subcom.get_name().empty()) {
                const auto& cmpres = _compare_subcommand_names(*subc, subcom);
                if (!cmpres.empty()) {
                    return cmpres;
                }
            }
        }
    }
    return estring;
}

inline bool capture_extras(ExtrasMode mode) {
    return mode == ExtrasMode::Capture || mode == ExtrasMode::AssumeSingleArgument ||
           mode == ExtrasMode::AssumeMultipleArguments;
}
CLI11_INLINE void App::_move_to_missing(detail::Classifier val_type, const std::string& val) {
    if (allow_extras_ == ExtrasMode::ErrorImmediately) {
        throw ExtrasError(name_, std::vector<std::string>{val});
    }
    if (capture_extras(allow_extras_) || subcommands_.empty() || get_prefix_command()) {
        if (allow_extras_ != ExtrasMode::Ignore) {
            missing_.emplace_back(val_type, val);
        }
        return;
    }
    for (auto& subc : subcommands_) {
        if (subc->name_.empty() && capture_extras(subc->allow_extras_)) {
            subc->missing_.emplace_back(val_type, val);
            return;
        }
    }
    if (allow_extras_ != ExtrasMode::Ignore) {
        missing_.emplace_back(val_type, val);
    }
}

CLI11_INLINE void App::_move_option(Option* opt, App* app) {
    if (opt == nullptr) {
        throw OptionNotFound("the option is NULL");
    }
    bool found = false;
    for (auto& subc : subcommands_) {
        if (app == subc.get()) {
            found = true;
        }
    }
    if (!found) {
        throw OptionNotFound("The Given app is not a subcommand");
    }

    if ((help_ptr_ == opt) || (help_all_ptr_ == opt))
        throw OptionAlreadyAdded("cannot move help options");

    if (config_ptr_ == opt)
        throw OptionAlreadyAdded("cannot move config file options");

    auto iterator =
        std::find_if(std::begin(options_), std::end(options_), [opt](const Option_p& v) { return v.get() == opt; });
    if (iterator != std::end(options_)) {
        const auto& opt_p = *iterator;
        if (std::find_if(std::begin(app->options_), std::end(app->options_),
                         [&opt_p](const Option_p& v) { return (*v == *opt_p); }) == std::end(app->options_)) {
            app->options_.push_back(std::move(*iterator));
            options_.erase(iterator);
        } else {
            throw OptionAlreadyAdded("option was not located: " + opt->get_name());
        }
    } else {
        throw OptionNotFound("could not locate the given Option");
    }
}

CLI11_INLINE void TriggerOn(App* trigger_app, App* app_to_enable) {
    app_to_enable->enabled_by_default(false);
    app_to_enable->disabled_by_default();
    trigger_app->preparse_callback([app_to_enable](std::size_t) { app_to_enable->disabled(false); });
}

CLI11_INLINE void TriggerOn(App* trigger_app, std::vector<App*> apps_to_enable) {
    for (auto& app : apps_to_enable) {
        app->enabled_by_default(false);
        app->disabled_by_default();
    }

    trigger_app->preparse_callback([apps_to_enable](std::size_t) {
        for (const auto& app : apps_to_enable) {
            app->disabled(false);
        }
    });
}

CLI11_INLINE void TriggerOff(App* trigger_app, App* app_to_enable) {
    app_to_enable->disabled_by_default(false);
    app_to_enable->enabled_by_default();
    trigger_app->preparse_callback([app_to_enable](std::size_t) { app_to_enable->disabled(); });
}

CLI11_INLINE void TriggerOff(App* trigger_app, std::vector<App*> apps_to_enable) {
    for (auto& app : apps_to_enable) {
        app->disabled_by_default(false);
        app->enabled_by_default();
    }

    trigger_app->preparse_callback([apps_to_enable](std::size_t) {
        for (const auto& app : apps_to_enable) {
            app->disabled();
        }
    });
}

CLI11_INLINE void deprecate_option(Option* opt, const std::string& replacement) {
    Validator deprecate_warning{[opt, replacement](std::string&) {
                                    std::cout << opt->get_name() << " is deprecated please use '" << replacement
                                              << "' instead\n";
                                    return std::string();
                                },
                                "DEPRECATED"};
    deprecate_warning.application_index(0);
    opt->check(deprecate_warning);
    if (!replacement.empty()) {
        opt->description(opt->get_description() + " DEPRECATED: please use '" + replacement + "' instead");
    }
}

CLI11_INLINE void retire_option(App* app, Option* opt) {
    App temp;
    auto* option_copy = temp.add_option(opt->get_name(false, true))
                            ->type_size(opt->get_type_size_min(), opt->get_type_size_max())
                            ->expected(opt->get_expected_min(), opt->get_expected_max())
                            ->allow_extra_args(opt->get_allow_extra_args());

    app->remove_option(opt);
    auto* opt2 = app->add_option(option_copy->get_name(false, true), "option has been retired and has no effect");
    opt2->type_name("RETIRED")
        ->default_str("RETIRED")
        ->type_size(option_copy->get_type_size_min(), option_copy->get_type_size_max())
        ->expected(option_copy->get_expected_min(), option_copy->get_expected_max())
        ->allow_extra_args(option_copy->get_allow_extra_args());

    Validator retired_warning{[opt2](std::string&) {
                                  std::cout << "WARNING " << opt2->get_name() << " is retired and has no effect\n";
                                  return std::string();
                              },
                              ""};
    retired_warning.application_index(0);
    opt2->check(retired_warning);
}

CLI11_INLINE void retire_option(App& app, Option* opt) {
    retire_option(&app, opt);
}

CLI11_INLINE void retire_option(App* app, const std::string& option_name) {
    auto* opt = app->get_option_no_throw(option_name);
    if (opt != nullptr) {
        retire_option(app, opt);
        return;
    }
    auto* opt2 = app->add_option(option_name, "option has been retired and has no effect")
                     ->type_name("RETIRED")
                     ->expected(0, 1)
                     ->default_str("RETIRED");
    Validator retired_warning{[opt2](std::string&) {
                                  std::cout << "WARNING " << opt2->get_name() << " is retired and has no effect\n";
                                  return std::string();
                              },
                              ""};
    retired_warning.application_index(0);
    opt2->check(retired_warning);
}

CLI11_INLINE void retire_option(App& app, const std::string& option_name) {
    retire_option(&app, option_name);
}

namespace FailureMessage {

CLI11_INLINE std::string simple(const App* app, const Error& e) {
    std::string header = std::string(e.what()) + "\n";
    std::vector<std::string> names;

    if (app->get_help_ptr() != nullptr)
        names.push_back(app->get_help_ptr()->get_name());

    if (app->get_help_all_ptr() != nullptr)
        names.push_back(app->get_help_all_ptr()->get_name());

    if (!names.empty())
        header += "Run with " + detail::join(names, " or ") + " for more information.\n";

    return header;
}

CLI11_INLINE std::string help(const App* app, const Error& e) {
    std::string header = std::string("ERROR: ") + e.get_name() + ": " + e.what() + "\n";
    header += app->help();
    return header;
}

}  

namespace detail {

std::string convert_arg_for_ini(const std::string& arg, char stringQuote = '"', char literalQuote = '\'',
                                bool disable_multi_line = false);

std::string ini_join(const std::vector<std::string>& args, char sepChar = ',', char arrayStart = '[',
                     char arrayEnd = ']', char stringQuote = '"', char literalQuote = '\'');

void clean_name_string(std::string& name, const std::string& keyChars);

std::vector<std::string> generate_parents(const std::string& section, std::string& name, char parentSeparator);

void checkParentSegments(std::vector<ConfigItem>& output, const std::string& currentSection, char parentSeparator);
}  

static constexpr auto multiline_literal_quote = R"(''')";
static constexpr auto multiline_string_quote = R"(""")";

namespace detail {

CLI11_INLINE bool is_printable(const std::string& test_string) {
    return std::all_of(test_string.begin(), test_string.end(),
                       [](char x) { return (isprint(static_cast<unsigned char>(x)) != 0 || x == '\n' || x == '\t'); });
}

CLI11_INLINE std::string convert_arg_for_ini(const std::string& arg, char stringQuote, char literalQuote,
                                             bool disable_multi_line) {
    if (arg.empty()) {
        return std::string(2, stringQuote);
    }
    if (arg == "true" || arg == "false" || arg == "nan" || arg == "inf") {
        return arg;
    }
    if (arg.compare(0, 2, "0x") != 0 && arg.compare(0, 2, "0X") != 0) {
        using CLI::detail::lexical_cast;
        double val = 0.0;
        if (lexical_cast(arg, val)) {
            if (arg.find_first_not_of("0123456789.-+eE") == std::string::npos) {
                return arg;
            }
        }
    }
    if (arg.size() == 1) {
        if (isprint(static_cast<unsigned char>(arg.front())) == 0) {
            return binary_escape_string(arg);
        }
        if (arg == "'") {
            return std::string(1, stringQuote) + "'" + stringQuote;
        }
        return std::string(1, literalQuote) + arg + literalQuote;
    }
    if (arg.front() == '0') {
        if (arg[1] == 'x') {
            if (std::all_of(arg.begin() + 2, arg.end(), [](char x) {
                    return (x >= '0' && x <= '9') || (x >= 'A' && x <= 'F') || (x >= 'a' && x <= 'f');
                })) {
                return arg;
            }
        } else if (arg[1] == 'o') {
            if (std::all_of(arg.begin() + 2, arg.end(), [](char x) { return (x >= '0' && x <= '7'); })) {
                return arg;
            }
        } else if (arg[1] == 'b') {
            if (std::all_of(arg.begin() + 2, arg.end(), [](char x) { return (x == '0' || x == '1'); })) {
                return arg;
            }
        }
    }
    if (!is_printable(arg)) {
        return binary_escape_string(arg);
    }
    if (detail::has_escapable_character(arg)) {
        if (arg.size() > 100 && !disable_multi_line) {
            if (arg.find(multiline_literal_quote) != std::string::npos) {
                return binary_escape_string(arg, true);
            }
            std::string return_string{multiline_literal_quote};
            return_string.reserve(7 + arg.size());
            if (arg.front() == '\n' || arg.front() == '\r') {
                return_string.push_back('\n');
            }
            return_string.append(arg);
            if (arg.back() == '\n' || arg.back() == '\r') {
                return_string.push_back('\n');
            }
            return_string.append(multiline_literal_quote, 3);
            return return_string;
        }
        return std::string(1, stringQuote) + detail::add_escaped_characters(arg) + stringQuote;
    }
    return std::string(1, stringQuote) + arg + stringQuote;
}

CLI11_INLINE std::string ini_join(const std::vector<std::string>& args, char sepChar, char arrayStart, char arrayEnd,
                                  char stringQuote, char literalQuote) {
    bool disable_multi_line{false};
    std::string joined;
    if (args.size() > 1 && arrayStart != '\0') {
        joined.push_back(arrayStart);
        disable_multi_line = true;
    }
    std::size_t start = 0;
    for (const auto& arg : args) {
        if (start++ > 0) {
            joined.push_back(sepChar);
            if (!std::isspace<char>(sepChar, std::locale())) {
                joined.push_back(' ');
            }
        }
        joined.append(convert_arg_for_ini(arg, stringQuote, literalQuote, disable_multi_line));
    }
    if (args.size() > 1 && arrayEnd != '\0') {
        joined.push_back(arrayEnd);
    }
    return joined;
}

CLI11_INLINE std::vector<std::string> generate_parents(const std::string& section, std::string& name,
                                                       char parentSeparator) {
    std::vector<std::string> parents;
    if (detail::to_lower(section) != "default") {
        if (section.find(parentSeparator) != std::string::npos) {
            parents = detail::split_up(section, parentSeparator);
        } else {
            parents = {section};
        }
    }
    if (name.find(parentSeparator) != std::string::npos) {
        std::vector<std::string> plist = detail::split_up(name, parentSeparator);
        name = plist.back();
        plist.pop_back();
        parents.insert(parents.end(), plist.begin(), plist.end());
    }
    try {
        detail::remove_quotes(parents);
    } catch (const std::invalid_argument& iarg) {
        throw CLI::ParseError(iarg.what(), CLI::ExitCodes::InvalidError);
    }
    return parents;
}

CLI11_INLINE void checkParentSegments(std::vector<ConfigItem>& output, const std::string& currentSection,
                                      char parentSeparator) {
    std::string estring;
    auto parents = detail::generate_parents(currentSection, estring, parentSeparator);
    if (!output.empty() && output.back().name == "--") {
        std::size_t msize = (parents.size() > 1U) ? parents.size() : 2;
        while (output.back().parents.size() >= msize) {
            output.push_back(output.back());
            output.back().parents.pop_back();
        }

        if (parents.size() > 1) {
            std::size_t common = 0;
            std::size_t mpair = (std::min)(output.back().parents.size(), parents.size() - 1);
            for (std::size_t ii = 0; ii < mpair; ++ii) {
                if (output.back().parents[ii] != parents[ii]) {
                    break;
                }
                ++common;
            }
            if (common == mpair) {
                output.pop_back();
            } else {
                while (output.back().parents.size() > common + 1) {
                    output.push_back(output.back());
                    output.back().parents.pop_back();
                }
            }
            for (std::size_t ii = common; ii < parents.size() - 1; ++ii) {
                output.emplace_back();
                output.back().parents.assign(parents.begin(), parents.begin() + static_cast<std::ptrdiff_t>(ii) + 1);
                output.back().name = "++";
            }
        }
    } else if (parents.size() > 1) {
        for (std::size_t ii = 0; ii < parents.size() - 1; ++ii) {
            output.emplace_back();
            output.back().parents.assign(parents.begin(), parents.begin() + static_cast<std::ptrdiff_t>(ii) + 1);
            output.back().name = "++";
        }
    }

    output.emplace_back();
    output.back().parents = std::move(parents);
    output.back().name = "++";
}

CLI11_INLINE bool hasMLString(std::string const& fullString, char check) {
    if (fullString.length() < 3) {
        return false;
    }
    auto it = fullString.rbegin();
    return (*it == check) && (*(it + 1) == check) && (*(it + 2) == check);
}

inline auto find_matching_config(std::vector<ConfigItem>& items, const std::vector<std::string>& parents,
                                 const std::string& name, bool fullSearch) -> decltype(items.begin()) {
    if (items.empty()) {
        return items.end();
    }
    auto search = items.end() - 1;
    do {
        if (search->parents == parents && search->name == name) {
            return search;
        }
        if (search == items.begin()) {
            break;
        }
        --search;
    } while (fullSearch);
    return items.end();
}
}  

inline std::vector<ConfigItem> ConfigBase::from_config(std::istream& input) const {
    std::string line;
    std::string buffer;
    std::string currentSection = "default";
    std::string previousSection = "default";
    std::vector<ConfigItem> output;
    bool isDefaultArray = (arrayStart == '[' && arrayEnd == ']' && arraySeparator == ',');
    bool isINIArray = (arrayStart == '\0' || arrayStart == ' ') && arrayStart == arrayEnd;
    bool inSection{false};
    bool inMLineComment{false};
    bool inMLineValue{false};

    char aStart = (isINIArray) ? '[' : arrayStart;
    char aEnd = (isINIArray) ? ']' : arrayEnd;
    char aSep = (isINIArray && arraySeparator == ' ') ? ',' : arraySeparator;
    int currentSectionIndex{0};

    std::string line_sep_chars{parentSeparatorChar, commentChar, valueDelimiter};
    while (getline(input, buffer)) {
        std::vector<std::string> items_buffer;
        std::string name;
        line = detail::trim_copy(buffer);
        std::size_t len = line.length();
        if (len < 3) {
            continue;
        }
        if (line.compare(0, 3, multiline_string_quote) == 0 || line.compare(0, 3, multiline_literal_quote) == 0) {
            inMLineComment = true;
            auto cchar = line.front();
            while (inMLineComment) {
                if (getline(input, line)) {
                    detail::trim(line);
                } else {
                    break;
                }
                if (detail::hasMLString(line, cchar)) {
                    inMLineComment = false;
                }
            }
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            if (currentSection != "default") {
                output.emplace_back();
                output.back().parents = detail::generate_parents(currentSection, name, parentSeparatorChar);
                output.back().name = "--";
            }
            currentSection = line.substr(1, len - 2);
            if (currentSection.size() > 1 && currentSection.front() == '[' && currentSection.back() == ']') {
                currentSection = currentSection.substr(1, currentSection.size() - 2);
            }
            if (detail::to_lower(currentSection) == "default") {
                currentSection = "default";
            } else {
                detail::checkParentSegments(output, currentSection, parentSeparatorChar);
            }
            inSection = false;
            if (currentSection == previousSection) {
                ++currentSectionIndex;
            } else {
                currentSectionIndex = 0;
                previousSection = currentSection;
            }
            continue;
        }

        if (line.front() == ';' || line.front() == '#' || line.front() == commentChar) {
            continue;
        }
        std::size_t search_start = 0;
        if (line.find_first_of("\"'`") != std::string::npos) {
            while (search_start < line.size()) {
                auto test_char = line[search_start];
                if (test_char == '\"' || test_char == '\'' || test_char == '`') {
                    search_start = detail::close_sequence(line, search_start, line[search_start]);
                    ++search_start;
                } else if (test_char == valueDelimiter || test_char == commentChar) {
                    --search_start;
                    break;
                } else if (test_char == ' ' || test_char == '\t' || test_char == parentSeparatorChar) {
                    ++search_start;
                } else {
                    search_start = line.find_first_of(line_sep_chars, search_start);
                }
            }
        }
        auto delimiter_pos = line.find_first_of(valueDelimiter, search_start + 1);
        auto comment_pos = line.find_first_of(commentChar, search_start);
        if (comment_pos < delimiter_pos) {
            delimiter_pos = std::string::npos;
        }
        if (delimiter_pos != std::string::npos) {
            name = detail::trim_copy(line.substr(0, delimiter_pos));
            std::string item = detail::trim_copy(line.substr(delimiter_pos + 1, std::string::npos));
            bool mlquote =
                (item.compare(0, 3, multiline_literal_quote) == 0 || item.compare(0, 3, multiline_string_quote) == 0);
            if (!mlquote && comment_pos != std::string::npos) {
                auto citems = detail::split_up(item, commentChar);
                item = detail::trim_copy(citems.front());
            }
            if (mlquote) {
                auto keyChar = item.front();
                item = buffer.substr(delimiter_pos + 1, std::string::npos);
                detail::ltrim(item);
                item.erase(0, 3);
                inMLineValue = true;
                bool lineExtension{false};
                bool firstLine = true;
                if (!item.empty() && item.back() == '\\' && keyChar == '\"') {
                    item.pop_back();
                    lineExtension = true;
                } else if (detail::hasMLString(item, keyChar)) {
                    item.pop_back();
                    item.pop_back();
                    item.pop_back();
                    if (keyChar == '\"') {
                        try {
                            item = detail::remove_escaped_characters(item);
                        } catch (const std::invalid_argument& iarg) {
                            throw CLI::ParseError(iarg.what(), CLI::ExitCodes::InvalidError);
                        }
                    }
                    inMLineValue = false;
                }
                while (inMLineValue) {
                    std::string l2;
                    if (!std::getline(input, l2)) {
                        break;
                    }
                    line = l2;
                    detail::rtrim(line);
                    if (detail::hasMLString(line, keyChar)) {
                        line.pop_back();
                        line.pop_back();
                        line.pop_back();
                        if (lineExtension) {
                            detail::ltrim(line);
                        } else if (!(firstLine && item.empty())) {
                            item.push_back('\n');
                        }
                        firstLine = false;
                        item += line;
                        inMLineValue = false;
                        if (!item.empty() && item.back() == '\n') {
                            item.pop_back();
                        }
                        if (keyChar == '\"') {
                            try {
                                item = detail::remove_escaped_characters(item);
                            } catch (const std::invalid_argument& iarg) {
                                throw CLI::ParseError(iarg.what(), CLI::ExitCodes::InvalidError);
                            }
                        }
                    } else {
                        if (lineExtension) {
                            detail::trim(l2);
                        } else if (!(firstLine && item.empty())) {
                            item.push_back('\n');
                        }
                        lineExtension = false;
                        firstLine = false;
                        if (!l2.empty() && l2.back() == '\\' && keyChar == '\"') {
                            lineExtension = true;
                            l2.pop_back();
                        }
                        item += l2;
                    }
                }
                items_buffer = {item};
            } else if (!item.empty() && item.front() == aStart) {
                for (std::string multiline; item.back() != aEnd && std::getline(input, multiline);) {
                    detail::trim(multiline);
                    item += multiline;
                }
                if (item.back() == aEnd) {
                    items_buffer = detail::split_up(item.substr(1, item.length() - 2), aSep);
                } else {
                    items_buffer = detail::split_up(item.substr(1, std::string::npos), aSep);
                }
            } else if ((isDefaultArray || isINIArray) && item.find_first_of(aSep) != std::string::npos) {
                items_buffer = detail::split_up(item, aSep);
            } else if ((isDefaultArray || isINIArray) && item.find_first_of(' ') != std::string::npos) {
                items_buffer = detail::split_up(item, '\0');
            } else {
                items_buffer = {item};
            }
        } else {
            name = detail::trim_copy(line.substr(0, comment_pos));
            items_buffer = {"true"};
        }
        std::vector<std::string> parents;
        try {
            parents = detail::generate_parents(currentSection, name, parentSeparatorChar);
            detail::process_quoted_string(name, '"', '\'', true);
            for (auto& it : items_buffer) {
                detail::process_quoted_string(it, stringQuote, literalQuote);
            }
        } catch (const std::invalid_argument& ia) {
            throw CLI::ParseError(ia.what(), CLI::ExitCodes::InvalidError);
        }

        if (parents.size() > maximumLayers) {
            continue;
        }
        if (!configSection.empty() && !inSection) {
            if (parents.empty() || parents.front() != configSection) {
                continue;
            }
            if (configIndex >= 0 && currentSectionIndex != configIndex) {
                continue;
            }
            parents.erase(parents.begin());
            inSection = true;
        }
        auto match = detail::find_matching_config(output, parents, name, allowMultipleDuplicateFields);
        if (match != output.end()) {
            if ((match->inputs.size() > 1 && items_buffer.size() > 1) || allowMultipleDuplicateFields) {
                if (!(match->inputs.back().empty() || items_buffer.front().empty() || match->inputs.back() == "%%" ||
                      items_buffer.front() == "%%")) {
                    match->inputs.emplace_back("%%");
                    match->multiline = true;
                }
            }
            match->inputs.insert(match->inputs.end(), items_buffer.begin(), items_buffer.end());
        } else {
            output.emplace_back();
            output.back().parents = std::move(parents);
            output.back().name = std::move(name);
            output.back().inputs = std::move(items_buffer);
        }
    }
    if (currentSection != "default") {
        std::string ename;
        output.emplace_back();
        output.back().parents = detail::generate_parents(currentSection, ename, parentSeparatorChar);
        output.back().name = "--";
        while (output.back().parents.size() > 1) {
            output.push_back(output.back());
            output.back().parents.pop_back();
        }
    }
    return output;
}

CLI11_INLINE std::string& clean_name_string(std::string& name, const std::string& keyChars) {
    if (name.find_first_of(keyChars) != std::string::npos || (name.front() == '[' && name.back() == ']') ||
        (name.find_first_of("'`\"\\") != std::string::npos)) {
        if (name.find_first_of('\'') == std::string::npos) {
            name.insert(0, 1, '\'');
            name.push_back('\'');
        } else {
            if (detail::has_escapable_character(name)) {
                name = detail::add_escaped_characters(name);
            }
            name.insert(0, 1, '\"');
            name.push_back('\"');
        }
    }
    return name;
}

CLI11_INLINE std::string ConfigBase::to_config(const App* app, bool default_also, bool write_description,
                                               std::string prefix) const {
    std::stringstream out;
    std::string commentLead;
    commentLead.push_back(commentChar);
    commentLead.push_back(' ');

    std::string commentTest = "#;";
    commentTest.push_back(commentChar);
    commentTest.push_back(parentSeparatorChar);

    std::string keyChars = commentTest;
    keyChars.push_back(literalQuote);
    keyChars.push_back(stringQuote);
    keyChars.push_back(arrayStart);
    keyChars.push_back(arrayEnd);
    keyChars.push_back(valueDelimiter);
    keyChars.push_back(arraySeparator);

    std::vector<std::string> groups = app->get_groups();
    bool defaultUsed = false;
    groups.insert(groups.begin(), std::string("OPTIONS"));

    for (auto& group : groups) {
        if (group == "OPTIONS" || group.empty()) {
            if (defaultUsed) {
                continue;
            }
            defaultUsed = true;
        }
        if (write_description && group != "OPTIONS" && !group.empty()) {
            out << '\n' << commentChar << commentLead << group << " Options\n";
        }
        for (const Option* opt : app->get_options({})) {
            if (opt->get_configurable()) {
                if (opt->get_group() != group) {
                    if (!(group == "OPTIONS" && opt->get_group().empty())) {
                        continue;
                    }
                }
                std::string single_name = opt->get_single_name();
                if (single_name.empty()) {
                    continue;
                }

                auto results = opt->reduced_results();
                if (results.size() > 1 && opt->get_multi_option_policy() == CLI::MultiOptionPolicy::Reverse) {
                    std::reverse(results.begin(), results.end());
                }
                if (opt->get_multi_option_policy() == CLI::MultiOptionPolicy::Sum && opt->count() >= 1 &&
                    results.size() == 1) {
                    auto pos = opt->_validate(results[0], 0);
                    if (!pos.empty()) {
                        results = opt->results();
                    }
                }
                if (opt->get_multi_option_policy() == CLI::MultiOptionPolicy::Join && opt->count() > 1) {
                    char delim = opt->get_delimiter();
                    if (delim == '\0') {
                        results = opt->results();
                    } else {
                        auto delim_count = std::count(results[0].begin(), results[0].end(), delim);
                        if (results[0].back() == delim ||
                            static_cast<decltype(delim_count)>(opt->count()) < delim_count - 1 ||
                            results[0].find(std::string(2, delim)) != std::string::npos) {
                            results = opt->results();
                        }
                    }
                }
                std::string value;

                if (opt->count() == 1 && results.size() == 2 && results.front() == "{}" && results.back() == "%%") {
                    value = "\"{}\"";
                } else {
                    value = detail::ini_join(results, arraySeparator, arrayStart, arrayEnd, stringQuote, literalQuote);
                }

                bool isDefault = false;
                if (value.empty() && default_also) {
                    if (!opt->get_default_str().empty()) {
                        results_t res;
                        opt->results(res);
                        value = detail::ini_join(res, arraySeparator, arrayStart, arrayEnd, stringQuote, literalQuote);
                    } else if (opt->get_expected_min() == 0) {
                        value = "false";
                    } else if (opt->get_run_callback_for_default() || !opt->get_required()) {
                        value = "\"\"";
                    } else {
                        value = "\"<REQUIRED>\"";
                    }
                    isDefault = true;
                }

                if (!value.empty()) {
                    if (!opt->get_fnames().empty()) {
                        try {
                            value = opt->get_flag_value(single_name, value);
                        } catch (const CLI::ArgumentMismatch&) {
                            bool valid{false};
                            for (const auto& test_name : opt->get_fnames()) {
                                try {
                                    value = opt->get_flag_value(test_name, value);
                                    single_name = test_name;
                                    valid = true;
                                } catch (const CLI::ArgumentMismatch&) {
                                    continue;
                                }
                            }
                            if (!valid) {
                                value = detail::ini_join(opt->results(), arraySeparator, arrayStart, arrayEnd,
                                                         stringQuote, literalQuote);
                            }
                        }
                    }
                    if (write_description && opt->has_description()) {
                        if (out.tellp() != std::streampos(0)) {
                            out << '\n';
                        }
                        out << commentLead << detail::fix_newlines(commentLead, opt->get_description()) << '\n';
                    }
                    clean_name_string(single_name, keyChars);

                    std::string name = prefix + single_name;
                    if (commentDefaultsBool && isDefault) {
                        name = commentChar + name;
                    }
                    out << name << valueDelimiter << value << '\n';
                }
            }
        }
    }

    auto subcommands = app->get_subcommands({});
    for (const App* subcom : subcommands) {
        if (subcom->get_name().empty()) {
            if (!default_also && (subcom->count_all() == 0)) {
                continue;
            }
            if (write_description && !subcom->get_group().empty()) {
                out << '\n' << commentLead << subcom->get_group() << " Options\n";
            }
            out << to_config(subcom, default_also, write_description, prefix);
        }
    }

    for (const App* subcom : subcommands) {
        if (!subcom->get_name().empty()) {
            if (!default_also && (subcom->count_all() == 0)) {
                continue;
            }
            std::string subname = subcom->get_name();
            clean_name_string(subname, keyChars);

            if (subcom->get_configurable() && (default_also || app->got_subcommand(subcom))) {
                if (!prefix.empty() || app->get_parent() == nullptr) {
                    out << '[' << prefix << subname << "]\n";
                } else {
                    std::string appname = app->get_name();
                    clean_name_string(appname, keyChars);
                    subname = appname + parentSeparatorChar + subname;
                    const auto* p = app->get_parent();
                    while (p->get_parent() != nullptr) {
                        std::string pname = p->get_name();
                        clean_name_string(pname, keyChars);
                        subname = pname + parentSeparatorChar + subname;
                        p = p->get_parent();
                    }
                    out << '[' << subname << "]\n";
                }
                out << to_config(subcom, default_also, write_description, "");
            } else {
                out << to_config(subcom, default_also, write_description, prefix + subname + parentSeparatorChar);
            }
        }
    }

    if (write_description && !out.str().empty()) {
        std::string outString =
            commentChar + commentLead + detail::fix_newlines(commentChar + commentLead, app->get_description()) + '\n';
        return outString + out.str();
    }
    return out.str();
}

CLI11_INLINE std::string Formatter::make_group(std::string group, bool is_positional,
                                               std::vector<const Option*> opts) const {
    std::stringstream out;

    out << "\n" << group << ":\n";
    for (const Option* opt : opts) {
        out << make_option(opt, is_positional);
    }

    return out.str();
}

CLI11_INLINE std::string Formatter::make_positionals(const App* app) const {
    std::vector<const Option*> opts =
        app->get_options([](const Option* opt) { return !opt->get_group().empty() && opt->get_positional(); });

    if (opts.empty())
        return {};

    return make_group(get_label("POSITIONALS"), true, opts);
}

CLI11_INLINE std::string Formatter::make_groups(const App* app, AppFormatMode mode) const {
    std::stringstream out;
    std::vector<std::string> groups = app->get_groups();

    for (const std::string& group : groups) {
        std::vector<const Option*> opts = app->get_options([app, mode, &group](const Option* opt) {
            return opt->get_group() == group && opt->nonpositional() &&
                   (mode != AppFormatMode::Sub || (app->get_help_ptr() != opt && app->get_help_all_ptr() != opt));
        });
        if (!group.empty() && !opts.empty()) {
            out << make_group(group, false, opts);
        }
    }

    return out.str();
}

CLI11_INLINE std::string Formatter::make_description(const App* app) const {
    std::string desc = app->get_description();
    auto min_options = app->get_require_option_min();
    auto max_options = app->get_require_option_max();

    if (app->get_required()) {
        desc += " " + get_label("REQUIRED") + " ";
    }

    if (min_options > 0) {
        if (max_options == min_options) {
            desc += " \n[Exactly " + std::to_string(min_options) + " of the following options are required]";
        } else if (max_options > 0) {
            desc += " \n[Between " + std::to_string(min_options) + " and " + std::to_string(max_options) +
                    " of the following options are required]";
        } else {
            desc += " \n[At least " + std::to_string(min_options) + " of the following options are required]";
        }
    } else if (max_options > 0) {
        desc += " \n[At most " + std::to_string(max_options) + " of the following options are allowed]";
    }

    return (!desc.empty()) ? desc + "\n\n" : std::string{};
}

CLI11_INLINE std::string Formatter::make_usage(const App* app, std::string name) const {
    std::string usage = app->get_usage();
    if (!usage.empty()) {
        return usage + "\n\n";
    }

    std::stringstream out;
    out << '\n';

    if (name.empty())
        out << get_label("Usage") << ':';
    else
        out << name;

    std::vector<std::string> groups = app->get_groups();

    std::vector<const Option*> non_pos_options =
        app->get_options([](const Option* opt) { return opt->nonpositional(); });
    if (!non_pos_options.empty())
        out << " [" << get_label("OPTIONS") << "]";

    std::vector<const Option*> positionals = app->get_options([](const Option* opt) { return opt->get_positional(); });

    if (!positionals.empty()) {
        std::vector<std::string> positional_names(positionals.size());
        std::transform(positionals.begin(), positionals.end(), positional_names.begin(),
                       [this](const Option* opt) { return make_option_usage(opt); });

        out << " " << detail::join(positional_names, " ");
    }

    if (!app->get_subcommands(
                [](const CLI::App* subc) { return ((!subc->get_disabled()) && (!subc->get_name().empty())); })
             .empty()) {
        out << ' ' << (app->get_require_subcommand_min() == 0 ? "[" : "")
            << get_label(app->get_require_subcommand_max() == 1 ? "SUBCOMMAND" : "SUBCOMMANDS")
            << (app->get_require_subcommand_min() == 0 ? "]" : "");
    }

    out << "\n\n";

    return out.str();
}

CLI11_INLINE std::string Formatter::make_footer(const App* app) const {
    std::string footer = app->get_footer();
    if (footer.empty()) {
        return std::string{};
    }
    return '\n' + footer + '\n';
}

CLI11_INLINE std::string Formatter::make_help(const App* app, std::string name, AppFormatMode mode) const {
    if (mode == AppFormatMode::Sub)
        return make_expanded(app, mode);

    std::stringstream out;
    if ((app->get_name().empty()) && (app->get_parent() != nullptr)) {
        if (app->get_group() != "SUBCOMMANDS") {
            out << app->get_group() << ':';
        }
    }
    if (is_description_paragraph_formatting_enabled()) {
        detail::streamOutAsParagraph(out, make_description(app), description_paragraph_width_, "");
    } else {
        out << make_description(app) << '\n';
    }
    out << make_usage(app, name);
    out << make_positionals(app);
    out << make_groups(app, mode);
    out << make_subcommands(app, mode);
    std::string footer_string = make_footer(app);

    if (is_footer_paragraph_formatting_enabled()) {
        detail::streamOutAsParagraph(out, footer_string, footer_paragraph_width_);
    } else {
        out << footer_string;
    }

    return out.str();
}

CLI11_INLINE std::string Formatter::make_subcommands(const App* app, AppFormatMode mode) const {
    std::stringstream out;

    std::vector<const App*> subcommands = app->get_subcommands({});

    std::vector<std::string> subcmd_groups_seen;
    for (const App* com : subcommands) {
        if (com->get_name().empty()) {
            if (!com->get_group().empty() && com->get_group().front() != '+') {
                out << make_expanded(com, mode);
            }
            continue;
        }
        std::string group_key = com->get_group();
        if (!group_key.empty() &&
            std::find_if(subcmd_groups_seen.begin(), subcmd_groups_seen.end(), [&group_key](std::string a) {
                return detail::to_lower(a) == detail::to_lower(group_key);
            }) == subcmd_groups_seen.end())
            subcmd_groups_seen.push_back(group_key);
    }

    for (const std::string& group : subcmd_groups_seen) {
        out << '\n' << group << ":\n";
        std::vector<const App*> subcommands_group = app->get_subcommands(
            [&group](const App* sub_app) { return detail::to_lower(sub_app->get_group()) == detail::to_lower(group); });
        for (const App* new_com : subcommands_group) {
            if (new_com->get_name().empty())
                continue;
            if (mode != AppFormatMode::All) {
                out << make_subcommand(new_com);
            } else {
                out << new_com->help(new_com->get_name(), AppFormatMode::Sub);
                out << '\n';
            }
        }
    }

    return out.str();
}

CLI11_INLINE std::string Formatter::make_subcommand(const App* sub) const {
    std::stringstream out;
    std::string name = "  " + sub->get_display_name(true) + (sub->get_required() ? " " + get_label("REQUIRED") : "");

    out << std::setw(static_cast<int>(column_width_)) << std::left << name;
    detail::streamOutAsParagraph(out, sub->get_description(), right_column_width_, std::string(column_width_, ' '),
                                 true);
    out << '\n';
    return out.str();
}

CLI11_INLINE std::string Formatter::make_expanded(const App* sub, AppFormatMode mode) const {
    std::stringstream out;
    out << sub->get_display_name(true) << '\n';

    if (is_description_paragraph_formatting_enabled()) {
        detail::streamOutAsParagraph(out, make_description(sub), description_paragraph_width_, "  ");
    } else {
        out << make_description(sub) << '\n';
    }

    if (sub->get_name().empty() && !sub->get_aliases().empty()) {
        detail::format_aliases(out, sub->get_aliases(), column_width_ + 2);
    }

    out << make_positionals(sub);
    out << make_groups(sub, mode);
    out << make_subcommands(sub, mode);
    std::string footer_string = make_footer(sub);

    if (mode == AppFormatMode::Sub && !footer_string.empty()) {
        const auto* parent = sub->get_parent();
        std::string parent_footer = (parent != nullptr) ? make_footer(sub->get_parent()) : std::string{};
        if (footer_string == parent_footer) {
            footer_string = "";
        }
    }
    if (!footer_string.empty()) {
        if (is_footer_paragraph_formatting_enabled()) {
            detail::streamOutAsParagraph(out, footer_string, footer_paragraph_width_);
        } else {
            out << footer_string;
        }
    }
    return out.str();
}

CLI11_INLINE std::string Formatter::make_option(const Option* opt, bool is_positional) const {
    std::stringstream out;
    if (is_positional) {
        const std::string left = "  " + make_option_name(opt, true) + make_option_opts(opt);
        const std::string desc = make_option_desc(opt);
        out << std::setw(static_cast<int>(column_width_)) << std::left << left;

        if (!desc.empty()) {
            bool skipFirstLinePrefix = true;
            if (left.length() >= column_width_) {
                out << '\n';
                skipFirstLinePrefix = false;
            }
            detail::streamOutAsParagraph(out, desc, right_column_width_, std::string(column_width_, ' '),
                                         skipFirstLinePrefix);
        }
    } else {
        const std::string namesCombined = make_option_name(opt, false);
        const std::string opts = make_option_opts(opt);
        const std::string desc = make_option_desc(opt);

        const auto names = detail::split(namesCombined, ',');
        std::vector<std::string> vshortNames;
        std::vector<std::string> vlongNames;
        std::for_each(names.begin(), names.end(), [&vshortNames, &vlongNames](const std::string& name) {
            if (name.find("--", 0) != std::string::npos)
                vlongNames.push_back(name);
            else
                vshortNames.push_back(name);
        });

        std::string shortNames = detail::join(vshortNames, ", ");
        std::string longNames = detail::join(vlongNames, ", ");

        const auto shortNamesColumnWidth =
            static_cast<int>(static_cast<float>(column_width_) * long_option_alignment_ratio_);
        const auto longNamesColumnWidth = static_cast<int>(column_width_) - shortNamesColumnWidth;
        int shortNamesOverSize = 0;

        if (!shortNames.empty()) {
            shortNames = "  " + shortNames;
            if (longNames.empty() && !opts.empty())
                shortNames += opts;
            if (!longNames.empty())
                shortNames += ",";
            if (static_cast<int>(shortNames.length()) >= shortNamesColumnWidth) {
                shortNames += " ";
                shortNamesOverSize = static_cast<int>(shortNames.length()) - shortNamesColumnWidth;
            }
            out << std::setw(shortNamesColumnWidth) << std::left << shortNames;
        } else {
            out << std::setw(shortNamesColumnWidth) << std::left << "";
        }

        shortNamesOverSize = (std::min)(shortNamesOverSize, longNamesColumnWidth);
        const auto adjustedLongNamesColumnWidth = longNamesColumnWidth - shortNamesOverSize;

        if (!longNames.empty()) {
            if (!opts.empty())
                longNames += opts;
            if (static_cast<int>(longNames.length()) >= adjustedLongNamesColumnWidth)
                longNames += " ";

            out << std::setw(adjustedLongNamesColumnWidth) << std::left << longNames;
        } else {
            out << std::setw(adjustedLongNamesColumnWidth) << std::left << "";
        }

        if (!desc.empty()) {
            bool skipFirstLinePrefix = true;
            if (out.str().length() > column_width_) {
                out << '\n';
                skipFirstLinePrefix = false;
            }
            detail::streamOutAsParagraph(out, desc, right_column_width_, std::string(column_width_, ' '),
                                         skipFirstLinePrefix);
        }
    }

    out << '\n';
    return out.str();
}

CLI11_INLINE std::string Formatter::make_option_name(const Option* opt, bool is_positional) const {
    if (is_positional)
        return opt->get_name(true, false);

    return opt->get_name(false, true, !enable_default_flag_values_);
}

CLI11_INLINE std::string Formatter::make_option_opts(const Option* opt) const {
    std::stringstream out;

    if (!opt->get_option_text().empty()) {
        out << " " << opt->get_option_text();
    } else {
        if (opt->get_type_size() != 0) {
            if (enable_option_type_names_) {
                if (!opt->get_type_name().empty())
                    out << " " << get_label(opt->get_type_name());
            }
            if (enable_option_defaults_) {
                if (!opt->get_default_str().empty())
                    out << " [" << opt->get_default_str() << "] ";
            }
            if (opt->get_expected_max() == detail::expected_max_vector_size)
                out << " ...";
            else if (opt->get_expected_min() > 1)
                out << " x " << opt->get_expected();

            if (opt->get_required())
                out << " " << get_label("REQUIRED");
        }
        if (!opt->get_envname().empty())
            out << " (" << get_label("Env") << ":" << opt->get_envname() << ")";
        if (!opt->get_needs().empty()) {
            out << " " << get_label("Needs") << ":";
            for (const Option* op : opt->get_needs())
                out << " " << op->get_name();
        }
        if (!opt->get_excludes().empty()) {
            out << " " << get_label("Excludes") << ":";
            for (const Option* op : opt->get_excludes())
                out << " " << op->get_name();
        }
    }
    return out.str();
}

CLI11_INLINE std::string Formatter::make_option_desc(const Option* opt) const {
    return opt->get_description();
}

CLI11_INLINE std::string Formatter::make_option_usage(const Option* opt) const {
    std::stringstream out;
    out << make_option_name(opt, true);
    if (opt->get_expected_max() >= detail::expected_max_vector_size)
        out << "...";
    else if (opt->get_expected_max() > 1)
        out << "(" << opt->get_expected() << "x)";

    return opt->get_required() ? out.str() : "[" + out.str() + "]";
}

}  
