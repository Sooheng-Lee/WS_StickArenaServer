#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// One match per server process. Late registrations never reset the deadline.
class MatchClock
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::int64_t DurationMs = 180000;

    void Start(Clock::time_point now = Clock::now())
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        if (!Started) { Deadline = now + std::chrono::milliseconds(DurationMs); Started = true; }
    }

    std::int64_t RemainingMs(Clock::time_point now = Clock::now()) const
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        return RemainingLocked(now);
    }

    std::string Packet(const std::string& epoch, Clock::time_point now = Clock::now())
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        return "SA6|type=MATCH_STATE|epoch=" + epoch + "|seq=" + std::to_string(++Sequence)
            + "|remaining_ms=" + std::to_string(RemainingLocked(now));
    }

private:
    std::int64_t RemainingLocked(Clock::time_point now) const
    {
        if (!Started) return DurationMs;
        if (now >= Deadline) return 0;
        return std::chrono::ceil<std::chrono::milliseconds>(Deadline - now).count();
    }
    mutable std::mutex Mutex;
    bool Started = false;
    Clock::time_point Deadline{};
    std::uint64_t Sequence = 0;
};
