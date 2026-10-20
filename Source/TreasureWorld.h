#pragma once
#include "PlayerRegistration.h"
#include <map>
#include <random>
#include <numeric>
#include "TreasureSpawnData.h"

// Wire enum shared with ESATreasureItem in the Unreal client.
enum class TreasureItem : unsigned { Sword = 0 };
class TreasureWorld
{
public:
    static constexpr unsigned HitsToBreak = 5;
    size_t MaxTreasureBoxes = 5;
    double MinimumPlayerSpawnDistance = 200.0; // XY center distance, Unreal centimeters.
    double MinimumBoxSpawnDistance = 250.0;
    static constexpr double SpawnInterval = 20.0;
    double BoxSpawnCount = 0.0;
    const std::string Epoch = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    void LoadSpawnPoints(const std::filesystem::path& path)
    {
        auto points = ReadTreasureSpawnPoints(path);
        const std::lock_guard<std::mutex> lock(Mutex);
        if (!Objects.empty()) throw std::runtime_error("Spawn points must be loaded before spawning");
        SpawnPoints = std::move(points);
    }
    size_t SpawnPointCount() const { return SpawnPoints.size(); }

    bool Tick(double seconds, const std::vector<PlayerRegistration>& players)
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        size_t count = 0;
        for (const auto& entry : Objects) if (entry.second.Hits < HitsToBreak) ++count;
        if (count >= MaxTreasureBoxes) { BoxSpawnCount = 0; return false; }
        BoxSpawnCount = (std::min)(SpawnInterval, BoxSpawnCount + (std::max)(0.0, seconds));
        if (BoxSpawnCount < SpawnInterval) return false;
        BoxSpawnCount = 0; // No valid point means skip this interval, never arbitrary XY.
        std::vector<size_t> candidates(SpawnPoints.size());
        std::iota(candidates.begin(), candidates.end(), size_t{0});
        std::shuffle(candidates.begin(), candidates.end(), Random);
        for (const size_t candidate : candidates)
        {
            Object box;
            box.Position = SpawnPoints[candidate].Position;
            box.SpawnPointId = SpawnPoints[candidate].Id;
            const auto tooClose = [&](const Vector3& position, double clearance)
            {
                const double dx = position.X - box.Position.X;
                const double dy = position.Y - box.Position.Y;
                return dx * dx + dy * dy < clearance * clearance;
            };
            const bool nearPlayer = std::any_of(players.begin(), players.end(), [&](const auto& player)
                { return player.HasState && tooClose(player.LatestState.Position, MinimumPlayerSpawnDistance); });
            if (nearPlayer) continue;
            const bool nearBox = std::any_of(Objects.begin(), Objects.end(), [&](const auto& entry)
                { return entry.second.Hits < HitsToBreak && (entry.second.SpawnPointId == box.SpawnPointId || tooClose(entry.second.Position, MinimumBoxSpawnDistance)); });
            if (nearBox) continue;
            Objects.emplace(++NextId, std::move(box));
            BoxSpawnCount = 0;
            return true;
        }
        // All exported points are blocked; skip until the next twenty-second interval.
        return false;
    }

    bool Hit(std::string_view packet, const PlayerEndpoint& sender, const std::vector<PlayerRegistration>& players)
    {
        if (packet.size() > 256) return false;
        std::istringstream input{std::string(packet)};
        std::array<std::string, 6> fields;
        for (auto& field : fields) if (!std::getline(input, field, '|')) return false;
        std::string extra;
        if (std::getline(input, extra, '|') || fields[0] != "SA4" || fields[1] != "type=BOX_HIT"
            || fields[2] != "epoch=" + Epoch || fields[3].substr(0, 8) != "session="
            || fields[4].substr(0, 3) != "id=" || fields[5].substr(0, 4) != "seq=") return false;
        std::uint64_t id = 0, seq = 0;
        const auto parse = [](std::string_view s, std::uint64_t& n) {
            const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
            return r.ec == std::errc{} && r.ptr == s.data() + s.size() && n > 0;
        };
        if (!parse(std::string_view(fields[4]).substr(3), id) || !parse(std::string_view(fields[5]).substr(4), seq)) return false;
        const std::string session = fields[3].substr(8);
        const auto player = std::find_if(players.begin(), players.end(), [&](const auto& p) {
            return p.PlayerId == session && p.Address == sender.Address && p.Port == sender.Port && p.HasState;
        });
        if (player == players.end()) return false;
        const std::lock_guard<std::mutex> lock(Mutex);
        const auto found = Objects.find(id);
        if (found == Objects.end()) return false;
        auto& box = found->second;
        if (box.Hits >= HitsToBreak) return true;
        const auto& position = player->LatestState.Position;
        const double dx = position.X - box.Position.X, dy = position.Y - box.Position.Y, dz = position.Z - box.Position.Z;
        if (dx*dx + dy*dy + dz*dz > 350.0*350.0) return false;
        auto& hit = box.LastHits[session];
        if (seq <= hit.Sequence) return true; // Retry must never count twice.
        const auto now = std::chrono::steady_clock::now();
        if (hit.Sequence && now - hit.Time < std::chrono::milliseconds(200)) return false;
        hit = {seq, now};
        ++box.Hits;
        return true;
    }

    // The collected state is permanent for this epoch, including after disconnect.
    // Returning true also sends the existing winner back to a competing claimant.
    bool Pickup(std::string_view packet, const PlayerEndpoint& sender, const std::vector<PlayerRegistration>& players)
    {
        if (packet.size() > 256) return false;
        std::istringstream input{std::string(packet)};
        std::array<std::string, 6> fields;
        for (auto& field : fields) if (!std::getline(input, field, '|')) return false;
        std::string extra;
        if (std::getline(input, extra, '|') || fields[0] != "SA4" || fields[1] != "type=PICKUP"
            || fields[2] != "epoch=" + Epoch || fields[3].substr(0, 8) != "session="
            || fields[4].substr(0, 3) != "id=" || fields[5].substr(0, 4) != "seq=") return false;
        const auto parse = [](std::string_view text, std::uint64_t& value) {
            const auto read = std::from_chars(text.data(), text.data() + text.size(), value);
            return read.ec == std::errc{} && read.ptr == text.data() + text.size() && value > 0;
        };
        std::uint64_t id = 0, sequence = 0;
        if (!parse(std::string_view(fields[4]).substr(3), id) || !parse(std::string_view(fields[5]).substr(4), sequence)) return false;
        const std::string session = fields[3].substr(8);
        const auto player = std::find_if(players.begin(), players.end(), [&](const auto& value) {
            return value.PlayerId == session && value.Address == sender.Address && value.Port == sender.Port && value.HasState;
        });
        if (player == players.end()) return false;
        const std::lock_guard<std::mutex> lock(Mutex);
        const auto found = Objects.find(id);
        if (found == Objects.end() || found->second.Hits != HitsToBreak) return false;
        auto& item = found->second;
        if (!item.Owner.empty()) return true; // Retries and races never transfer ownership.
        const auto& position = player->LatestState.Position;
        const double dx = position.X - item.Position.X, dy = position.Y - item.Position.Y, dz = position.Z - item.Position.Z;
        if (dx*dx + dy*dy + dz*dz > 350.0*350.0) return false;
        item.Owner = session;
        return true;
    }

    std::vector<std::string> Packets() const
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        std::vector<std::string> result;
        for (const auto& entry : Objects)
        {
            const auto& box = entry.second;
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::fixed << std::setprecision(6)
                << "SA4|type=TREASURE_STATE|epoch=" << Epoch << "|id=" << entry.first
                << "|item=" << static_cast<unsigned>(box.Item) << "|hits=" << box.Hits
                << "|pos=" << box.Position.X << ',' << box.Position.Y << ',' << box.Position.Z;
            if (!box.Owner.empty()) out << "|owner=" << box.Owner;
            result.push_back(out.str());
        }
        return result;
    }
private:
    struct LastHit { std::uint64_t Sequence = 0; std::chrono::steady_clock::time_point Time{}; };
    struct Object {
        TreasureItem Item = TreasureItem::Sword;
        Vector3 Position;
        unsigned Hits = 0;
        std::string SpawnPointId;
        std::string Owner;
        std::map<std::string, LastHit> LastHits;
    };
    mutable std::mutex Mutex;
    std::map<std::uint64_t, Object> Objects;
    std::vector<TreasureSpawnPoint> SpawnPoints;
    std::uint64_t NextId = 0;
    std::mt19937 Random{std::random_device{}()};
};
