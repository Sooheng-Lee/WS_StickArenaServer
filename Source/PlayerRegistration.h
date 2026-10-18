#pragma once

#include <map>

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "PlayerState.h"

// Wire: SA2|type=REGISTER|session=<32 hex GUID>|name=<hex UTF-8>
//       |character=<hex UTF-8>|mesh=<hex UTF-8>|anim=<hex UTF-8>
// Asset/class paths identify Unreal resources; no process-local pointers are sent.
struct PlayerRegistration
{
    std::string PlayerId;
    std::string PlayerName;
    std::string CharacterClassPath;
    std::string SkeletalMeshPath;
    std::string AnimInstanceClassPath;
    std::string Address;
    std::uint16_t Port = 0;
    PlayerState LatestState;
    bool HasState = false;
    std::chrono::steady_clock::time_point LastSeen = std::chrono::steady_clock::now();
};

namespace RegistrationProtocol
{
    inline std::string Encode(std::string_view value)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (const unsigned char byte : value)
        {
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 15]);
        }
        return encoded;
    }

    inline std::string SerializePlayerInfo(const PlayerRegistration& player)
    {
        return "SA2|type=PLAYER_INFO|session=" + player.PlayerId + "|name=" + Encode(player.PlayerName)
            + "|character=" + Encode(player.CharacterClassPath) + "|mesh=" + Encode(player.SkeletalMeshPath)
            + "|anim=" + Encode(player.AnimInstanceClassPath);
    }

    inline int HexDigit(char value)
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    inline bool Decode(std::string_view encoded, std::string& decoded)
    {
        if (encoded.size() % 2 != 0 || encoded.size() > 2048) return false;
        decoded.clear();
        for (size_t index = 0; index < encoded.size(); index += 2)
        {
            const int high = HexDigit(encoded[index]);
            const int low = HexDigit(encoded[index + 1]);
            if (high < 0 || low < 0) return false;
            const auto value = static_cast<unsigned char>((high << 4) | low);
            if (value < 32 || value == 127) return false;
            decoded.push_back(static_cast<char>(value));
        }
        return true;
    }

    inline bool Parse(std::string_view packet, PlayerRegistration& result)
    {
        constexpr std::string_view prefix = "SA2|type=REGISTER|";
        if (packet.size() > 8192 || packet.substr(0, prefix.size()) != prefix) return false;
        packet.remove_prefix(prefix.size());
        constexpr std::array<std::string_view, 5> keys = { "session", "name", "character", "mesh", "anim" };
        std::array<std::string_view, 5> values{};
        for (size_t index = 0; index < keys.size(); ++index)
        {
            const size_t separator = packet.find('|');
            const auto field = packet.substr(0, separator);
            const size_t equals = field.find('=');
            if (equals == std::string_view::npos || field.substr(0, equals) != keys[index]) return false;
            values[index] = field.substr(equals + 1);
            if (index + 1 < keys.size())
            {
                if (separator == std::string_view::npos) return false;
                packet.remove_prefix(separator + 1);
            }
            else if (separator != std::string_view::npos) return false;
        }
        if (values[0].size() != 32) return false;
        for (const char digit : values[0]) if (HexDigit(digit) < 0) return false;
        PlayerRegistration parsed;
        parsed.PlayerId = values[0];
        if (!Decode(values[1], parsed.PlayerName) || parsed.PlayerName.empty()
            || !Decode(values[2], parsed.CharacterClassPath) || parsed.CharacterClassPath.empty()
            || !Decode(values[3], parsed.SkeletalMeshPath)
            || !Decode(values[4], parsed.AnimInstanceClassPath)) return false;
        result = std::move(parsed);
        return true;
    }
}

struct PlayerEndpoint
{
    std::string Address;
    std::uint16_t Port = 0;
};

// Every access to the vector is synchronized. Never expose references after unlocking.
class PlayerRegistry
{
public:
    enum class StoreResult { Inserted, Updated, EndpointMismatch, SessionClosed };
    struct Removal
    {
        PlayerRegistration Player;
        size_t RemainingPlayers = 0;
    };

    StoreResult Store(PlayerRegistration player)
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = ClosedSessions.begin(); it != ClosedSessions.end();)
            if (now - it->second > std::chrono::seconds(10)) it = ClosedSessions.erase(it);
            else ++it;
        if (ClosedSessions.count(player.PlayerId)) return StoreResult::SessionClosed;
        const auto existing = std::find_if(Players.begin(), Players.end(),
            [&player](const PlayerRegistration& value) { return value.PlayerId == player.PlayerId; });
        if (existing != Players.end())
        {
            if (existing->Address != player.Address || existing->Port != player.Port) return StoreResult::EndpointMismatch;
            // A registration retry must not reset the last accepted sequence/state.
            player.LatestState = existing->LatestState;
            player.HasState = existing->HasState;
            *existing = std::move(player);
            return StoreResult::Updated;
        }
        Players.push_back(std::move(player));
        return StoreResult::Inserted;
    }

    std::vector<PlayerRegistration> GetPlayers() const
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        return Players;
    }

    bool UpdateState(const PlayerState& state, const PlayerEndpoint& sender,
        std::vector<PlayerEndpoint>& recipients)
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        const auto player = std::find_if(Players.begin(), Players.end(),
            [&state](const PlayerRegistration& value) { return value.PlayerId == state.PlayerId; });
        if (player == Players.end() || player->Address != sender.Address || player->Port != sender.Port
            || (player->HasState && (state.Sequence <= player->LatestState.Sequence
                || state.AttackSequence < player->LatestState.AttackSequence))) return false;
        player->LatestState = state;
        player->HasState = true;
        player->LastSeen = std::chrono::steady_clock::now();
        recipients.clear();
        for (const auto& other : Players)
        {
            if (other.PlayerId != state.PlayerId && !(other.Address == sender.Address && other.Port == sender.Port))
                recipients.push_back({other.Address, other.Port});
        }
        return true;
    }

    bool Remove(const std::string& playerId, const PlayerEndpoint& sender, Removal* removal = nullptr)
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        const auto player = std::find_if(Players.begin(), Players.end(), [&](const PlayerRegistration& value)
            { return value.PlayerId == playerId && value.Address == sender.Address && value.Port == sender.Port; });
        if (player == Players.end()) return false;
        if (removal) *removal = {*player, Players.size() - 1};
        ClosedSessions[playerId] = std::chrono::steady_clock::now();
        Players.erase(player);
        return true;
    }

    std::vector<Removal> RemoveInactive(std::chrono::steady_clock::time_point now)
    {
        const std::lock_guard<std::mutex> lock(Mutex);
        std::vector<Removal> removed;
        for (auto player = Players.begin(); player != Players.end();)
        {
            if (now - player->LastSeen > std::chrono::seconds(10))
            {
                removed.push_back({*player, Players.size() - 1});
                player = Players.erase(player);
            }
            else ++player;
        }
        return removed;
    }

private:
    mutable std::mutex Mutex;
    std::vector<PlayerRegistration> Players;
    // Prevent delayed registration datagrams from resurrecting a disconnected session.
    std::map<std::string, std::chrono::steady_clock::time_point> ClosedSessions;
};
