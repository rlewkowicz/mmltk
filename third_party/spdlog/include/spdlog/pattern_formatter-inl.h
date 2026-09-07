
#pragma once

#ifndef SPDLOG_HEADER_ONLY
#include <spdlog/pattern_formatter.h>
#endif

#include <spdlog/details/fmt_helper.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/os.h>

#ifndef SPDLOG_NO_TLS
#include <spdlog/mdc.h>
#endif

#include <spdlog/fmt/fmt.h>
#include <spdlog/formatter.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace spdlog {
namespace details {

class scoped_padder {
   public:
    scoped_padder(size_t wrapped_size, const padding_info& padinfo, memory_buf_t& dest)
        : padinfo_(padinfo), dest_(dest) {
        remaining_pad_ = static_cast<long>(padinfo.width_) - static_cast<long>(wrapped_size);
        if (remaining_pad_ <= 0) {
            return;
        }

        if (padinfo_.side_ == padding_info::pad_side::left) {
            pad_it(remaining_pad_);
            remaining_pad_ = 0;
        } else if (padinfo_.side_ == padding_info::pad_side::center) {
            auto half_pad = remaining_pad_ / 2;
            auto reminder = remaining_pad_ & 1;
            pad_it(half_pad);
            remaining_pad_ = half_pad + reminder;
        }
    }

    template <typename T>
    static unsigned int count_digits(T n) {
        return fmt_helper::count_digits(n);
    }

    ~scoped_padder() {
        if (remaining_pad_ >= 0) {
            pad_it(remaining_pad_);
        } else if (padinfo_.truncate_) {
            long new_size = static_cast<long>(dest_.size()) + remaining_pad_;
            if (new_size < 0) {
                new_size = 0;
            }
            dest_.resize(static_cast<size_t>(new_size));
        }
    }

   private:
    void pad_it(long count) {
        // The two-argument view constructor receives the exact initialized byte count; termination is irrelevant.
        // NOLINTBEGIN(bugprone-suspicious-stringview-data-usage)
        fmt_helper::append_string_view(string_view_t(spaces_.data(), static_cast<size_t>(count)), dest_);
        // NOLINTEND(bugprone-suspicious-stringview-data-usage)
    }

    const padding_info& padinfo_;
    memory_buf_t& dest_;
    long remaining_pad_;
    string_view_t spaces_{"                                                                ", 64};
};

struct null_scoped_padder {
    null_scoped_padder(size_t, const padding_info&, memory_buf_t&) {}

    template <typename T>
    static unsigned int count_digits(T) {
        return 0;
    }
};

// Generic flag formatter for "fetch a text field, pad to its size, append it".
// The field is selected at compile time by a getter over the message/tm pair.
template <typename ScopedPadder, string_view_t (*Field)(const details::log_msg&, const std::tm&)>
class text_flag_formatter final : public flag_formatter {
   public:
    explicit text_flag_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm& tm_time, memory_buf_t& dest) override {
        string_view_t field_value = Field(msg, tm_time);
        ScopedPadder p(field_value.size(), padinfo_, dest);
        fmt_helper::append_string_view(field_value, dest);
    }
};

inline const char* ampm(const tm& t) {
    return t.tm_hour >= 12 ? "PM" : "AM";
}

inline int to12h(const tm& t) {
    return t.tm_hour > 12 ? t.tm_hour - 12 : t.tm_hour;
}

inline constexpr std::array<const char*, 7> days{{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}};

inline constexpr std::array<const char*, 7> full_days{
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"}};

inline constexpr std::array<const char*, 12> months{
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}};

inline constexpr std::array<const char*, 12> full_months{
    {"January", "February", "March", "April", "May", "June", "July",
     "August", "September", "October", "November", "December"}};

inline string_view_t logger_name_field(const details::log_msg& msg, const std::tm&) {
    return msg.logger_name;
}

inline string_view_t level_field(const details::log_msg& msg, const std::tm&) {
    return level::to_string_view(msg.level);
}

inline string_view_t short_level_field(const details::log_msg& msg, const std::tm&) {
    const char* const short_name = level::to_short_c_str(msg.level);
    return short_name != nullptr ? string_view_t{short_name} : string_view_t{};
}

inline string_view_t payload_field(const details::log_msg& msg, const std::tm&) {
    return msg.payload;
}

