#include "pch.h"
#include "Server.h"
#include "PacketWorkers.h"
#include "TreasureWorld.h"
#include "CrystalWorld.h"
#include "MatchClock.h"

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto MatchBroadcastInterval = std::chrono::milliseconds(250);
    constexpr auto CleanupInterval = std::chrono::seconds(1);
    constexpr size_t ReceiveBufferSize = 8192;
    constexpr std::string_view LeavePrefix = "SA2|type=LEAVE|session=";

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
        bool Started = false;
        ~WinsockLifetime() { if (Started) WSACleanup(); }
    };

    struct SocketLifetime
    {
        SOCKET Handle = INVALID_SOCKET;
        ~SocketLifetime() { if (Handle != INVALID_SOCKET) closesocket(Handle); }
    };

    struct ConsoleHandlerLifetime
    {
        bool Registered = false;
        ~ConsoleHandlerLifetime() { if (Registered) SetConsoleCtrlHandler(HandleConsoleSignal, FALSE); }
    };

    struct ServerOptions
    {
        int Port = 7777;
        const char* SpawnDataPath = nullptr;
    };

    bool ParseOptions(int argc, char* argv[], ServerOptions& options)
    {
        if (argc > 3)
        {
            std::cerr << "Usage: VS_StickArenaServer.exe [port] [spawn-data.json]\n";
            return false;
        }
        if (argc >= 2)
        {
            const std::string_view argument(argv[1]);
            const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), options.Port);
            if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size()
                || options.Port < 1 || options.Port > 65535)
            {
                std::cerr << "Port must be an integer from 1 to 65535.\n";
                return false;
            }
        }
        if (argc == 3) options.SpawnDataPath = argv[2];
        return true;
    }

    std::filesystem::path ResolveSpawnDataPath(const ServerOptions& options)
    {
        std::array<wchar_t, 32768> executable{};
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("Cannot resolve server executable path");
        return options.SpawnDataPath ? std::filesystem::u8path(options.SpawnDataPath)
            : std::filesystem::path(executable.data()).parent_path() / "Main_LevelObjects.json";
    }

    bool IsValidPacket(std::string_view packet)
    {
        return packet.size() >= 4 && std::all_of(packet.begin(), packet.end(),
            [](unsigned char value) { return value >= 32 && value <= 126; });
    }

    size_t GetWorkerCount()
    {
        return (std::max)(size_t{2}, (std::min)(size_t{8}, static_cast<size_t>(std::thread::hardware_concurrency())));
    }

    class UdpServer
    {
    public:
        int Run(const ServerOptions& options);

    private:
        bool LoadWorldData(const ServerOptions& options);
        bool InitializeNetwork(int port);
        bool ConfigureSocket();
        bool BindSocket(int port);
        int RunLoop(PacketWorkers& workers);
        bool ReceivePacket(PacketWorkers& workers);

        void ProcessPacket(const ReceivedPacket& received);
        void HandleLeave(const ReceivedPacket& received);
        void HandleRegistration(const ReceivedPacket& received);
        void HandlePlayerState(const ReceivedPacket& received);
        void HandleTreasure(const ReceivedPacket& received);
        void HandleCrystal(const ReceivedPacket& received);
        void LogDiagnostic(const ReceivedPacket& received);

        void SendPacket(const std::string& packet, const PlayerEndpoint& recipient);
        void SendPackets(const std::vector<std::string>& packets, const PlayerEndpoint& recipient);
        void BroadcastPacket(const std::string& packet);
        void BroadcastPackets(const std::vector<std::string>& packets);
        void SendInitialState(const PlayerEndpoint& recipient);
        void SyncPeerInformation(const PlayerRegistration& player, const std::vector<PlayerRegistration>& players);
        void ApplyMatchEndRestrictions(PlayerState& state);
        void LogPlayerRemoval(const PlayerRegistry::Removal& removal, std::string_view reason);

        void UpdateWorlds(double elapsedSeconds);
        void BroadcastMatchState();
        void CleanupInactivePlayers(Clock::time_point now);
        void BroadcastWorldState();

        // Destruction closes the socket before shutting down Winsock.
        WinsockLifetime Winsock;
        SocketLifetime Socket;
        ConsoleHandlerLifetime ConsoleHandler;
        TreasureWorld Treasures;
        CrystalWorld Crystals;
        PlayerRegistry Players;
        MatchClock Match;
        std::mutex LogMutex;
    };

    int UdpServer::Run(const ServerOptions& options)
    {
        if (!LoadWorldData(options) || !InitializeNetwork(options.Port)) return 1;
        Running.store(true);
        if (!SetConsoleCtrlHandler(HandleConsoleSignal, TRUE))
        {
            std::cerr << "SetConsoleCtrlHandler failed: " << GetLastError() << '\n';
            return 1;
        }
        ConsoleHandler.Registered = true;

        std::cout << std::unitbuf << "StickArena UDP server listening on 0.0.0.0:" << options.Port
            << "\nWaiting for player actions. Press Ctrl+C to stop.\n";
        const size_t workerCount = GetWorkerCount();
        PacketWorkers workers(workerCount, [this](const ReceivedPacket& received) { ProcessPacket(received); });
        std::cout << "Packet workers: " << workerCount << '\n';

        const int exitCode = RunLoop(workers);
        // Drain queued packets while the socket and all shared state are still alive.
        workers.Stop();
        std::cout << "Server stopped.\n";
        return exitCode;
    }

    bool UdpServer::LoadWorldData(const ServerOptions& options)
    {
        try
        {
            const auto spawnPath = ResolveSpawnDataPath(options);
            Treasures.LoadSpawnPoints(spawnPath);
            Crystals.Load(spawnPath);
            std::cout << "Loaded treasure spawn data: " << spawnPath.u8string()
                << " | points=" << Treasures.SpawnPointCount() << " | interval=20s\n";
            return true;
        }
        catch (const std::exception& error)
        {
            std::cerr << "Failed to load treasure spawn data: " << error.what() << '\n';
            return false;
        }
    }

    bool UdpServer::InitializeNetwork(int port)
    {
        WSADATA data{};
        const int startupError = WSAStartup(MAKEWORD(2, 2), &data);
        if (startupError != 0)
        {
            std::cerr << "WSAStartup failed: " << startupError << '\n';
            return false;
        }
        Winsock.Started = true;
        Socket.Handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (Socket.Handle == INVALID_SOCKET)
        {
            std::cerr << "socket failed: " << WSAGetLastError() << '\n';
            return false;
        }
        return ConfigureSocket() && BindSocket(port);
    }

    bool UdpServer::ConfigureSocket()
    {
        // One listener owns the port. Do not silently share it with another process.
        const BOOL exclusive = TRUE;
        const DWORD timeoutMs = 250;
        if (setsockopt(Socket.Handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
            setsockopt(Socket.Handle, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == SOCKET_ERROR)
        {
            std::cerr << "setsockopt failed: " << WSAGetLastError() << '\n';
            return false;
        }
        return true;
    }

    bool UdpServer::BindSocket(int port)
    {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(static_cast<u_short>(port));
        if (bind(Socket.Handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            std::cerr << "bind failed on port " << port << ": " << WSAGetLastError() << '\n';
            return false;
        }
        return true;
    }

    void UdpServer::SendPacket(const std::string& packet, const PlayerEndpoint& recipient)
    {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(recipient.Port);
        if (InetPtonA(AF_INET, recipient.Address.c_str(), &destination.sin_addr) != 1) return;
        if (sendto(Socket.Handle, packet.data(), static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            const std::lock_guard<std::mutex> lock(LogMutex);
            std::cerr << "sendto failed: " << error << '\n';
        }
    }

    void UdpServer::SendPackets(const std::vector<std::string>& packets, const PlayerEndpoint& recipient)
    {
        for (const auto& packet : packets) SendPacket(packet, recipient);
    }

    void UdpServer::BroadcastPacket(const std::string& packet)
    {
        for (const auto& peer : Players.GetPlayers()) SendPacket(packet, {peer.Address, peer.Port});
    }

    void UdpServer::BroadcastPackets(const std::vector<std::string>& packets)
    {
        for (const auto& peer : Players.GetPlayers()) SendPackets(packets, {peer.Address, peer.Port});
    }

    void UdpServer::ProcessPacket(const ReceivedPacket& received)
    {
        const std::string_view packet = received.Data;
        if (!IsValidPacket(packet)) return;
        if (packet.substr(0, LeavePrefix.size()) == LeavePrefix) HandleLeave(received);
        else if (packet.substr(0, 4) == "SA5|") HandleCrystal(received);
        else if (packet.substr(0, 4) == "SA4|") HandleTreasure(received);
        else if (packet.substr(0, 4) == "SA2|") HandleRegistration(received);
        else if (packet.substr(0, 4) == "SA3|") HandlePlayerState(received);
        else if (packet.substr(0, 4) == "SA1|") LogDiagnostic(received);
    }

    void UdpServer::HandleLeave(const ReceivedPacket& received)
    {
        const auto session = std::string_view(received.Data).substr(LeavePrefix.size());
        if (session.size() != 32) return;
        for (const char digit : session) if (RegistrationProtocol::HexDigit(digit) < 0) return;
        PlayerRegistry::Removal removal;
        if (Players.Remove(std::string(session), received.Sender, &removal)) LogPlayerRemoval(removal, "LEAVE");
    }

    void UdpServer::HandleRegistration(const ReceivedPacket& received)
    {
        PlayerRegistration player;
        if (!RegistrationProtocol::Parse(received.Data, player))
        {
            const std::lock_guard<std::mutex> lock(LogMutex);
            std::cerr << "Dropped invalid player registration.\n";
            return;
        }
        player.Address = received.Sender.Address;
        player.Port = received.Sender.Port;
        const auto result = Players.Store(player);
        if (result == PlayerRegistry::StoreResult::EndpointMismatch
            || result == PlayerRegistry::StoreResult::SessionClosed) return;
        Match.Start();
        SendPacket("SA2|type=REGISTER_ACK|session=" + player.PlayerId, received.Sender);
        SendInitialState(received.Sender);

        const auto players = Players.GetPlayers();
        SyncPeerInformation(player, players);
        if (result == PlayerRegistry::StoreResult::Inserted)
        {
            const std::lock_guard<std::mutex> lock(LogMutex);
            std::cout << "Registered player " << player.PlayerId << " | name=" << player.PlayerName
                << " | character=" << player.CharacterClassPath << " | mesh=" << player.SkeletalMeshPath
                << " | anim=" << player.AnimInstanceClassPath << " | players=" << players.size() << '\n';
        }
    }

    void UdpServer::SendInitialState(const PlayerEndpoint& recipient)
    {
        SendPacket(Match.Packet(Treasures.Epoch), recipient);
        SendPackets(Treasures.Packets(), recipient);
        SendPackets(Crystals.Packets(), recipient);
    }

    void UdpServer::SyncPeerInformation(const PlayerRegistration& player, const std::vector<PlayerRegistration>& players)
    {
        // Repeat metadata on heartbeats to recover lost UDP info and send latest peer snapshots to new players.
        const std::string info = RegistrationProtocol::SerializePlayerInfo(player);
        const PlayerEndpoint recipient{player.Address, player.Port};
        for (const auto& other : players)
        {
            if (other.PlayerId == player.PlayerId
                || (other.Address == player.Address && other.Port == player.Port)) continue;
            SendPacket(info, {other.Address, other.Port});
            SendPacket(RegistrationProtocol::SerializePlayerInfo(other), recipient);
            if (other.HasState) SendPacket(StateProtocol::Serialize(other.LatestState), recipient);
        }
    }

    void UdpServer::HandlePlayerState(const ReceivedPacket& received)
    {
        PlayerState state;
        std::vector<PlayerEndpoint> recipients;
        if (!StateProtocol::Parse(received.Data, state)) return;
        ApplyMatchEndRestrictions(state);
        if (!Players.UpdateState(state, received.Sender, recipients)) return;
        // UpdateState commits before taking a recipient snapshot; no registry lock is held during sends.
        const std::string broadcast = StateProtocol::Serialize(state);
        for (const auto& recipient : recipients) SendPacket(broadcast, recipient);
    }

    void UdpServer::ApplyMatchEndRestrictions(PlayerState& state)
    {
        if (Match.RemainingMs() != 0) return;
        // Keep the last accepted position even if an old client keeps moving.
        for (const auto& player : Players.GetPlayers())
            if (player.PlayerId == state.PlayerId && player.HasState) state.Position = player.LatestState.Position;
        state.MovementInput = {};
        state.Velocity = {};
        state.State = "IDLE";
        if (state.Action == "MOVE") state.Action = "STOP";
    }

    void UdpServer::HandleTreasure(const ReceivedPacket& received)
    {
        const std::string_view packet = received.Data;
        if (packet.substr(0, 16) == "SA4|type=PICKUP|")
        {
            if (Treasures.Pickup(packet, received.Sender, Players.GetPlayers())) BroadcastPackets(Treasures.Packets());
            return;
        }
        if (!Treasures.Hit(packet, received.Sender, Players.GetPlayers())) return;
        const auto idStart = packet.find("|id=");
        const auto seqStart = packet.find("|seq=");
        SendPacket("SA4|type=HIT_ACK|epoch=" + Treasures.Epoch
            + std::string(packet.substr(idStart, seqStart - idStart)) + std::string(packet.substr(seqStart)), received.Sender);
        BroadcastPackets(Treasures.Packets());
    }

    void UdpServer::HandleCrystal(const ReceivedPacket& received)
    {
        const std::string_view packet = received.Data;
        if (!Crystals.Hit(packet, received.Sender, Players.GetPlayers())) return;
        const auto idStart = packet.find("|id=");
        const auto cycleStart = packet.find("|cycle=");
        SendPacket("SA5|type=CRYSTAL_ACK|epoch=" + Crystals.Epoch
            + std::string(packet.substr(idStart, cycleStart - idStart)), received.Sender);
        BroadcastPackets(Crystals.Packets());
    }

    void UdpServer::LogDiagnostic(const ReceivedPacket& received)
    {
        // Legacy diagnostic packets remain log-only; new clients send SA3 state structs.
        const std::lock_guard<std::mutex> lock(LogMutex);
        std::cout << '[' << received.Sender.Address << ':' << received.Sender.Port << "] " << received.Data << '\n';
    }

    void UdpServer::LogPlayerRemoval(const PlayerRegistry::Removal& removal, std::string_view reason)
    {
        const std::lock_guard<std::mutex> lock(LogMutex);
        std::cout << "Player left " << removal.Player.PlayerId << " | players=" << removal.RemainingPlayers
            << " | reason=" << reason << " | name=" << removal.Player.PlayerName
            << " | endpoint=" << removal.Player.Address << ':' << removal.Player.Port << '\n';
    }

    void UdpServer::UpdateWorlds(double elapsedSeconds)
    {
        if (Treasures.Tick(elapsedSeconds, Players.GetPlayers())) BroadcastPackets(Treasures.Packets());
        if (Crystals.Tick(elapsedSeconds)) BroadcastPackets(Crystals.Packets());
    }

    void UdpServer::BroadcastMatchState()
    {
        BroadcastPacket(Match.Packet(Treasures.Epoch));
    }

    void UdpServer::CleanupInactivePlayers(Clock::time_point now)
    {
        for (const auto& removal : Players.RemoveInactive(now)) LogPlayerRemoval(removal, "TIMEOUT");
    }

    void UdpServer::BroadcastWorldState()
    {
        BroadcastPackets(Treasures.Packets());
        BroadcastPackets(Crystals.Packets());
    }

    int UdpServer::RunLoop(PacketWorkers& workers)
    {
        auto lastCleanup = Clock::now();
        auto lastTick = lastCleanup;
        auto lastMatchBroadcast = lastCleanup;
        while (Running.load())
        {
            const auto now = Clock::now();
            if (now - lastMatchBroadcast >= MatchBroadcastInterval)
            {
                BroadcastMatchState();
                lastMatchBroadcast = now;
            }
            UpdateWorlds(std::chrono::duration<double>(now - lastTick).count());
            lastTick = now;
            if (now - lastCleanup >= CleanupInterval)
            {
                CleanupInactivePlayers(now);
                BroadcastWorldState();
                lastCleanup = now;
            }
            if (!ReceivePacket(workers)) return 1;
        }
        return 0;
    }

    bool UdpServer::ReceivePacket(PacketWorkers& workers)
    {
        std::array<char, ReceiveBufferSize> buffer{};
        sockaddr_in sender{};
        int senderLength = sizeof(sender);
        const int received = recvfrom(Socket.Handle, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sender), &senderLength);
        if (received == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK || error == WSAECONNRESET) return true;
            if (error == WSAEMSGSIZE)
            {
                std::cerr << "Dropped oversized UDP datagram (maximum 8192 bytes).\n";
                return true;
            }
            std::cerr << "recvfrom failed: " << error << '\n';
            return false;
        }

        std::array<char, INET_ADDRSTRLEN> address{};
        if (!InetNtopA(AF_INET, &sender.sin_addr, address.data(), address.size()))
        {
            std::cerr << "InetNtopA failed: " << WSAGetLastError() << '\n';
            return true;
        }
        if (!workers.Enqueue({std::string(buffer.data(), static_cast<size_t>(received)),
            {address.data(), ntohs(sender.sin_port)}}))
        {
            const std::lock_guard<std::mutex> lock(LogMutex);
            std::cerr << "Dropped datagram: worker queue full.\n";
        }
        return true;
    }
}

int RunServer(int argc, char* argv[])
{
    ServerOptions options;
    if (!ParseOptions(argc, argv, options)) return 1;
    UdpServer server;
    return server.Run(options);
}
