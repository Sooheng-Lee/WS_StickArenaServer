#pragma once
#include "ThirdParty/nlohmann/json.hpp"
#include "PlayerState.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include <stdexcept>

struct TreasureSpawnPoint
{
    std::string Id;
    Vector3 Position;
};

inline std::vector<TreasureSpawnPoint> ReadTreasureSpawnPoints(const std::filesystem::path& path)
{
    if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Spawn JSON exceeds 16 MB");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open spawn JSON");
    const auto root = nlohmann::json::parse(stream);
    if (root.at("schema_version") != 1 || root.at("map") != "/Game/Maps/Main")
        throw std::runtime_error("Unsupported spawn JSON schema or map");
    const auto& treasure = root.at("treasure_box");
    const auto& rules = treasure.at("spawn_rules");
    const auto folder = rules.at("allowed_tile_folder").get<std::string>();
    if (folder.empty() || !rules.at("tiles_only").get<bool>() || rules.at("allow_arbitrary_xy").get<bool>()
        || rules.at("max_boxes_per_point") != 1
        || rules.at("selection") != "random_without_replacement_from_spawn_points"
        || rules.at("fallback_when_no_valid_point") != "skip_spawn")
        throw std::runtime_error("Unsupported treasure spawn rules");
    std::vector<TreasureSpawnPoint> result;
    std::set<std::string> ids;
    std::set<std::pair<double, double>> locations;
    const auto& points = treasure.at("spawn_points");
    if (!points.is_array() || points.empty() || treasure.at("spawn_point_count") != points.size())
        throw std::runtime_error("Empty or inconsistent treasure spawn point list");
    for (const auto& point : points)
    {
        if (point.at("folder") != folder) throw std::runtime_error("Spawn point outside allowed tile folder");
        const auto& position = point.at("position");
        TreasureSpawnPoint parsed{point.at("id").get<std::string>(),
            {position.at("x").get<double>(), position.at("y").get<double>(), position.at("z").get<double>()}};
        if (parsed.Id.empty() || !ids.insert(parsed.Id).second
            || !locations.emplace(parsed.Position.X, parsed.Position.Y).second
            || !std::isfinite(parsed.Position.X) || !std::isfinite(parsed.Position.Y) || !std::isfinite(parsed.Position.Z)
            || std::abs(parsed.Position.X) > 1.e8 || std::abs(parsed.Position.Y) > 1.e8 || std::abs(parsed.Position.Z) > 1.e8)
            throw std::runtime_error("Invalid or duplicate treasure spawn point");
        result.push_back(std::move(parsed));
    }
    return result;
}