inline string_view_t weekday_field(const details::log_msg&, const std::tm& tm_time) {
    return days[static_cast<size_t>(tm_time.tm_wday)];
}

inline string_view_t full_weekday_field(const details::log_msg&, const std::tm& tm_time) {
    return full_days[static_cast<size_t>(tm_time.tm_wday)];
}

inline string_view_t month_field(const details::log_msg&, const std::tm& tm_time) {
    return months[static_cast<size_t>(tm_time.tm_mon)];
}

inline string_view_t full_month_field(const details::log_msg&, const std::tm& tm_time) {
    return full_months[static_cast<size_t>(tm_time.tm_mon)];
}

inline string_view_t ampm_field(const details::log_msg&, const std::tm& tm_time) {
    return ampm(tm_time);
}

template <typename ScopedPadder>
using name_formatter = text_flag_formatter<ScopedPadder, logger_name_field>;
template <typename ScopedPadder>
using level_formatter = text_flag_formatter<ScopedPadder, level_field>;
template <typename ScopedPadder>
using short_level_formatter = text_flag_formatter<ScopedPadder, short_level_field>;
template <typename ScopedPadder>
using v_formatter = text_flag_formatter<ScopedPadder, payload_field>;
template <typename ScopedPadder>
using a_formatter = text_flag_formatter<ScopedPadder, weekday_field>;
template <typename ScopedPadder>
using A_formatter = text_flag_formatter<ScopedPadder, full_weekday_field>;
template <typename ScopedPadder>
using b_formatter = text_flag_formatter<ScopedPadder, month_field>;
template <typename ScopedPadder>
using B_formatter = text_flag_formatter<ScopedPadder, full_month_field>;
template <typename ScopedPadder>
using p_formatter = text_flag_formatter<ScopedPadder, ampm_field>;

template <typename ScopedPadder>
class c_formatter final : public flag_formatter {
   public:
    explicit c_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg&, const std::tm& tm_time, memory_buf_t& dest) override {
        const size_t field_size = 24;
        ScopedPadder p(field_size, padinfo_, dest);

        fmt_helper::append_string_view(days[static_cast<size_t>(tm_time.tm_wday)], dest);
        dest.push_back(' ');
        fmt_helper::append_string_view(months[static_cast<size_t>(tm_time.tm_mon)], dest);
        dest.push_back(' ');
        fmt_helper::append_int(tm_time.tm_mday, dest);
        dest.push_back(' ');

        fmt_helper::pad2(tm_time.tm_hour, dest);
        dest.push_back(':');
        fmt_helper::pad2(tm_time.tm_min, dest);
        dest.push_back(':');
        fmt_helper::pad2(tm_time.tm_sec, dest);
        dest.push_back(' ');
        fmt_helper::append_int(tm_time.tm_year + 1900, dest);
    }
};

// Generic flag formatter for one or more zero-padded two-digit tm fields
// joined by Sep. Covers every "%X" flag that is a pad2 of a tm member as well
// as the composite date/time flags built purely from such fields.
template <typename ScopedPadder, char Sep, int (*... Fields)(const std::tm&)>
class tm_pad2_formatter final : public flag_formatter {
   public:
    explicit tm_pad2_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg&, const std::tm& tm_time, memory_buf_t& dest) override {
        const size_t field_size = sizeof...(Fields) * 3 - 1;
        ScopedPadder p(field_size, padinfo_, dest);
        bool first = true;
        for (int field_value : {Fields(tm_time)...}) {
            if (!first) {
                dest.push_back(Sep);
            }
            first = false;
            fmt_helper::pad2(field_value, dest);
        }
    }
};

inline int tm_year2_field(const std::tm& t) {
    return t.tm_year % 100;
}

inline int tm_mon1_field(const std::tm& t) {
    return t.tm_mon + 1;
}

inline int tm_mday_field(const std::tm& t) {
    return t.tm_mday;
}

inline int tm_hour_field(const std::tm& t) {
    return t.tm_hour;
}

inline int tm_min_field(const std::tm& t) {
    return t.tm_min;
}

inline int tm_sec_field(const std::tm& t) {
    return t.tm_sec;
}

