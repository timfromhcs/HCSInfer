#include "hcs/logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace hcs {

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Off:
            return "off";
    }
    return "info";
}

std::optional<LogLevel> parse_log_level(std::string_view t) noexcept {
    for (LogLevel l :
         {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Off}) {
        if (to_string(l) == t)
            return l;
    }
    return std::nullopt;
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

static std::string utc_timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

Logger::Logger(std::ostream* stream, LogLevel level) : stream_(stream), level_(level) {}

void Logger::set_level(LogLevel level) {
    std::lock_guard lock(mutex_);
    level_ = level;
}
LogLevel Logger::level() const {
    std::lock_guard lock(mutex_);
    return level_;
}
void Logger::set_stream(std::ostream* stream) {
    std::lock_guard lock(mutex_);
    stream_ = stream;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message,
                 std::initializer_list<LogField> fields) {
    std::lock_guard lock(mutex_);
    if (level == LogLevel::Off || level < level_)
        return;
    std::string line = "{\"ts\":\"" + utc_timestamp() + "\",\"level\":\"" + std::string(to_string(level)) +
                       "\",\"component\":\"" + json_escape(component) + "\",\"msg\":\"" +
                       json_escape(message) + "\"";
    for (const auto& [k, v] : fields) {
        line += ",\"" + json_escape(k) + "\":\"" + json_escape(v) + "\"";
    }
    line += "}\n";
    std::ostream& os = stream_ ? *stream_ : std::cerr;
    os << line;
    os.flush();
}

Logger& global_logger() {
    static Logger instance;
    return instance;
}

}  // namespace hcs
