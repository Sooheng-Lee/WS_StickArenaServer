#pragma once
#include "TreasureWorld.h"

// The server owns tower state, reward selection, and the relight clock.
class CrystalWorld
{
public:
    const std::string Epoch = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    void Load(const std::filesystem::path& path)
    {
        if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Tower JSON exceeds 16 MB");
        std::ifstream stream(path);
        const auto root = nlohmann::json::parse(stream);
        if (root.at("schema_version") != 1 || root.at("map") != "/Game/Maps/Main") throw std::runtime_error("Invalid tower map");
        const auto& towers = root.at("crystal_towers");
        if (!towers.is_array() || towers.empty() || root.at("crystal_tower_count") != towers.size()) throw std::runtime_error("Invalid tower count");
        std::map<std::string, Tower> loaded;
        for (const auto& data : towers)
        {
            const auto id = data.at("actor_name").get<std::string>();
            if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
                throw std::runtime_error("Invalid tower id");
            Tower tower;
            const auto& p = data.at("position");
            tower.Position = {p.at("x").get<double>(), p.at("y").get<double>(), p.at("z").get<double>()};
            if (!std::isfinite(tower.Position.X) || !std::isfinite(tower.Position.Y) || !std::isfinite(tower.Position.Z)
                || !loaded.emplace(id, tower).second) throw std::runtime_error("Invalid tower position or duplicate id");
        }
        const std::lock_guard<std::mutex> lock(Mutex);
        Towers = std::move(loaded);
    }

    bool Tick(double seconds)
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        Now += (std::max)(0.0, seconds);
        bool changed = false;
        for (auto& entry : Towers)
        {
            auto& tower = entry.second;
            if (!tower.Lit && Now >= tower.RelightAt)
            {
                tower.Lit = true;
                tower.Hits = 0;
                ++tower.Cycle;
                ++tower.Revision;
                tower.Winner.clear();
                changed = true;
            }
        }
        return changed;
    }

    bool Hit(std::string_view packet, const PlayerEndpoint& sender, const std::vector<PlayerRegistration>& players)
    {
        if (packet.size() > 512) return false;
        std::istringstream input{std::string(packet)};
        std::array<std::string, 7> fields;
        for (auto& field : fields) if (!std::getline(input, field, '|')) return false;
        std::string extra;
        if (std::getline(input, extra, '|') || fields[0] != "SA5" || fields[1] != "type=CRYSTAL_HIT"
            || fields[2] != "epoch=" + Epoch || fields[3].substr(0,8) != "session=" || fields[4].substr(0,3) != "id="
            || fields[5].substr(0,4) != "seq=" || fields[6].substr(0,6) != "cycle=") return false;
        const auto parse = [](std::string_view s, std::uint64_t& n) {
            const auto r = std::from_chars(s.data(), s.data()+s.size(), n);
            return r.ec == std::errc{} && r.ptr == s.data()+s.size();
        };
        std::uint64_t seq = 0, cycle = 0;
        if (!parse(std::string_view(fields[5]).substr(4), seq) || !seq || !parse(std::string_view(fields[6]).substr(6), cycle)) return false;
        const std::string session = fields[3].substr(8);
        const auto player = std::find_if(players.begin(), players.end(), [&](const auto& p) {
            return p.PlayerId == session && p.Address == sender.Address && p.Port == sender.Port && p.HasState;
        });
        if (player == players.end()) return false;
        const std::lock_guard<std::mutex> lock(Mutex);
        const auto found = Towers.find(fields[4].substr(3));
        if (found == Towers.end()) return false;
        auto& tower = found->second;
        if (cycle != tower.Cycle) return true; // Acknowledge stale retries without hitting a new cycle.
        const auto& p = player->LatestState.Position;
        const double dx = p.X-tower.Position.X, dy = p.Y-tower.Position.Y, dz = p.Z-tower.Position.Z;
        if (dx*dx+dy*dy+dz*dz > 350.0*350.0) return false;
        auto& hit = tower.LastHits[session];
        if (seq <= hit.Sequence || !tower.Lit) return true;
        if (hit.Sequence && Now-hit.Time < 0.2) return false;
        hit = {seq, Now};
        ++tower.Hits;
        ++tower.Revision;
        if (tower.Hits == 5)
        {
            tower.Lit = false;
            tower.Winner = session;
            tower.Effect = std::uniform_int_distribution<int>(0,2)(Random);
            tower.RewardUntil = Now + (tower.Effect == 0 ? 5.0 : 20.0);
            tower.RelightAt = Now + std::uniform_real_distribution<double>(25.0,35.0)(Random);
        }
        return true;
    }

    std::vector<std::string> Packets() const
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        std::vector<std::string> packets;
        for (const auto& entry : Towers)
        {
            const auto& t = entry.second;
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::fixed << std::setprecision(6) << "SA5|type=CRYSTAL_STATE|epoch=" << Epoch << "|id=" << entry.first
                << "|rev=" << t.Revision << "|cycle=" << t.Cycle << "|hits=" << t.Hits << "|lit=" << (t.Lit ? 1 : 0)
                << "|winner=" << t.Winner << "|effect=" << t.Effect << "|remaining=" << (t.Lit ? 0.0 : (std::max)(0.0,t.RewardUntil-Now));
            packets.push_back(out.str());
        }
        return packets;
    }
private:
    struct LastHit { std::uint64_t Sequence = 0; double Time = 0; };
    struct Tower
    {
        Vector3 Position;
        unsigned Hits = 0;
        bool Lit = true;
        std::uint64_t Revision = 1, Cycle = 0;
        double RelightAt = 0, RewardUntil = 0;
        std::string Winner;
        int Effect = 0;
        std::map<std::string, LastHit> LastHits;
    };
    mutable std::mutex Mutex;
    std::map<std::string, Tower> Towers;
    double Now = 0;
    std::mt19937 Random{std::random_device{}()};
};
