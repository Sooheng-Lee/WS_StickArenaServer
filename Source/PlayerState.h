#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

struct Vector3
{
    double X = 0;
    double Y = 0;
    double Z = 0;
};

struct PlayerState
{
    std::string PlayerId;
    std::uint64_t Sequence = 0;
    std::uint64_t AttackSequence = 0;
    std::string Action;
    std::string State;
    Vector3 MovementInput;
    Vector3 Position;
    Vector3 Velocity;
    // X = Pitch, Y = Yaw, Z = Roll (degrees).
    Vector3 Rotation;
    bool HasWeaponState = false;
    // UTF-8 asset path encoded as hex, matching the registration protocol.
    std::string WeaponClassHex;
    std::string WeaponInstanceId;
    std::uint32_t Ammo = 0;
    std::uint32_t MaxAmmo = 0;
    bool HasStatusState = false;
    double HP = 100;
    double MaxHP = 100;
    double DefaultDamage = 5;
    double Defense = 2;
};

namespace StateProtocol
{
    inline bool ParseNumber(std::string_view text, double& value)
    {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
    }

    inline bool ParseVector(std::string_view text, Vector3& value)
    {
        const size_t first = text.find(',');
        if (first == std::string_view::npos) return false;
        const size_t second = text.find(',', first + 1);
        if (second == std::string_view::npos) return false;
        return ParseNumber(text.substr(0, first), value.X)
            && ParseNumber(text.substr(first + 1, second - first - 1), value.Y)
            && ParseNumber(text.substr(second + 1), value.Z);
    }

