#include "../Source/TreasureWorld.h"
#include <cassert>
#include <thread>
#include <iostream>
int main()
{
    TreasureWorld world;
    world.LoadSpawnPoints("Main_LevelObjects.json");
    assert(world.SpawnPointCount() == 112);
    TreasureWorld emptyServer;
    emptyServer.LoadSpawnPoints("Main_LevelObjects.json");
    assert(!emptyServer.Tick(19.99, {}));
    assert(emptyServer.Tick(.02, {})); // Startup timer does not depend on a player joining.
    std::ifstream source("Main_LevelObjects.json");
    const auto sourceJson = nlohmann::json::parse(source);
    const auto rejects = [](const nlohmann::json& data) {
        const std::filesystem::path path = "obj/TreasureSpawnInvalid.json";
        { std::ofstream output(path); output << data; }
        bool rejected = false;
        try { ReadTreasureSpawnPoints(path); }
        catch (const std::exception&) { rejected = true; }
        std::filesystem::remove(path);
        assert(rejected);
    };
    auto invalid = sourceJson;
    invalid["map"] = "/Game/Other";
    rejects(invalid);
    invalid = sourceJson;
    invalid["treasure_box"]["spawn_points"] = nlohmann::json::array();
    invalid["treasure_box"]["spawn_point_count"] = 0;
    rejects(invalid);
    invalid = sourceJson;
    invalid["treasure_box"]["spawn_points"][1] = invalid["treasure_box"]["spawn_points"][0];
    rejects(invalid);
    invalid = sourceJson;
    invalid["treasure_box"]["spawn_points"][0]["folder"] = "Outside";
    rejects(invalid);
    bool missingRejected = false;
    try { ReadTreasureSpawnPoints("obj/MissingSpawnData.json"); }
    catch (const std::exception&) { missingRejected = true; }
    assert(missingRejected);
    PlayerRegistration p;
    p.PlayerId = std::string(32, 'a'); p.Address = "127.0.0.1"; p.Port = 9001;
    p.HasState = true; p.LatestState.Position = {0,0,88};
    std::vector<PlayerRegistration> players{p};
    world.Tick(19.99, players); assert(world.Packets().empty());
    world.Tick(.02, players); assert(world.Packets().size() == 1);
    for (int i = 0; i < 4; ++i) world.Tick(20.0, players);
    assert(world.Packets().size() == 5);
    const auto readPosition = [](const std::string& packet) {
        Vector3 result;
        assert(StateProtocol::ParseVector(packet.substr(packet.find("|pos=") + 5), result));
        return result;
    };
    const auto distanceXY = [](const Vector3& a, const Vector3& b) {
        const double dx = a.X-b.X, dy = a.Y-b.Y;
        return std::sqrt(dx*dx + dy*dy);
    };
    const auto exported = ReadTreasureSpawnPoints("Main_LevelObjects.json");
    const auto boxes = world.Packets();
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const auto a = readPosition(boxes[i]);
        assert(std::any_of(exported.begin(), exported.end(), [&](const auto& point) {
            return distanceXY(a, point.Position) < 0.00001 && std::abs(a.Z - point.Position.Z) < 0.00001;
        }));
        assert(distanceXY(a, p.LatestState.Position) >= world.MinimumPlayerSpawnDistance - 0.00001);
        for (size_t j = 0; j < i; ++j)
            assert(distanceXY(a, readPosition(boxes[j])) >= world.MinimumBoxSpawnDistance - 0.00001);
    }
    TreasureWorld crowded;
    crowded.LoadSpawnPoints("Main_LevelObjects.json");
    // Include a second player at a different elevation; clearance is measured in XY.
    auto blockers = players;
    auto blocker = p;
    blocker.PlayerId = std::string(32, 'b');
    blocker.LatestState.Position = {-450,0,10000};
    blockers.push_back(blocker);
    crowded.MinimumPlayerSpawnDistance = 1000000;
    blockers[0].LatestState.Position = {-750,0,88};
    // The anchor itself also excludes all candidates with this clearance.
    assert(!crowded.Tick(20.0, blockers));
    assert(crowded.Packets().empty() && crowded.BoxSpawnCount == 0.0);
    crowded.MinimumPlayerSpawnDistance = 200;
    assert(!crowded.Tick(0.0, blockers));
    assert(crowded.Tick(20.0, blockers));
    assert(crowded.BoxSpawnCount == 0 && crowded.Packets().size() == 1);
    for (const auto& player : blockers)
        assert(distanceXY(readPosition(crowded.Packets()[0]), player.LatestState.Position) >= 200 - 0.00001);
    world.Tick(100.0, players); assert(world.Packets().size() == 5 && world.BoxSpawnCount == 0);
    crowded.MinimumBoxSpawnDistance = 1000000;
    assert(!crowded.Tick(20.0, blockers));
    assert(crowded.Packets().size() == 1 && crowded.BoxSpawnCount == 0.0);
    crowded.MinimumBoxSpawnDistance = 250;
    assert(!crowded.Tick(0.0, blockers));
    assert(crowded.Tick(20.0, blockers));
    assert(crowded.Packets().size() == 2 && crowded.BoxSpawnCount == 0);
    TreasureWorld occupied;
    occupied.LoadSpawnPoints("Main_LevelObjects.json");
    std::vector<PlayerRegistration> pointBlockers;
    for (const auto& point : exported)
    {
        auto player = p;
        player.LatestState.Position = point.Position;
        pointBlockers.push_back(player);
    }
    assert(!occupied.Tick(20, pointBlockers) && occupied.Packets().empty());
    pointBlockers[0].LatestState.Position = {1.e7,1.e7,0};
    assert(!occupied.Tick(19.99, pointBlockers));
    assert(occupied.Tick(.02, pointBlockers));
    assert(distanceXY(readPosition(occupied.Packets()[0]), exported[0].Position) < .00001);
    const auto request = [&](unsigned seq) { return "SA4|type=BOX_HIT|epoch=" + world.Epoch
        + "|session=" + p.PlayerId + "|id=1|seq=" + std::to_string(seq); };
    const PlayerEndpoint sender{p.Address,p.Port};
    assert(!world.Hit(request(1), {"127.0.0.1",9002}, players));
    auto far = players; far[0].LatestState.Position = {100000,0,0};
    assert(!world.Hit(request(1), sender, far));
    // Move onto the randomly generated box to remove distance from hit-count assertions.
    auto first = world.Packets()[0];
    Vector3 position; assert(StateProtocol::ParseVector(first.substr(first.find("|pos=")+5), position));
    players[0].LatestState.Position = position;
    assert(world.Hit(request(1), sender, players));
    assert(world.Hit(request(1), sender, players));
    assert(world.Packets()[0].find("|hits=1|") != std::string::npos);
    assert(!world.Hit(request(2), sender, players));
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    assert(world.Hit(request(2), sender, players));
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    assert(world.Hit(request(3), sender, players));
    assert(world.Packets()[0].find("|item=0|hits=3|") != std::string::npos);
    const std::string earlyPickup = "SA4|type=PICKUP|epoch=" + world.Epoch + "|session=" + p.PlayerId + "|id=1|seq=1";
    assert(!world.Pickup(earlyPickup, sender, players));
    for (unsigned hit = 4; hit <= 5; ++hit)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        assert(world.Hit(request(hit), sender, players));
        if (hit == 4) assert(!world.Pickup(earlyPickup, sender, players));
    }
    assert(world.Packets()[0].find("|item=0|hits=5|") != std::string::npos);
	const std::string pickup = "SA4|type=PICKUP|epoch=" + world.Epoch + "|session=" + p.PlayerId + "|id=1|seq=1";
	assert(!world.Pickup(pickup, {"127.0.0.1",9002}, players));
	assert(!world.Pickup(pickup, sender, far));
	assert(!world.Pickup(pickup + "|extra=1", sender, players));
	assert(world.Pickup(pickup, sender, players));
	assert(world.Pickup(pickup, sender, players));
	assert(world.Packets()[0].find("|owner=" + p.PlayerId) != std::string::npos);
	auto competitor = players[0];
	competitor.PlayerId = std::string(32, 'b'); competitor.Port = 9002;
	players.push_back(competitor);
	assert(world.Pickup("SA4|type=PICKUP|epoch=" + world.Epoch + "|session=" + competitor.PlayerId + "|id=1|seq=1", {competitor.Address,competitor.Port}, players));
	assert(world.Packets()[0].find("|owner=" + p.PlayerId) != std::string::npos);
    assert(!world.Hit(request(4)+"|bad=1", sender, players));
    world.Tick(19.99, players); assert(world.Packets().size() == 5);
    world.Tick(.02, players); assert(world.Packets().size() == 6);
    std::cout << "PASS: player/box XY clearance, blocked spawn retry, 20-second timer, cap, refill, endpoint/range checks, retry dedup, five-hit item transition\n";
}
