#pragma once
// Structured JSON-lines logger. Writes to stderr by default (stdout stays clean for
// CLI output and the MCP stdio JSON-RPC stream). NEVER log prompt or image content.
#include <initializer_list>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hcs {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

std::string_view to_string(LogLevel level) noexcept;
std::optional<LogLevel> parse_log_level(std::string_view text) noexcept;

using LogField = std::pair<std::string_view, std::string_view>;

class Logger {
public:
    explicit Logger(std::ostream* stream = nullptr, LogLevel level = LogLevel::Info);  // nullptr => std::cerr

    void set_level(LogLevel level);
    LogLevel level() const;
    void set_stream(std::ostream* stream);  // nullptr => std::cerr

    void log(LogLevel level, std::string_view component, std::string_view message,
             std::initializer_list<LogField> fields = {});

    void info(std::string_view component, std::string_view message, std::initializer_list<LogField> f = {}) {
        log(LogLevel::Info, component, message, f);
    }
    void warn(std::string_view component, std::string_view message, std::initializer_list<LogField> f = {}) {
        log(LogLevel::Warn, component, message, f);
    }
    void error(std::string_view component, std::string_view message, std::initializer_list<LogField> f = {}) {
        log(LogLevel::Error, component, message, f);
    }
    void debug(std::string_view component, std::string_view message, std::initializer_list<LogField> f = {}) {
        log(LogLevel::Debug, component, message, f);
    }

private:
    mutable std::mutex mutex_;
    std::ostream* stream_;
    LogLevel level_;
};

// Process-wide logger (stderr).
Logger& global_logger();

// Exposed for tests: JSON string escaping per RFC 8259.
std::string json_escape(std::string_view text);

}  // namespace hcs
