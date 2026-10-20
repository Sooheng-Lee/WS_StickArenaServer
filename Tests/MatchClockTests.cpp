#include "../Source/MatchClock.h"
#include <cassert>
#include <iostream>

int main()
{
    MatchClock clock;
    const auto start = MatchClock::Clock::now();
    assert(clock.RemainingMs(start + std::chrono::hours(1)) == 180000);
    clock.Start(start);
    assert(clock.RemainingMs(start) == 180000);
    clock.Start(start + std::chrono::seconds(60));
    assert(clock.RemainingMs(start + std::chrono::seconds(60)) == 120000);
    assert(clock.RemainingMs(start + std::chrono::milliseconds(179999)) == 1);
    assert(clock.RemainingMs(start + std::chrono::seconds(180)) == 0);
    assert(clock.RemainingMs(start + std::chrono::hours(1)) == 0);
    clock.Start(start + std::chrono::hours(1));
    assert(clock.RemainingMs(start + std::chrono::hours(1)) == 0);
    assert(clock.Packet("123", start).find("|seq=1|remaining_ms=180000") != std::string::npos);
    assert(clock.Packet("123", start + std::chrono::seconds(180)).find("|seq=2|remaining_ms=0") != std::string::npos);
    std::cout << "PASS: three minutes, late join, exact deadline, zero clamp, no auto restart, sequenced snapshots\n";
}