template <typename ScopedPadder>
using C_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_year2_field>;
template <typename ScopedPadder>
using D_formatter = tm_pad2_formatter<ScopedPadder, '/', tm_mon1_field, tm_mday_field, tm_year2_field>;
template <typename ScopedPadder>
using m_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_mon1_field>;
template <typename ScopedPadder>
using d_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_mday_field>;
template <typename ScopedPadder>
using H_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_hour_field>;
template <typename ScopedPadder>
using I_formatter = tm_pad2_formatter<ScopedPadder, ':', to12h>;
template <typename ScopedPadder>
using M_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_min_field>;
template <typename ScopedPadder>
using S_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_sec_field>;
template <typename ScopedPadder>
using R_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_hour_field, tm_min_field>;
template <typename ScopedPadder>
using T_formatter = tm_pad2_formatter<ScopedPadder, ':', tm_hour_field, tm_min_field, tm_sec_field>;

template <typename ScopedPadder>
class Y_formatter final : public flag_formatter {
   public:
    explicit Y_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg&, const std::tm& tm_time, memory_buf_t& dest) override {
        const size_t field_size = 4;
        ScopedPadder p(field_size, padinfo_, dest);
        fmt_helper::append_int(tm_time.tm_year + 1900, dest);
    }
};

// Generic flag formatter for the sub-second fraction of the message timestamp,
// zero padded to Digits digits.
template <typename ScopedPadder, typename Units, size_t Digits>
class time_fraction_formatter final : public flag_formatter {
   public:
    explicit time_fraction_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm&, memory_buf_t& dest) override {
        auto fraction = fmt_helper::time_fraction<Units>(msg.time);
        ScopedPadder p(Digits, padinfo_, dest);
        if constexpr (Digits == 3) {
            fmt_helper::pad3(static_cast<uint32_t>(fraction.count()), dest);
        } else if constexpr (Digits == 6) {
            fmt_helper::pad6(static_cast<size_t>(fraction.count()), dest);
        } else {
            fmt_helper::pad9(static_cast<size_t>(fraction.count()), dest);
        }
    }
};

template <typename ScopedPadder>
using e_formatter = time_fraction_formatter<ScopedPadder, std::chrono::milliseconds, 3>;
template <typename ScopedPadder>
using f_formatter = time_fraction_formatter<ScopedPadder, std::chrono::microseconds, 6>;
template <typename ScopedPadder>
using F_formatter = time_fraction_formatter<ScopedPadder, std::chrono::nanoseconds, 9>;

template <typename ScopedPadder>
class E_formatter final : public flag_formatter {
   public:
    explicit E_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm&, memory_buf_t& dest) override {
        const size_t field_size = 10;
        ScopedPadder p(field_size, padinfo_, dest);
        auto duration = msg.time.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        fmt_helper::append_int(seconds, dest);
    }
};

template <typename ScopedPadder>
class r_formatter final : public flag_formatter {
   public:
    explicit r_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg&, const std::tm& tm_time, memory_buf_t& dest) override {
        const size_t field_size = 11;
        ScopedPadder p(field_size, padinfo_, dest);

        fmt_helper::pad2(to12h(tm_time), dest);
        dest.push_back(':');
        fmt_helper::pad2(tm_time.tm_min, dest);
        dest.push_back(':');
        fmt_helper::pad2(tm_time.tm_sec, dest);
        dest.push_back(' ');
        fmt_helper::append_string_view(ampm(tm_time), dest);
    }
};

template <typename ScopedPadder>
class z_formatter final : public flag_formatter {
   public:
    explicit z_formatter(padding_info padinfo, pattern_time_type time_type)
        : flag_formatter(padinfo), time_type_(time_type) {}

    z_formatter() = default;
    z_formatter(const z_formatter&) = delete;
    z_formatter& operator=(const z_formatter&) = delete;

    void format(const details::log_msg& msg, const std::tm& tm_time, memory_buf_t& dest) override {
        const size_t field_size = 6;
        ScopedPadder p(field_size, padinfo_, dest);
#ifdef SPDLOG_NO_TZ_OFFSET
        const char* str = "+??:??";
        dest.append(str, str + 6);
#else
        if (time_type_ == pattern_time_type::utc) {
            const char* zeroes = "+00:00";
            dest.append(zeroes, zeroes + 6);
            return;
        }
        auto total_minutes = get_cached_offset(msg, tm_time);
        bool is_negative = total_minutes < 0;
        if (is_negative) {
            total_minutes = -total_minutes;
            dest.push_back('-');
        } else {
            dest.push_back('+');
        }

        fmt_helper::pad2(total_minutes / 60, dest);
        dest.push_back(':');
        fmt_helper::pad2(total_minutes % 60, dest);
#endif  // SPDLOG_NO_TZ_OFFSET
    }

