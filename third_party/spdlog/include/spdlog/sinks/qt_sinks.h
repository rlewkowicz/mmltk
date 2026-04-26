// Copyright(c) 2015-present, Gabi Melman, mguludag and spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include "spdlog/common.h"
#include "spdlog/details/log_msg.h"
#include "spdlog/details/synchronous_factory.h"
#include "spdlog/sinks/base_sink.h"
#include <array>

#include <QPlainTextEdit>
#include <QTextEdit>

namespace spdlog {
namespace sinks {
template <typename Mutex>
class qt_sink : public base_sink<Mutex> {
   public:
    qt_sink(QObject* qt_object, std::string meta_method) : qt_object_(qt_object), meta_method_(std::move(meta_method)) {
        if (!qt_object_) {
            throw_spdlog_ex("qt_sink: qt_object is null");
        }
    }

    ~qt_sink() {
        flush_();
    }

   protected:
    void sink_it_(const details::log_msg& msg) override {
        memory_buf_t formatted;
        base_sink<Mutex>::formatter_->format(msg, formatted);
        const string_view_t str = string_view_t(formatted.data(), formatted.size());
        QMetaObject::invokeMethod(
            qt_object_, meta_method_.c_str(), Qt::AutoConnection,
            Q_ARG(QString, QString::fromUtf8(str.data(), static_cast<int>(str.size())).trimmed()));
    }

    void flush_() override {}

   private:
    QObject* qt_object_ = nullptr;
    std::string meta_method_;
};

template <typename Mutex>
class qt_color_sink : public base_sink<Mutex> {
   public:
    qt_color_sink(QTextEdit* qt_text_edit, int max_lines, bool dark_colors = false, bool is_utf8 = false)
        : qt_text_edit_(qt_text_edit), max_lines_(max_lines), is_utf8_(is_utf8) {
        if (!qt_text_edit_) {
            throw_spdlog_ex("qt_color_text_sink: text_edit is null");
        }

        default_color_ = qt_text_edit_->currentCharFormat();
        QTextCharFormat format;
        format.setForeground(dark_colors ? Qt::darkGray : Qt::gray);
        colors_.at(level::trace) = format;
        format.setForeground(dark_colors ? Qt::darkCyan : Qt::cyan);
        colors_.at(level::debug) = format;
        format.setForeground(dark_colors ? Qt::darkGreen : Qt::green);
        colors_.at(level::info) = format;
        format.setForeground(dark_colors ? Qt::darkYellow : Qt::yellow);
        colors_.at(level::warn) = format;
        format.setForeground(Qt::red);
        colors_.at(level::err) = format;
        format.setForeground(Qt::white);
        format.setBackground(Qt::red);
        colors_.at(level::critical) = format;
    }

    ~qt_color_sink() {
        flush_();
    }

    void set_default_color(QTextCharFormat format) {
        default_color_ = format;
    }

    void set_level_color(level::level_enum color_level, QTextCharFormat format) {
        colors_.at(static_cast<size_t>(color_level)) = format;
    }

    QTextCharFormat& get_level_color(level::level_enum color_level) {
        std::lock_guard<Mutex> lock(base_sink<Mutex>::mutex_);
        return colors_.at(static_cast<size_t>(color_level));
    }

    QTextCharFormat& get_default_color() {
        std::lock_guard<Mutex> lock(base_sink<Mutex>::mutex_);
        return default_color_;
    }

   protected:
    struct invoke_params {
        invoke_params(int max_lines, QTextEdit* q_text_edit, QString payload, QTextCharFormat default_color,
                      QTextCharFormat level_color, int color_range_start, int color_range_end)
            : max_lines(max_lines),
              q_text_edit(q_text_edit),
              payload(std::move(payload)),
              default_color(default_color),
              level_color(level_color),
              color_range_start(color_range_start),
              color_range_end(color_range_end) {}
        int max_lines;
        QTextEdit* q_text_edit;
        QString payload;
        QTextCharFormat default_color;
        QTextCharFormat level_color;
        int color_range_start;
        int color_range_end;
    };

