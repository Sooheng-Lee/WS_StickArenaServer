#include "../Source/PlayerRegistration.h"
#include <cassert>
#include <iostream>
int main()
{
    PlayerRegistry registry;
    PlayerRegistration first; first.PlayerId = std::string(32, 'a'); first.Address = "127.0.0.1"; first.Port = 10001;
    auto second = first; second.PlayerId = std::string(32, 'b'); second.Port = 10002;
    assert(registry.Store(first) == PlayerRegistry::StoreResult::Inserted);
    assert(registry.Store(second) == PlayerRegistry::StoreResult::Inserted);
    assert(!registry.Remove(first.PlayerId, {first.Address, second.Port}));
    assert(registry.GetPlayers().size() == 2);
    assert(registry.Remove(first.PlayerId, {first.Address, first.Port}));
    assert(registry.GetPlayers().size() == 1 && registry.GetPlayers()[0].PlayerId == second.PlayerId);
    assert(!registry.Remove(first.PlayerId, {first.Address, first.Port}));
    assert(registry.Store(first) == PlayerRegistry::StoreResult::SessionClosed);
    PlayerState stale; stale.PlayerId = first.PlayerId; stale.Sequence = 1;
    std::vector<PlayerEndpoint> recipients;
    assert(!registry.UpdateState(stale, {first.Address,first.Port}, recipients));
    first.PlayerId = std::string(32, 'c');
    assert(registry.Store(first) == PlayerRegistry::StoreResult::Inserted);
    assert(registry.GetPlayers().size() == 2);
    const auto expired = registry.RemoveInactive(std::chrono::steady_clock::now() + std::chrono::seconds(11));
    assert(expired.size() == 2 && registry.GetPlayers().empty());
    assert(expired[0].Player.PlayerId == second.PlayerId && expired[0].RemainingPlayers == 1);
    assert(expired[1].Player.PlayerId == first.PlayerId && expired[1].RemainingPlayers == 0);
    assert(registry.RemoveInactive(std::chrono::steady_clock::now() + std::chrono::seconds(12)).empty());
    std::cout << "PASS: immediate vector removal, endpoint validation, duplicate leave, delayed registration/state rejection, new-session reconnect, timeout removal metadata/counts and no duplicate timeout\n";
}
