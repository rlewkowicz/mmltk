// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <spdlog/details/windows_include.h>
#include <winbase.h>

#include <mutex>
#include <string>
#include <vector>

namespace spdlog {
namespace sinks {

namespace win_eventlog {

namespace internal {

struct local_alloc_t {
    HLOCAL hlocal_;

    SPDLOG_CONSTEXPR local_alloc_t() SPDLOG_NOEXCEPT : hlocal_(nullptr) {}

    local_alloc_t(local_alloc_t const&) = delete;
    local_alloc_t& operator=(local_alloc_t const&) = delete;

    ~local_alloc_t() SPDLOG_NOEXCEPT {
        if (hlocal_) {
            LocalFree(hlocal_);
        }
    }
};

struct win32_error : public spdlog_ex {
    static std::string format(std::string const& user_message, DWORD error_code = GetLastError()) {
        std::string system_message;

        local_alloc_t format_message_result{};
        auto format_message_succeeded = ::FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
            error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&format_message_result.hlocal_, 0, nullptr);

        if (format_message_succeeded && format_message_result.hlocal_) {
            system_message = fmt_lib::format(" ({})", (LPSTR)format_message_result.hlocal_);
        }

        return fmt_lib::format("{}: {}{}", user_message, error_code, system_message);
    }

    explicit win32_error(std::string const& func_name, DWORD error = GetLastError())
        : spdlog_ex(format(func_name, error)) {}
};

struct sid_t {
    std::vector<char> buffer_;

   public:
    sid_t() {}

    static sid_t duplicate_sid(PSID psid) {
        if (!::IsValidSid(psid)) {
            throw_spdlog_ex("sid_t::sid_t(): invalid SID received");
        }

        auto const sid_length{::GetLengthSid(psid)};

        sid_t result;
        result.buffer_.resize(sid_length);
        if (!::CopySid(sid_length, (PSID)result.as_sid(), psid)) {
            SPDLOG_THROW(win32_error("CopySid"));
        }

        return result;
    }

    SID* as_sid() const {
        return buffer_.empty() ? nullptr : (SID*)buffer_.data();
    }

    static sid_t get_current_user_sid() {
        struct process_token_t {
            HANDLE token_handle_ = INVALID_HANDLE_VALUE;
            explicit process_token_t(HANDLE process) {
                if (!::OpenProcessToken(process, TOKEN_QUERY, &token_handle_)) {
                    SPDLOG_THROW(win32_error("OpenProcessToken"));
                }
            }

            ~process_token_t() {
                ::CloseHandle(token_handle_);
            }

        } current_process_token(::GetCurrentProcess());

        DWORD tusize = 0;
        if (::GetTokenInformation(current_process_token.token_handle_, TokenUser, NULL, 0, &tusize)) {
            SPDLOG_THROW(win32_error("GetTokenInformation should fail"));
        }

        std::vector<unsigned char> buffer(static_cast<size_t>(tusize));
        if (!::GetTokenInformation(current_process_token.token_handle_, TokenUser, (LPVOID)buffer.data(), tusize,
                                   &tusize)) {
            SPDLOG_THROW(win32_error("GetTokenInformation"));
        }

        return sid_t::duplicate_sid(((TOKEN_USER*)buffer.data())->User.Sid);
    }
};

struct eventlog {
    static WORD get_event_type(details::log_msg const& msg) {
        switch (msg.level) {
            case level::trace:
            case level::debug:
                return EVENTLOG_SUCCESS;

            case level::info:
                return EVENTLOG_INFORMATION_TYPE;

            case level::warn:
                return EVENTLOG_WARNING_TYPE;

            case level::err:
            case level::critical:
            case level::off:
                return EVENTLOG_ERROR_TYPE;

            default:
                return EVENTLOG_INFORMATION_TYPE;
        }
    }

    static WORD get_event_category(details::log_msg const& msg) {
        return (WORD)msg.level;
    }
};

}  // namespace internal

template <typename Mutex>
class win_eventlog_sink : public base_sink<Mutex> {
   private:
    HANDLE hEventLog_{NULL};
    internal::sid_t current_user_sid_;
    std::string source_;
    DWORD event_id_;

    HANDLE event_log_handle() {
        if (!hEventLog_) {
            hEventLog_ = ::RegisterEventSourceA(nullptr, source_.c_str());
            if (!hEventLog_ || hEventLog_ == (HANDLE)ERROR_ACCESS_DENIED) {
                SPDLOG_THROW(internal::win32_error("RegisterEventSource"));
            }
        }

        return hEventLog_;
    }

   protected:
    void sink_it_(const details::log_msg& msg) override {
        using namespace internal;

        bool succeeded;
        memory_buf_t formatted;
        base_sink<Mutex>::formatter_->format(msg, formatted);
        formatted.push_back('\0');

#ifdef SPDLOG_WCHAR_TO_UTF8_SUPPORT
        wmemory_buf_t buf;
        details::os::utf8_to_wstrbuf(string_view_t(formatted.data(), formatted.size()), buf);

        LPCWSTR lp_wstr = buf.data();
        succeeded = static_cast<bool>(::ReportEventW(event_log_handle(), eventlog::get_event_type(msg),
                                                     eventlog::get_event_category(msg), event_id_,
                                                     current_user_sid_.as_sid(), 1, 0, &lp_wstr, nullptr));
#else
        LPCSTR lp_str = formatted.data();
        succeeded = static_cast<bool>(::ReportEventA(event_log_handle(), eventlog::get_event_type(msg),
                                                     eventlog::get_event_category(msg), event_id_,
                                                     current_user_sid_.as_sid(), 1, 0, &lp_str, nullptr));
#endif

        if (!succeeded) {
            SPDLOG_THROW(win32_error("ReportEvent"));
        }
    }

    void flush_() override {}

   public:
    win_eventlog_sink(std::string const& source, DWORD event_id = 1000) : source_(source), event_id_(event_id) {
        try {
            current_user_sid_ = internal::sid_t::get_current_user_sid();
        } catch (...) {
        }
    }

    ~win_eventlog_sink() {
        if (hEventLog_)
            DeregisterEventSource(hEventLog_);
    }
};

}  // namespace win_eventlog

using win_eventlog_sink_mt = win_eventlog::win_eventlog_sink<std::mutex>;
using win_eventlog_sink_st = win_eventlog::win_eventlog_sink<details::null_mutex>;

}  // namespace sinks
}  // namespace spdlog