    void sink_it_(const details::log_msg& msg) override {
        memory_buf_t formatted;
        base_sink<Mutex>::formatter_->format(msg, formatted);

        const string_view_t str = string_view_t(formatted.data(), formatted.size());
        QString payload;
        int color_range_start = static_cast<int>(msg.color_range_start);
        int color_range_end = static_cast<int>(msg.color_range_end);
        if (is_utf8_) {
            payload = QString::fromUtf8(str.data(), static_cast<int>(str.size()));
            if (msg.color_range_start < msg.color_range_end) {
                color_range_start = QString::fromUtf8(str.data(), static_cast<qsizetype>(msg.color_range_start)).size();
                color_range_end = QString::fromUtf8(str.data(), static_cast<qsizetype>(msg.color_range_end)).size();
            }
        } else {
            payload = QString::fromLatin1(str.data(), static_cast<int>(str.size()));
        }

        invoke_params params{max_lines_,
                             qt_text_edit_,
                             std::move(payload),
                             default_color_,
                             colors_.at(static_cast<size_t>(msg.level)),
                             color_range_start,
                             color_range_end};

        QMetaObject::invokeMethod(qt_text_edit_, [params]() { invoke_method_(params); }, Qt::AutoConnection);
    }

    void flush_() override {}

    static void invoke_method_(invoke_params params) {
        auto* document = params.q_text_edit->document();
        QTextCursor cursor(document);

        while (document->blockCount() > params.max_lines) {
            cursor.select(QTextCursor::BlockUnderCursor);
            cursor.removeSelectedText();
            cursor.deleteChar();
        }

        cursor.movePosition(QTextCursor::End);
        cursor.setCharFormat(params.default_color);

        if (params.color_range_end <= params.color_range_start) {
            cursor.insertText(params.payload);
            return;
        }

        cursor.insertText(params.payload.left(params.color_range_start));

        cursor.setCharFormat(params.level_color);
        cursor.insertText(
            params.payload.mid(params.color_range_start, params.color_range_end - params.color_range_start));

        cursor.setCharFormat(params.default_color);
        cursor.insertText(params.payload.mid(params.color_range_end));
    }

    QTextEdit* qt_text_edit_;
    int max_lines_;
    bool is_utf8_;
    QTextCharFormat default_color_;
    std::array<QTextCharFormat, level::n_levels> colors_;
};

#include "spdlog/details/null_mutex.h"
#include <mutex>

using qt_sink_mt = qt_sink<std::mutex>;
using qt_sink_st = qt_sink<details::null_mutex>;
using qt_color_sink_mt = qt_color_sink<std::mutex>;
using qt_color_sink_st = qt_color_sink<details::null_mutex>;
}  // namespace sinks

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_logger_mt(const std::string& logger_name, QTextEdit* qt_object,
                                            const std::string& meta_method = "append") {
    return Factory::template create<sinks::qt_sink_mt>(logger_name, qt_object, meta_method);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_logger_st(const std::string& logger_name, QTextEdit* qt_object,
                                            const std::string& meta_method = "append") {
    return Factory::template create<sinks::qt_sink_st>(logger_name, qt_object, meta_method);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_logger_mt(const std::string& logger_name, QPlainTextEdit* qt_object,
                                            const std::string& meta_method = "appendPlainText") {
    return Factory::template create<sinks::qt_sink_mt>(logger_name, qt_object, meta_method);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_logger_st(const std::string& logger_name, QPlainTextEdit* qt_object,
                                            const std::string& meta_method = "appendPlainText") {
    return Factory::template create<sinks::qt_sink_st>(logger_name, qt_object, meta_method);
}
template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_logger_mt(const std::string& logger_name, QObject* qt_object,
                                            const std::string& meta_method) {
    return Factory::template create<sinks::qt_sink_mt>(logger_name, qt_object, meta_method);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_logger_st(const std::string& logger_name, QObject* qt_object,
                                            const std::string& meta_method) {
    return Factory::template create<sinks::qt_sink_st>(logger_name, qt_object, meta_method);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_color_logger_mt(const std::string& logger_name, QTextEdit* qt_text_edit,
                                                  int max_lines, bool is_utf8 = false) {
    return Factory::template create<sinks::qt_color_sink_mt>(logger_name, qt_text_edit, max_lines, false, is_utf8);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> qt_color_logger_st(const std::string& logger_name, QTextEdit* qt_text_edit,
                                                  int max_lines, bool is_utf8 = false) {
    return Factory::template create<sinks::qt_color_sink_st>(logger_name, qt_text_edit, max_lines, false, is_utf8);
}

}  // namespace spdlog
