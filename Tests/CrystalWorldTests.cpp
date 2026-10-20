#include "../Source/CrystalWorld.h"
#include <cassert>
#include <iostream>
int main()
{
    CrystalWorld world;
    world.Load("Main_LevelObjects.json");
    assert(world.Packets().size() == 3);
    std::ifstream stream("Main_LevelObjects.json");
    const auto data = nlohmann::json::parse(stream);
    const auto& first = data.at("crystal_towers")[0];
    const auto id = first.at("actor_name").get<std::string>();
    const auto& p = first.at("position");
    PlayerRegistration a;
    a.PlayerId = std::string(32,'a'); a.Address = "127.0.0.1"; a.Port = 9001; a.HasState = true;
    a.LatestState.Position = {p.at("x"),p.at("y"),p.at("z")};
    auto b = a; b.PlayerId = std::string(32,'b'); b.Port = 9002;
    std::vector<PlayerRegistration> players{a,b};
    const auto hit = [&](const PlayerRegistration& player, unsigned sequence, unsigned cycle = 0) {
        return "SA5|type=CRYSTAL_HIT|epoch="+world.Epoch+"|session="+player.PlayerId+"|id="+id
            +"|seq="+std::to_string(sequence)+"|cycle="+std::to_string(cycle);
    };
    const auto state = [&] {
        for (const auto& packet : world.Packets()) if (packet.find("|id="+id+"|") != std::string::npos) return packet;
        return std::string{};
    };
    assert(!world.Hit(hit(a,1), {a.Address,9003},players));
    auto far = players; far[0].LatestState.Position.X += 10000;
    assert(!world.Hit(hit(a,1),{a.Address,a.Port},far));
    assert(world.Hit(hit(a,1),{a.Address,a.Port},players));
    assert(world.Hit(hit(a,1),{a.Address,a.Port},players));
    assert(state().find("|hits=1|lit=1|") != std::string::npos);
    assert(!world.Hit(hit(a,2),{a.Address,a.Port},players));
    for (unsigned i=2;i<=4;++i) {
        world.Tick(.21);
        assert(world.Hit(hit(a,i),{a.Address,a.Port},players));
    }
    assert(state().find("|hits=4|lit=1|") != std::string::npos);
    assert(world.Hit(hit(b,1),{b.Address,b.Port},players));
    const auto dark = state();
    assert(dark.find("|hits=5|lit=0|winner="+b.PlayerId) != std::string::npos);
    assert(world.Hit(hit(a,5),{a.Address,a.Port},players));
    assert(state() == dark); // Winner/effect cannot change while dark.
    assert(!world.Tick(24.99));
    assert(state().find("|lit=0|") != std::string::npos);
    assert(world.Tick(10.02));
    assert(state().find("|cycle=1|hits=0|lit=1|") != std::string::npos);
    assert(world.Hit(hit(a,6),{a.Address,a.Port},players)); // Old-cycle retry is acknowledged only.
    assert(state().find("|hits=0|") != std::string::npos);
    assert(world.Hit(hit(a,7,1),{a.Address,a.Port},players));
    assert(state().find("|hits=1|") != std::string::npos);
    assert(!world.Hit(hit(a,8,1)+"|extra=1",{a.Address,a.Port},players));
    std::cout << "PASS: shared tower count, endpoint/range checks, dedup, cooldown, final hitter, dark lockout, server 25-35s relight, stale cycle rejection\n";
}