   private:
    pattern_time_type time_type_;
    log_clock::time_point last_update_{std::chrono::seconds(0)};
    int offset_minutes_{0};

    int get_cached_offset(const log_msg& msg, const std::tm& tm_time) {
        if (msg.time - last_update_ >= std::chrono::seconds(10)) {
            offset_minutes_ = os::utc_minutes_offset(tm_time);
            last_update_ = msg.time;
        }
        return offset_minutes_;
    }
};

template <typename ScopedPadder>
class t_formatter final : public flag_formatter {
   public:
    explicit t_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm&, memory_buf_t& dest) override {
        const auto field_size = ScopedPadder::count_digits(msg.thread_id);
        ScopedPadder p(field_size, padinfo_, dest);
        fmt_helper::append_int(msg.thread_id, dest);
    }
};

template <typename ScopedPadder>
class pid_formatter final : public flag_formatter {
   public:
    explicit pid_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg&, const std::tm&, memory_buf_t& dest) override {
        const auto pid = static_cast<uint32_t>(details::os::pid());
        auto field_size = ScopedPadder::count_digits(pid);
        ScopedPadder p(field_size, padinfo_, dest);
        fmt_helper::append_int(pid, dest);
    }
};

class ch_formatter final : public flag_formatter {
   public:
    explicit ch_formatter(char ch) : ch_(ch) {}

    void format(const details::log_msg&, const std::tm&, memory_buf_t& dest) override {
        dest.push_back(ch_);
    }

   private:
    char ch_;
};

class aggregate_formatter final : public flag_formatter {
   public:
    aggregate_formatter() = default;

    void add_ch(char ch) {
        str_ += ch;
    }
    void format(const details::log_msg&, const std::tm&, memory_buf_t& dest) override {
        fmt_helper::append_string_view(str_, dest);
    }

   private:
    std::string str_;
};

class color_start_formatter final : public flag_formatter {
   public:
    explicit color_start_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm&, memory_buf_t& dest) override {
        msg.color_range_start = dest.size();
    }
};

class color_stop_formatter final : public flag_formatter {
   public:
    explicit color_stop_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm&, memory_buf_t& dest) override {
        msg.color_range_end = dest.size();
    }
};

// Base for the source-location flags: emits only padding when the message
// carries no source information, otherwise defers to the derived field writer.
template <typename ScopedPadder>
class source_flag_formatter : public flag_formatter {
   public:
    explicit source_flag_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm&, memory_buf_t& dest) final {
        if (msg.source.empty()) {
            ScopedPadder p(0, padinfo_, dest);
            return;
        }
        format_source(msg.source, dest);
    }

   private:
    virtual void format_source(const source_loc& source, memory_buf_t& dest) = 0;
};

inline const char* source_basename(const char* filename) {
    if (sizeof(os::folder_seps) == 2) {
        const char* rv = std::strrchr(filename, os::folder_seps[0]);
        return rv != nullptr ? rv + 1 : filename;
    } else {
        const std::reverse_iterator<const char*> begin(filename + std::strlen(filename));
        const std::reverse_iterator<const char*> end(filename);

        const auto it = std::find_first_of(begin, end, std::begin(os::folder_seps), std::end(os::folder_seps) - 1);
        return it != end ? it.base() : filename;
    }
}

inline const char* source_filename_text(const source_loc& source) {
    return source.filename;
}

inline const char* source_short_filename_text(const source_loc& source) {
    return source_basename(source.filename);
}

inline const char* source_funcname_text(const source_loc& source) {
    return source.funcname;
}

// Source flags that print a single C-string selected from the source_loc.
template <typename ScopedPadder, const char* (*Text)(const source_loc&)>
class source_text_formatter final : public source_flag_formatter<ScopedPadder> {
   public:
    using source_flag_formatter<ScopedPadder>::source_flag_formatter;

   private:
    void format_source(const source_loc& source, memory_buf_t& dest) override {
        const char* text = Text(source);
        size_t text_size = this->padinfo_.enabled() ? std::char_traits<char>::length(text) : 0;
        ScopedPadder p(text_size, this->padinfo_, dest);
        fmt_helper::append_string_view(text, dest);
    }
};

