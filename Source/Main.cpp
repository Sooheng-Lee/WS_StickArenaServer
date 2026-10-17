#include "pch.h"

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
    if (argc > 2)
    {
        std::cerr << "Usage: VS_StickArenaServer.exe [port]\n";
        return 1;
    }
    if (argc == 2)
    {
        const std::string_view argument(argv[1]);
        const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), port);
        if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || port < 1 || port > 65535)
        {
            std::cerr << "Port must be an integer from 1 to 65535.\n";
            return 1;
        }
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
    std::array<char, 1200> buffer{};
    int exitCode = 0;
    while (Running.load())
    {
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
                std::cerr << "Dropped oversized UDP datagram (maximum 1200 bytes).\n";
                continue;
            }
            std::cerr << "recvfrom failed: " << error << '\n';
            exitCode = 1;
            break;
        }

        const std::string_view packet(buffer.data(), static_cast<size_t>(received));
        bool valid = packet.size() >= 4 && packet.substr(0, 4) == "SA1|";
        for (const unsigned char value : packet)
            valid = valid && value >= 32 && value <= 126;
        if (!valid)
        {
            std::cerr << "Dropped invalid StickArena datagram (" << received << " bytes).\n";
            continue;
        }

        std::array<char, INET_ADDRSTRLEN> address{};
        if (!InetNtopA(AF_INET, &sender.sin_addr, address.data(), address.size()))
        {
            std::cerr << "InetNtopA failed: " << WSAGetLastError() << '\n';
            continue;
        }
        std::cout << '[' << address.data() << ':' << ntohs(sender.sin_port) << "] "
            << received << " bytes | " << packet << '\n';
    }
    SetConsoleCtrlHandler(HandleConsoleSignal, FALSE);
    std::cout << "Server stopped.\n";
    return exitCode;
}