    inline bool Parse(std::string_view packet, PlayerState& result)
    {
        constexpr std::string_view prefix = "SA3|type=PLAYER_STATE|";
        if (packet.size() > 2048 || packet.substr(0, prefix.size()) != prefix) return false;
        packet.remove_prefix(prefix.size());
        constexpr std::array<std::string_view, 9> keys = {
            "session", "seq", "action", "state", "input", "pos", "vel", "rot", "attack" };
        std::array<std::string_view, 9> values{};
        bool hasEquipment = false;
        bool hasStatus = false;
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
            else if (separator != std::string_view::npos)
            {
                hasEquipment = true;
                packet.remove_prefix(separator + 1);
            }
        }
        if (values[0].size() != 32) return false;
        for (const char digit : values[0])
            if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F'))) return false;
        PlayerState parsed;
        parsed.PlayerId = values[0];
        const auto sequence = std::from_chars(values[1].data(), values[1].data() + values[1].size(), parsed.Sequence);
        if (sequence.ec != std::errc{} || sequence.ptr != values[1].data() + values[1].size()
            || parsed.Sequence == 0 || parsed.Sequence > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return false;
        if (values[2] != "STATE" && values[2] != "MOVE" && values[2] != "STOP" && values[2] != "ATTACK") return false;
        if (values[3] != "IDLE" && values[3] != "MOVING" && values[3] != "FALLING" && values[3] != "ATTACKING") return false;
        parsed.Action = values[2];
        parsed.State = values[3];
        if (!ParseVector(values[4], parsed.MovementInput) || !ParseVector(values[5], parsed.Position)
            || !ParseVector(values[6], parsed.Velocity) || !ParseVector(values[7], parsed.Rotation)) return false;
        const auto attack = std::from_chars(values[8].data(), values[8].data() + values[8].size(), parsed.AttackSequence);
        if (attack.ec != std::errc{} || attack.ptr != values[8].data() + values[8].size()
            || parsed.AttackSequence > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return false;
        if (hasEquipment)
        {
            constexpr std::array<std::string_view, 4> equipmentKeys = { "weapon", "weaponid", "ammo", "maxammo" };
            std::array<std::string_view, 4> equipment{};
            for (size_t index = 0; index < equipment.size(); ++index)
            {
                const size_t separator = packet.find('|');
                const auto field = packet.substr(0, separator);
                const size_t equals = field.find('=');
                if (equals == std::string_view::npos || field.substr(0, equals) != equipmentKeys[index]) return false;
                equipment[index] = field.substr(equals + 1);
                if (index + 1 < equipment.size())
                {
                    if (separator == std::string_view::npos) return false;
                    packet.remove_prefix(separator + 1);
                }
                else if (separator != std::string_view::npos)
                {
                    hasStatus = true;
                    packet.remove_prefix(separator + 1);
                }
            }
            const auto isHex = [](std::string_view text)
            {
                for (char c : text)
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
                return true;
            };
            if (equipment[0].size() > 2048 || equipment[0].size() % 2 != 0 || !isHex(equipment[0])) return false;
            if (!equipment[0].empty())
            {
                if (equipment[1].size() != 32 || !isHex(equipment[1])) return false;
            }
            else if (!equipment[1].empty()) return false;
            const auto parseAmmo = [](std::string_view text, std::uint32_t& value)
            {
                const auto read = std::from_chars(text.data(), text.data() + text.size(), value);
                return read.ec == std::errc{} && read.ptr == text.data() + text.size() && value <= 1000000;
            };
            if (!parseAmmo(equipment[2], parsed.Ammo) || !parseAmmo(equipment[3], parsed.MaxAmmo)
                || parsed.Ammo > parsed.MaxAmmo) return false;
            if (equipment[0].empty() && (parsed.Ammo != 0 || parsed.MaxAmmo != 0)) return false;
            parsed.HasWeaponState = true;
            parsed.WeaponClassHex = equipment[0];
            parsed.WeaponInstanceId = equipment[1];
        }
        if (hasStatus)
        {
            constexpr std::array<std::string_view, 4> statusKeys = { "hp", "maxhp", "damage", "defense" };
            const std::array<double*, 4> statusValues = { &parsed.HP, &parsed.MaxHP, &parsed.DefaultDamage, &parsed.Defense };
            for (size_t index = 0; index < statusKeys.size(); ++index)
            {
                const size_t separator = packet.find('|');
                const auto field = packet.substr(0, separator);
                const size_t equals = field.find('=');
                if (equals == std::string_view::npos || field.substr(0, equals) != statusKeys[index]
                    || !ParseNumber(field.substr(equals + 1), *statusValues[index])
                    || *statusValues[index] < 0 || *statusValues[index] > 1000000) return false;
                if (index + 1 < statusKeys.size())
                {
                    if (separator == std::string_view::npos) return false;
                    packet.remove_prefix(separator + 1);
                }
                else if (separator != std::string_view::npos) return false;
            }
            if (parsed.HP > parsed.MaxHP) return false;
            parsed.HasStatusState = true;
        }
        result = std::move(parsed);
        return true;
    }

    inline std::string Serialize(const PlayerState& state)
    {
        std::ostringstream packet;
        packet.imbue(std::locale::classic());
        packet << std::setprecision(17) << "SA3|type=PLAYER_STATE|session=" << state.PlayerId
            << "|seq=" << state.Sequence << "|action=" << state.Action << "|state=" << state.State;
        const auto vector = [&packet](const char* key, const Vector3& value)
        {
            packet << '|' << key << '=' << value.X << ',' << value.Y << ',' << value.Z;
        };
        vector("input", state.MovementInput);
        vector("pos", state.Position);
        vector("vel", state.Velocity);
        vector("rot", state.Rotation);
        packet << "|attack=" << state.AttackSequence;
        if (state.HasWeaponState)
            packet << "|weapon=" << state.WeaponClassHex << "|weaponid=" << state.WeaponInstanceId
                << "|ammo=" << state.Ammo << "|maxammo=" << state.MaxAmmo;
        if (state.HasWeaponState && state.HasStatusState)
            packet << "|hp=" << state.HP << "|maxhp=" << state.MaxHP
                << "|damage=" << state.DefaultDamage << "|defense=" << state.Defense;
        return packet.str();
    }
}