template <typename ScopedPadder>
using source_filename_formatter = source_text_formatter<ScopedPadder, source_filename_text>;
template <typename ScopedPadder>
using short_filename_formatter = source_text_formatter<ScopedPadder, source_short_filename_text>;
template <typename ScopedPadder>
using source_funcname_formatter = source_text_formatter<ScopedPadder, source_funcname_text>;

template <typename ScopedPadder>
class source_location_formatter final : public source_flag_formatter<ScopedPadder> {
   public:
    using source_flag_formatter<ScopedPadder>::source_flag_formatter;

   private:
    void format_source(const source_loc& source, memory_buf_t& dest) override {
        size_t text_size;
        if (this->padinfo_.enabled()) {
            text_size = std::char_traits<char>::length(source.filename) + ScopedPadder::count_digits(source.line) + 1;
        } else {
            text_size = 0;
        }

        ScopedPadder p(text_size, this->padinfo_, dest);
        fmt_helper::append_string_view(source.filename, dest);
        dest.push_back(':');
        fmt_helper::append_int(source.line, dest);
    }
};

template <typename ScopedPadder>
class source_linenum_formatter final : public source_flag_formatter<ScopedPadder> {
   public:
    using source_flag_formatter<ScopedPadder>::source_flag_formatter;

   private:
    void format_source(const source_loc& source, memory_buf_t& dest) override {
        auto field_size = ScopedPadder::count_digits(source.line);
        ScopedPadder p(field_size, this->padinfo_, dest);
        fmt_helper::append_int(source.line, dest);
    }
};

template <typename ScopedPadder, typename Units>
class elapsed_formatter final : public flag_formatter {
   public:
    using DurationUnits = Units;

    explicit elapsed_formatter(padding_info padinfo) : flag_formatter(padinfo), last_message_time_(log_clock::now()) {}

    void format(const details::log_msg& msg, const std::tm&, memory_buf_t& dest) override {
        auto delta = (std::max)(msg.time - last_message_time_, log_clock::duration::zero());
        auto delta_units = std::chrono::duration_cast<DurationUnits>(delta);
        last_message_time_ = msg.time;
        auto delta_count = static_cast<size_t>(delta_units.count());
        auto n_digits = static_cast<size_t>(ScopedPadder::count_digits(delta_count));
        ScopedPadder p(n_digits, padinfo_, dest);
        fmt_helper::append_int(delta_count, dest);
    }

   private:
    log_clock::time_point last_message_time_;
};

#ifndef SPDLOG_NO_TLS
template <typename ScopedPadder>
class mdc_formatter : public flag_formatter {
   public:
    explicit mdc_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg&, const std::tm&, memory_buf_t& dest) override {
        const auto& mdc_map = mdc::get_context();
        if (mdc_map.empty()) {
            ScopedPadder p(0, padinfo_, dest);
            return;
        } else {
            format_mdc(mdc_map, dest);
        }
    }

    void format_mdc(const mdc::mdc_map_t& mdc_map, memory_buf_t& dest) {
        const auto last_element = std::prev(mdc_map.end());
        for (auto it = mdc_map.begin(); it != mdc_map.end(); ++it) {
            const auto& key = it->first;
            const auto& value = it->second;
            size_t content_size = key.size() + value.size() + 1;

            if (it != last_element) {
                content_size++;
            }

            ScopedPadder p(content_size, padinfo_, dest);
            fmt_helper::append_string_view(key, dest);
            fmt_helper::append_string_view(":", dest);
            fmt_helper::append_string_view(value, dest);
            if (it != last_element) {
                fmt_helper::append_string_view(" ", dest);
            }
        }
    }
};
#endif

class full_formatter final : public flag_formatter {
   public:
    explicit full_formatter(padding_info padinfo) : flag_formatter(padinfo) {}

