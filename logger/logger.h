#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <mutex>
#include <string_view>

// ── logger ─────────────────────────────────────────────────────────────────────
//
//  A minimal, header-only, globally-enabled structured logger.
//
//  Usage:
//      logger::SetEnabled(true);
//      logger::Info("webview created: {}x{}", width, height);
//      logger::Warn("channel '{}' not found", ch);
//      logger::Error("HRESULT failed: 0x{:08X}", hr);
//      logger::Debug("slot key: {}", key);
//
//  Enable / disable at any time — all calls are no-ops when disabled.

namespace logger {

namespace detail {

    inline bool      g_enabled = false;
    inline std::mutex g_mutex;

        inline std::string timestamp()
        {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char buf[20];
            std::tm tm{};
        #ifdef _WIN32
            localtime_s(&tm, &t);
        #else
            localtime_r(&t, &tm);
        #endif
            std::strftime(buf, sizeof(buf), "%F %T", &tm);
            return buf;
        }

    enum class Level { Info, Warn, Error, Debug };

    inline const char* level_tag(Level l)
    {
        switch (l) {
            case Level::Info:  return "INFO ";
            case Level::Warn:  return "WARN ";
            case Level::Error: return "ERROR";
            case Level::Debug: return "DEBUG";
        }
        return "?????";
    }

    inline void emit(Level level, std::string_view msg)
    {
        if (!g_enabled) return;
        std::lock_guard lock(g_mutex);
        std::fprintf(stdout, "[%s] [%s] %.*s\n",
                     timestamp().c_str(),
                     level_tag(level),
                     static_cast<int>(msg.size()), msg.data());
        std::fflush(stdout);
    }

} // namespace detail

// ── public API ────────────────────────────────────────────────────────────────

inline void SetEnabled(bool enabled) { detail::g_enabled = enabled; }
inline bool IsEnabled()              { return detail::g_enabled;    }

template<typename... Args>
void Info(std::format_string<Args...> fmt, Args&&... args)
{
    if (!detail::g_enabled) return;
    detail::emit(detail::Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args)
{
    if (!detail::g_enabled) return;
    detail::emit(detail::Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
void Error(std::format_string<Args...> fmt, Args&&... args)
{
    if (!detail::g_enabled) return;
    detail::emit(detail::Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
void Debug(std::format_string<Args...> fmt, Args&&... args)
{
    if (!detail::g_enabled) return;
    detail::emit(detail::Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace logger