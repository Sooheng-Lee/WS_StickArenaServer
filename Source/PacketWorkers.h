#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "PlayerRegistration.h"

struct ReceivedPacket
{
    std::string Data;
    PlayerEndpoint Sender;
};

// Endpoint sharding preserves arrival order for a player's REGISTER and STATE packets.
// Queues are bounded; UDP senders will refresh state on the next heartbeat if overloaded.
class PacketWorkers
{
public:
    using Handler = std::function<void(const ReceivedPacket&)>;

    PacketWorkers(size_t count, Handler handler) : Process(std::move(handler))
    {
        try
        {
            for (size_t index = 0; index < count; ++index)
            {
                Queues.push_back(std::make_unique<Queue>());
                Queue* queue = Queues.back().get();
                Threads.emplace_back([this, queue] { Run(*queue); });
            }
        }
        catch (...)
        {
            Stop();
            throw;
        }
    }

    ~PacketWorkers() { Stop(); }
    PacketWorkers(const PacketWorkers&) = delete;
    PacketWorkers& operator=(const PacketWorkers&) = delete;

    bool Enqueue(ReceivedPacket packet)
    {
        const auto hash = std::hash<std::string>{}(packet.Sender.Address) ^ packet.Sender.Port;
        Queue& queue = *Queues[hash % Queues.size()];
        {
            const std::lock_guard<std::mutex> lock(queue.Mutex);
            if (queue.Stopping || queue.Packets.size() >= 1024) return false;
            queue.Packets.push_back(std::move(packet));
        }
        queue.Ready.notify_one();
        return true;
    }

    void Stop()
    {
        for (auto& queue : Queues)
        {
            {
                const std::lock_guard<std::mutex> lock(queue->Mutex);
                queue->Stopping = true;
            }
            queue->Ready.notify_all();
        }
        for (auto& thread : Threads) if (thread.joinable()) thread.join();
    }

private:
    struct Queue
    {
        std::mutex Mutex;
        std::condition_variable Ready;
        std::deque<ReceivedPacket> Packets;
        bool Stopping = false;
    };

    void Run(Queue& queue)
    {
        for (;;)
        {
            ReceivedPacket packet;
            {
                std::unique_lock<std::mutex> lock(queue.Mutex);
                queue.Ready.wait(lock, [&queue] { return queue.Stopping || !queue.Packets.empty(); });
                if (queue.Packets.empty()) return;
                packet = std::move(queue.Packets.front());
                queue.Packets.pop_front();
            }
            Process(packet);
        }
    }

    Handler Process;
    std::vector<std::unique_ptr<Queue>> Queues;
    std::vector<std::thread> Threads;
};