    void format(const details::log_msg& msg, const std::tm& tm_time, memory_buf_t& dest) override {
        using std::chrono::duration_cast;
        using std::chrono::milliseconds;
        using std::chrono::seconds;

        auto duration = msg.time.time_since_epoch();
        auto secs = duration_cast<seconds>(duration);

        if (cache_timestamp_ != secs || cached_datetime_.size() == 0) {
            cached_datetime_.clear();
            cached_datetime_.push_back('[');
            fmt_helper::append_int(tm_time.tm_year + 1900, cached_datetime_);
            cached_datetime_.push_back('-');

            fmt_helper::pad2(tm_time.tm_mon + 1, cached_datetime_);
            cached_datetime_.push_back('-');

            fmt_helper::pad2(tm_time.tm_mday, cached_datetime_);
            cached_datetime_.push_back(' ');

            fmt_helper::pad2(tm_time.tm_hour, cached_datetime_);
            cached_datetime_.push_back(':');

            fmt_helper::pad2(tm_time.tm_min, cached_datetime_);
            cached_datetime_.push_back(':');

            fmt_helper::pad2(tm_time.tm_sec, cached_datetime_);
            cached_datetime_.push_back('.');

            cache_timestamp_ = secs;
        }
        dest.append(cached_datetime_.begin(), cached_datetime_.end());

        auto millis = fmt_helper::time_fraction<milliseconds>(msg.time);
        fmt_helper::pad3(static_cast<uint32_t>(millis.count()), dest);
        dest.push_back(']');
        dest.push_back(' ');

        if (msg.logger_name.size() > 0) {
            dest.push_back('[');
            fmt_helper::append_string_view(msg.logger_name, dest);
            dest.push_back(']');
            dest.push_back(' ');
        }

        dest.push_back('[');
        msg.color_range_start = dest.size();
        fmt_helper::append_string_view(level::to_string_view(msg.level), dest);
        msg.color_range_end = dest.size();
        dest.push_back(']');
        dest.push_back(' ');

        if (!msg.source.empty()) {
            dest.push_back('[');
            const char* filename = details::source_basename(msg.source.filename);
            fmt_helper::append_string_view(filename, dest);
            dest.push_back(':');
            fmt_helper::append_int(msg.source.line, dest);
            dest.push_back(']');
            dest.push_back(' ');
        }

#ifndef SPDLOG_NO_TLS
        auto& mdc_map = mdc::get_context();
        if (!mdc_map.empty()) {
            dest.push_back('[');
            mdc_formatter_.format_mdc(mdc_map, dest);
            dest.push_back(']');
            dest.push_back(' ');
        }
#endif
        fmt_helper::append_string_view(msg.payload, dest);
    }

   private:
    std::chrono::seconds cache_timestamp_{0};
    memory_buf_t cached_datetime_;

#ifndef SPDLOG_NO_TLS
    mdc_formatter<null_scoped_padder> mdc_formatter_{padding_info {}};
#endif
};

}

SPDLOG_INLINE pattern_formatter::pattern_formatter(std::string pattern, pattern_time_type time_type, std::string eol,
                                                   custom_flags custom_user_flags)
    : pattern_(std::move(pattern)),
      eol_(std::move(eol)),
      pattern_time_type_(time_type),
      need_localtime_(false),
      last_log_secs_(0),
      custom_handlers_(std::move(custom_user_flags)) {
    std::memset(&cached_tm_, 0, sizeof(cached_tm_));
    compile_pattern_(pattern_);
}

SPDLOG_INLINE pattern_formatter::pattern_formatter(pattern_time_type time_type, std::string eol)
    : pattern_("%+"), eol_(std::move(eol)), pattern_time_type_(time_type), need_localtime_(true), last_log_secs_(0) {
    std::memset(&cached_tm_, 0, sizeof(cached_tm_));
    formatters_.push_back(details::make_unique<details::full_formatter>(details::padding_info{}));
}

SPDLOG_INLINE std::unique_ptr<formatter> pattern_formatter::clone() const {
    custom_flags cloned_custom_formatters;
    for (auto& it : custom_handlers_) {
        cloned_custom_formatters[it.first] = it.second->clone();
    }
    auto cloned = details::make_unique<pattern_formatter>(pattern_, pattern_time_type_, eol_,
                                                          std::move(cloned_custom_formatters));
    cloned->need_localtime(need_localtime_);
#if defined(__GNUC__) && __GNUC__ < 5
    return std::move(cloned);
#else
    return cloned;
#endif
}

SPDLOG_INLINE void pattern_formatter::format(const details::log_msg& msg, memory_buf_t& dest) {
    if (need_localtime_) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(msg.time.time_since_epoch());
        if (secs != last_log_secs_) {
            cached_tm_ = get_time_(msg);
            last_log_secs_ = secs;
        }
    }

    for (auto& f : formatters_) {
        f->format(msg, cached_tm_, dest);
    }
    details::fmt_helper::append_string_view(eol_, dest);
}

