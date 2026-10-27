#ifndef CLUSTER_TOOLS_APP_FORWARDER_LOG_RING_H
#define CLUSTER_TOOLS_APP_FORWARDER_LOG_RING_H

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

// LogRing - the duty thread never writes to a file descriptor (RC2 S12 G5, NOTE-10): it formats a fixed
// record into this single-producer / single-consumer ring and main drains it to stderr once a second. A full
// ring drops the record and counts it (forwarder_log_dropped_total). No allocation after construction;
// vsnprintf into the slot does not allocate for the integer/string formats the agent uses.

namespace cluster_tools { namespace app { namespace forwarder {

enum class LogLevel : std::uint8_t
{
    INFO = 0,
    WARN = 1,
    ERROR = 2,
    CRITICAL = 3,
};

inline const char *nameOf(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
    }
    return "?";
}

struct LogRecord
{
    LogLevel level = LogLevel::INFO;
    std::int64_t nowNs = 0;
    char text[248] = {};
};

class LogRing
{
public:
    static constexpr std::size_t CAPACITY = 4096;

    LogRing() : m_records(new LogRecord[CAPACITY]) {}

    // Producer (duty thread). False when full (the record is dropped and counted).
    bool push(LogLevel level, std::int64_t nowNs, const char *format, ...) noexcept __attribute__((format(printf, 4, 5)))
    {
        const std::uint64_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail - m_head.load(std::memory_order_acquire) >= CAPACITY)
        {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        LogRecord &r = m_records[tail & (CAPACITY - 1)];
        r.level = level;
        r.nowNs = nowNs;
        va_list args;
        va_start(args, format);
        std::vsnprintf(r.text, sizeof(r.text), format, args);
        va_end(args);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer (main). False when empty.
    bool pop(LogRecord &out) noexcept
    {
        const std::uint64_t head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire))
        {
            return false;
        }
        out = m_records[head & (CAPACITY - 1)];
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    template<typename Writer>
    std::size_t drain(Writer &&writer)
    {
        LogRecord r;
        std::size_t n = 0;
        while (pop(r))
        {
            writer(r);
            n++;
        }
        return n;
    }

    std::uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<LogRecord[]> m_records;
    std::atomic<std::uint64_t> m_head{ 0 };
    std::atomic<std::uint64_t> m_tail{ 0 };
    std::atomic<std::uint64_t> m_dropped{ 0 };
};

// First and every 1000th occurrence of a repeated event (contract section 3).
inline bool shouldLog(std::uint64_t occurrences) noexcept
{
    return 1 == occurrences || 0 == occurrences % 1000;
}

}}}

#endif
