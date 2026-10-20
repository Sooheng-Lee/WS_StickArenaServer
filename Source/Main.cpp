#include "pch.h"
#include "PlayerRegistration.h"
#include "PacketWorkers.h"
#include "TreasureWorld.h"
#include "CrystalWorld.h"
#include "MatchClock.h"

namespace
{
    std::atomic_bool Running{true};

    BOOL WINAPI HandleConsoleSignal(DWORD signal)
    {
        if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT)
        {
            Running.store(false);
            return TRUE;
        }
        return FALSE;
    }

    struct WinsockLifetime
    {
        ~WinsockLifetime() { WSACleanup(); }
    };

    struct SocketLifetime
    {
        SOCKET Handle = INVALID_SOCKET;
        ~SocketLifetime() { if (Handle != INVALID_SOCKET) closesocket(Handle); }
    };
}

int main(int argc, char* argv[])
{
    int port = 7777;
    if (argc > 3)
    {
        std::cerr << "Usage: VS_StickArenaServer.exe [port] [spawn-data.json]\n";
        return 1;
    }
    if (argc >= 2)
    {
        const std::string_view argument(argv[1]);
        const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), port);
        if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || port < 1 || port > 65535)
        {
            std::cerr << "Port must be an integer from 1 to 65535.\n";
            return 1;
        }
    }

    TreasureWorld treasureWorld;
    CrystalWorld crystalWorld;
    try
    {
        std::array<wchar_t, 32768> executable{};
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("Cannot resolve server executable path");
        const std::filesystem::path spawnPath = argc == 3 ? std::filesystem::u8path(argv[2])
            : std::filesystem::path(executable.data()).parent_path() / "Main_LevelObjects.json";
        treasureWorld.LoadSpawnPoints(spawnPath);
        crystalWorld.Load(spawnPath);
        std::cout << "Loaded treasure spawn data: " << spawnPath.u8string()
            << " | points=" << treasureWorld.SpawnPointCount() << " | interval=20s\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Failed to load treasure spawn data: " << error.what() << '\n';
        return 1;
    }

    WSADATA data{};
    const int startupError = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupError != 0)
    {
        std::cerr << "WSAStartup failed: " << startupError << '\n';
        return 1;
    }
    const WinsockLifetime winsock;
    const SocketLifetime socket{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    if (socket.Handle == INVALID_SOCKET)
    {
        std::cerr << "socket failed: " << WSAGetLastError() << '\n';
        return 1;
    }

    // One listener owns the port. Do not silently share it with another process.
    const BOOL exclusive = TRUE;
    const DWORD timeoutMs = 250;
    if (setsockopt(socket.Handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
        setsockopt(socket.Handle, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == SOCKET_ERROR)
    {
        std::cerr << "setsockopt failed: " << WSAGetLastError() << '\n';
        return 1;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(static_cast<u_short>(port));
    if (bind(socket.Handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        std::cerr << "bind failed on port " << port << ": " << WSAGetLastError() << '\n';
        return 1;
    }
    if (!SetConsoleCtrlHandler(HandleConsoleSignal, TRUE))
    {
        std::cerr << "SetConsoleCtrlHandler failed: " << GetLastError() << '\n';
        return 1;
    }

    std::cout << std::unitbuf << "StickArena UDP server listening on 0.0.0.0:" << port
        << "\nWaiting for player actions. Press Ctrl+C to stop.\n";
    PlayerRegistry playerRegistry;
    MatchClock matchClock;

    std::mutex logMutex;
    const auto sendPacket = [&](const std::string& packet, const PlayerEndpoint& recipient)
    {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(recipient.Port);
        if (InetPtonA(AF_INET, recipient.Address.c_str(), &destination.sin_addr) != 1) return;
        if (sendto(socket.Handle, packet.data(), static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            const std::lock_guard<std::mutex> lock(logMutex);
            std::cerr << "sendto failed: " << error << '\n';
        }
    };
    const auto processPacket = [&](const ReceivedPacket& received)
    {
        const std::string_view packet = received.Data;
        bool valid = packet.size() >= 4;
        for (const unsigned char value : packet) valid = valid && value >= 32 && value <= 126;
        if (!valid) return;
        constexpr std::string_view leavePrefix = "SA2|type=LEAVE|session=";
        if (packet.substr(0, leavePrefix.size()) == leavePrefix)
        {
            const auto session = packet.substr(leavePrefix.size());
            if (session.size() != 32) return;
            for (const char digit : session) if (RegistrationProtocol::HexDigit(digit) < 0) return;
            PlayerRegistry::Removal removal;
            if (playerRegistry.Remove(std::string(session), received.Sender, &removal))
            {
                const std::lock_guard<std::mutex> lock(logMutex);
                std::cout << "Player left " << session << " | players=" << removal.RemainingPlayers
                    << " | reason=LEAVE | name=" << removal.Player.PlayerName
                    << " | endpoint=" << removal.Player.Address << ':' << removal.Player.Port << '\n';
            }
            return;
        }
        if (packet.substr(0, 4) == "SA5|")
        {
            if (crystalWorld.Hit(packet, received.Sender, playerRegistry.GetPlayers()))
            {
                const auto idStart = packet.find("|id=");
                const auto cycleStart = packet.find("|cycle=");
                sendPacket("SA5|type=CRYSTAL_ACK|epoch=" + crystalWorld.Epoch
                    + std::string(packet.substr(idStart, cycleStart-idStart)), received.Sender);
                const auto updates = crystalWorld.Packets();
                for (const auto& peer : playerRegistry.GetPlayers())
                    for (const auto& update : updates) sendPacket(update, {peer.Address,peer.Port});
            }
            return;
        }
        if (packet.substr(0, 4) == "SA4|")
        {
            if (packet.substr(0, 16) == "SA4|type=PICKUP|")
            {
                if (treasureWorld.Pickup(packet, received.Sender, playerRegistry.GetPlayers()))
                    for (const auto& peer : playerRegistry.GetPlayers())
                        for (const auto& update : treasureWorld.Packets()) sendPacket(update, {peer.Address, peer.Port});
                return;
            }
            if (treasureWorld.Hit(packet, received.Sender, playerRegistry.GetPlayers()))
            {
                const auto idStart = packet.find("|id=");
                const auto seqStart = packet.find("|seq=");
                sendPacket("SA4|type=HIT_ACK|epoch=" + treasureWorld.Epoch
                    + std::string(packet.substr(idStart, seqStart - idStart)) + std::string(packet.substr(seqStart)), received.Sender);
                const auto packets = treasureWorld.Packets();
                for (const auto& peer : playerRegistry.GetPlayers())
                    for (const auto& update : packets) sendPacket(update, {peer.Address, peer.Port});
            }
            return;
        }
        if (packet.substr(0, 4) == "SA2|")
        {
            PlayerRegistration player;
            if (!RegistrationProtocol::Parse(packet, player))
            {
                const std::lock_guard<std::mutex> lock(logMutex);
                std::cerr << "Dropped invalid player registration.\n";
                return;
            }
            player.Address = received.Sender.Address;
            player.Port = received.Sender.Port;
            const auto result = playerRegistry.Store(player);
            if (result == PlayerRegistry::StoreResult::EndpointMismatch
                || result == PlayerRegistry::StoreResult::SessionClosed) return;
            matchClock.Start();
            sendPacket("SA2|type=REGISTER_ACK|session=" + player.PlayerId, received.Sender);
            sendPacket(matchClock.Packet(treasureWorld.Epoch), received.Sender);
            for (const auto& update : treasureWorld.Packets()) sendPacket(update, received.Sender);
            for (const auto& update : crystalWorld.Packets()) sendPacket(update, received.Sender);
            // Repeat peer metadata during registration heartbeats so lost UDP info recovers.
            // A new peer also gets the existing players' latest snapshots immediately.
            const auto players = playerRegistry.GetPlayers();
            const std::string info = RegistrationProtocol::SerializePlayerInfo(player);
            for (const auto& other : players)
            {
                if (other.PlayerId == player.PlayerId
                    || (other.Address == player.Address && other.Port == player.Port)) continue;
                sendPacket(info, {other.Address, other.Port});
                sendPacket(RegistrationProtocol::SerializePlayerInfo(other), received.Sender);
                if (other.HasState) sendPacket(StateProtocol::Serialize(other.LatestState), received.Sender);
            }
            if (result == PlayerRegistry::StoreResult::Inserted)
            {
                const auto count = players.size();
                const std::lock_guard<std::mutex> lock(logMutex);
                std::cout << "Registered player " << player.PlayerId << " | name=" << player.PlayerName
                    << " | character=" << player.CharacterClassPath << " | mesh=" << player.SkeletalMeshPath
                    << " | anim=" << player.AnimInstanceClassPath << " | players=" << count << '\n';
            }
            return;
        }
        if (packet.substr(0, 4) == "SA3|")
        {
            PlayerState state;
            std::vector<PlayerEndpoint> recipients;
            if (!StateProtocol::Parse(packet, state)) return;
            if (matchClock.RemainingMs() == 0)
            {
                // Keep the last accepted position even if an old client keeps moving.
                for (const auto& player : playerRegistry.GetPlayers())
                    if (player.PlayerId == state.PlayerId && player.HasState)
                        state.Position = player.LatestState.Position;
                state.MovementInput = {};
                state.Velocity = {};
                state.State = "IDLE";
                if (state.Action == "MOVE") state.Action = "STOP";
            }
            if (!playerRegistry.UpdateState(state, received.Sender, recipients)) return;
            // UpdateState commits the struct before taking a recipient snapshot.
            // No registry lock is held during socket sends.
            const std::string broadcast = StateProtocol::Serialize(state);
            for (const auto& recipient : recipients) sendPacket(broadcast, recipient);
            return;
        }
        // Legacy diagnostic packets remain log-only; new clients send SA3 state structs.
        if (packet.substr(0, 4) == "SA1|")
        {
            const std::lock_guard<std::mutex> lock(logMutex);
            std::cout << '[' << received.Sender.Address << ':' << received.Sender.Port << "] " << packet << '\n';
        }
    };
    const size_t workerCount = (std::max)(size_t{2}, (std::min)(size_t{8}, static_cast<size_t>(std::thread::hardware_concurrency())));
    PacketWorkers workers(workerCount, processPacket);
    std::cout << "Packet workers: " << workerCount << '\n';
    std::array<char, 8192> buffer{};
    int exitCode = 0;
    auto lastCleanup = std::chrono::steady_clock::now();
    auto lastTick = lastCleanup;
    auto lastMatchBroadcast = lastCleanup;
    while (Running.load())
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastMatchBroadcast >= std::chrono::milliseconds(250))
        {
            const auto update = matchClock.Packet(treasureWorld.Epoch);
            for (const auto& peer : playerRegistry.GetPlayers())
                sendPacket(update, {peer.Address, peer.Port});
            lastMatchBroadcast = now;
        }
        if (treasureWorld.Tick(std::chrono::duration<double>(now - lastTick).count(), playerRegistry.GetPlayers()))
        {
            const auto updates = treasureWorld.Packets();
            for (const auto& peer : playerRegistry.GetPlayers())
                for (const auto& update : updates) sendPacket(update, {peer.Address, peer.Port});
        }
        if (crystalWorld.Tick(std::chrono::duration<double>(now-lastTick).count()))
        {
            const auto updates = crystalWorld.Packets();
            for (const auto& peer : playerRegistry.GetPlayers())
                for (const auto& update : updates) sendPacket(update, {peer.Address,peer.Port});
        }
        lastTick = now;
        if (now - lastCleanup >= std::chrono::seconds(1))
        {
            const auto removed = playerRegistry.RemoveInactive(now);
            for (const auto& removal : removed)
            {
                const std::lock_guard<std::mutex> lock(logMutex);
                std::cout << "Player left " << removal.Player.PlayerId << " | players=" << removal.RemainingPlayers
                    << " | reason=TIMEOUT | name=" << removal.Player.PlayerName
                    << " | endpoint=" << removal.Player.Address << ':' << removal.Player.Port << '\n';
            }
            const auto updates = treasureWorld.Packets();
            for (const auto& peer : playerRegistry.GetPlayers())
                for (const auto& update : updates) sendPacket(update, {peer.Address, peer.Port});
            for (const auto& peer : playerRegistry.GetPlayers())
                for (const auto& update : crystalWorld.Packets()) sendPacket(update, {peer.Address,peer.Port});
            lastCleanup = now;
        }
        sockaddr_in sender{};
        int senderLength = sizeof(sender);
        const int received = recvfrom(socket.Handle, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sender), &senderLength);
        if (received == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK || error == WSAECONNRESET) continue;
            if (error == WSAEMSGSIZE)
            {
                std::cerr << "Dropped oversized UDP datagram (maximum 8192 bytes).\n";
                continue;
            }
            std::cerr << "recvfrom failed: " << error << '\n';
            exitCode = 1;
            break;
        }

        std::array<char, INET_ADDRSTRLEN> address{};
        if (!InetNtopA(AF_INET, &sender.sin_addr, address.data(), address.size()))
        {
            std::cerr << "InetNtopA failed: " << WSAGetLastError() << '\n';
            continue;
        }
        if (!workers.Enqueue({std::string(buffer.data(), static_cast<size_t>(received)),
            {address.data(), ntohs(sender.sin_port)}}))
        {
            const std::lock_guard<std::mutex> lock(logMutex);
            std::cerr << "Dropped datagram: worker queue full.\n";
        }
    }
    workers.Stop();
    SetConsoleCtrlHandler(HandleConsoleSignal, FALSE);
    std::cout << "Server stopped.\n";
    return exitCode;
}