SPDLOG_INLINE void pattern_formatter::set_pattern(std::string pattern) {
    pattern_ = std::move(pattern);
    need_localtime_ = false;
    compile_pattern_(pattern_);
}

SPDLOG_INLINE void pattern_formatter::need_localtime(bool need) {
    need_localtime_ = need;
}

SPDLOG_INLINE std::tm pattern_formatter::get_time_(const details::log_msg& msg) const {
    if (pattern_time_type_ == pattern_time_type::local) {
        return details::os::localtime(log_clock::to_time_t(msg.time));
    }
    return details::os::gmtime(log_clock::to_time_t(msg.time));
}

template <typename Padder>
SPDLOG_INLINE void pattern_formatter::handle_flag_(char flag, details::padding_info padding) {
    auto it = custom_handlers_.find(flag);
    if (it != custom_handlers_.end()) {
        auto custom_handler = it->second->clone();
        custom_handler->set_padding_info(padding);
        formatters_.push_back(std::move(custom_handler));
        return;
    }

    switch (flag) {
        case ('+'):
            formatters_.push_back(details::make_unique<details::full_formatter>(padding));
            need_localtime_ = true;
            break;

        case 'n':
            formatters_.push_back(details::make_unique<details::name_formatter<Padder>>(padding));
            break;

        case 'l':
            formatters_.push_back(details::make_unique<details::level_formatter<Padder>>(padding));
            break;

        case 'L':
            formatters_.push_back(details::make_unique<details::short_level_formatter<Padder>>(padding));
            break;

        case ('t'):
            formatters_.push_back(details::make_unique<details::t_formatter<Padder>>(padding));
            break;

        case ('v'):
            formatters_.push_back(details::make_unique<details::v_formatter<Padder>>(padding));
            break;

        case ('a'):
            formatters_.push_back(details::make_unique<details::a_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('A'):
            formatters_.push_back(details::make_unique<details::A_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('b'):
        case ('h'):
            formatters_.push_back(details::make_unique<details::b_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('B'):
            formatters_.push_back(details::make_unique<details::B_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('c'):
            formatters_.push_back(details::make_unique<details::c_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('C'):
            formatters_.push_back(details::make_unique<details::C_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('Y'):
            formatters_.push_back(details::make_unique<details::Y_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('D'):
        case ('x'):
            formatters_.push_back(details::make_unique<details::D_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('m'):
            formatters_.push_back(details::make_unique<details::m_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('d'):
            formatters_.push_back(details::make_unique<details::d_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('H'):
            formatters_.push_back(details::make_unique<details::H_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('I'):
            formatters_.push_back(details::make_unique<details::I_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('M'):
            formatters_.push_back(details::make_unique<details::M_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('S'):
            formatters_.push_back(details::make_unique<details::S_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('e'):
            formatters_.push_back(details::make_unique<details::e_formatter<Padder>>(padding));
            break;

        case ('f'):
            formatters_.push_back(details::make_unique<details::f_formatter<Padder>>(padding));
            break;

        case ('F'):
            formatters_.push_back(details::make_unique<details::F_formatter<Padder>>(padding));
            break;

        case ('E'):
            formatters_.push_back(details::make_unique<details::E_formatter<Padder>>(padding));
            break;

        case ('p'):
            formatters_.push_back(details::make_unique<details::p_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('r'):
            formatters_.push_back(details::make_unique<details::r_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('R'):
            formatters_.push_back(details::make_unique<details::R_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;

        case ('T'):
        case ('X'):
            formatters_.push_back(details::make_unique<details::T_formatter<Padder>>(padding));
            need_localtime_ = true;
            break;
        case ('z'):
            formatters_.push_back(details::make_unique<details::z_formatter<Padder>>(padding, pattern_time_type_));
            need_localtime_ = true;
            break;
        case ('P'):
            formatters_.push_back(details::make_unique<details::pid_formatter<Padder>>(padding));
            break;

        case ('^'):
            formatters_.push_back(details::make_unique<details::color_start_formatter>(padding));
            break;

        case ('$'):
            formatters_.push_back(details::make_unique<details::color_stop_formatter>(padding));
            break;

        case ('@'):
            formatters_.push_back(details::make_unique<details::source_location_formatter<Padder>>(padding));
            break;

        case ('s'):
            formatters_.push_back(details::make_unique<details::short_filename_formatter<Padder>>(padding));
            break;

        case ('g'):
            formatters_.push_back(details::make_unique<details::source_filename_formatter<Padder>>(padding));
            break;

        case ('#'):
            formatters_.push_back(details::make_unique<details::source_linenum_formatter<Padder>>(padding));
            break;

        case ('!'):
            formatters_.push_back(details::make_unique<details::source_funcname_formatter<Padder>>(padding));
            break;

        case ('%'):
            formatters_.push_back(details::make_unique<details::ch_formatter>('%'));
            break;

        case ('u'):
            formatters_.push_back(
                details::make_unique<details::elapsed_formatter<Padder, std::chrono::nanoseconds>>(padding));
            break;

        case ('i'):
            formatters_.push_back(
                details::make_unique<details::elapsed_formatter<Padder, std::chrono::microseconds>>(padding));
            break;

        case ('o'):
            formatters_.push_back(
                details::make_unique<details::elapsed_formatter<Padder, std::chrono::milliseconds>>(padding));
            break;

        case ('O'):
            formatters_.push_back(
                details::make_unique<details::elapsed_formatter<Padder, std::chrono::seconds>>(padding));
            break;

#ifndef SPDLOG_NO_TLS  // mdc formatter requires TLS support
        case ('&'):
            formatters_.push_back(details::make_unique<details::mdc_formatter<Padder>>(padding));
            break;
#endif

        default:
            auto unknown_flag = details::make_unique<details::aggregate_formatter>();

            if (!padding.truncate_) {
                unknown_flag->add_ch('%');
                unknown_flag->add_ch(flag);
                formatters_.push_back((std::move(unknown_flag)));
            } else {
                padding.truncate_ = false;
                formatters_.push_back(details::make_unique<details::source_funcname_formatter<Padder>>(padding));
                unknown_flag->add_ch(flag);
                formatters_.push_back((std::move(unknown_flag)));
            }

            break;
    }
}

SPDLOG_INLINE details::padding_info pattern_formatter::handle_padspec_(std::string::const_iterator& it,
                                                                       std::string::const_iterator end) {
    using details::padding_info;
    using details::scoped_padder;
    const size_t max_width = 64;
    if (it == end) {
        return padding_info{};
    }

    padding_info::pad_side side;
    switch (*it) {
        case '-':
            side = padding_info::pad_side::right;
            ++it;
            break;
        case '=':
            side = padding_info::pad_side::center;
            ++it;
            break;
        default:
            side = details::padding_info::pad_side::left;
            break;
    }

    if (it == end || !std::isdigit(static_cast<unsigned char>(*it))) {
        return padding_info{};
    }

    auto width = static_cast<size_t>(*it) - '0';
    for (++it; it != end && std::isdigit(static_cast<unsigned char>(*it)); ++it) {
        auto digit = static_cast<size_t>(*it) - '0';
        width = width * 10 + digit;
    }

    bool truncate;
    if (it != end && *it == '!') {
        truncate = true;
        ++it;
    } else {
        truncate = false;
    }
    return details::padding_info{std::min<size_t>(width, max_width), side, truncate};
}

SPDLOG_INLINE void pattern_formatter::compile_pattern_(const std::string& pattern) {
    auto end = pattern.end();
    std::unique_ptr<details::aggregate_formatter> user_chars;
    formatters_.clear();
    for (auto it = pattern.begin(); it != end; ++it) {
        if (*it == '%') {
            if (user_chars) {
                formatters_.push_back(std::move(user_chars));
                user_chars.reset();
            }

            auto padding = handle_padspec_(++it, end);

            if (it != end) {
                if (padding.enabled()) {
                    handle_flag_<details::scoped_padder>(*it, padding);
                } else {
                    handle_flag_<details::null_scoped_padder>(*it, padding);
                }
            } else {
                break;
            }
        } else {
            if (!user_chars) {
                user_chars = details::make_unique<details::aggregate_formatter>();
            }
            user_chars->add_ch(*it);
        }
    }
    if (user_chars) {
        formatters_.push_back(std::move(user_chars));
    }
}
}
