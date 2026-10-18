using System;
using System.Collections.Generic;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

public static class MultiplayerTests
{
    private static int MetadataCount;
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    private static string Hex(string value)
    {
        return BitConverter.ToString(Encoding.UTF8.GetBytes(value)).Replace("-", "");
    }

    private static string Id(int index) { return (index + 1).ToString("x32", CultureInfo.InvariantCulture); }

    private static string Registration(int index)
    {
        return "SA2|type=REGISTER|session=" + Id(index) + "|name=" + Hex("Player" + index)
            + "|character=" + Hex("/Game/BP_Player.BP_Player_C") + "|mesh=|anim=";
    }

    private static string State(int index, int sequence, int attack = 0)
    {
        return "SA3|type=PLAYER_STATE|session=" + Id(index) + "|seq=" + sequence
            + "|action=STATE|state=MOVING|input=1,0,0|pos=" + sequence
            + ",20,30|vel=100,0,0|rot=10,90,5|attack=" + attack;
    }

    private static void Send(UdpClient client, string packet)
    {
        byte[] bytes = Encoding.ASCII.GetBytes(packet);
        client.Send(bytes, bytes.Length);
    }

    private static string Receive(UdpClient client)
    {
        IPEndPoint remote = new IPEndPoint(IPAddress.Any, 0);
        for (;;)
        {
            string packet = Encoding.ASCII.GetString(client.Receive(ref remote));
            if (!packet.StartsWith("SA2|type=PLAYER_INFO|", StringComparison.Ordinal)) return packet;
            Require(packet.Contains("|character=" + Hex("/Game/BP_Player.BP_Player_C") + "|mesh=|anim="), "Invalid peer metadata");
            Interlocked.Increment(ref MetadataCount);
        }
    }

    private static void ExpectSilence(UdpClient client, string message)
    {
        client.Client.ReceiveTimeout = 200;
        try { string unexpected = Receive(client); throw new Exception(message + ": " + unexpected); }
        catch (SocketException error)
        {
            if (error.SocketErrorCode != SocketError.TimedOut) throw;
        }
        finally { client.Client.ReceiveTimeout = 3000; }
    }

    private static string ReceiveState(UdpClient client, int source, int sequence)
    {
        string packet = Receive(client);
        Require(packet.Contains("session=" + Id(source) + "|seq=" + sequence + "|"), "Wrong sender/sequence: " + packet);
        Require(packet.Contains("|state=MOVING|input=1,0,0|pos=" + sequence + ",20,30|vel=100,0,0|rot=10,90,5"),
            "Stored state did not round-trip: " + packet);
        return packet;
    }

    public static void Run(int port)
    {
        const int count = 8;
        UdpClient[] clients = new UdpClient[count];
        UdpClient unknown = new UdpClient(0);
        try
        {
            unknown.Connect("127.0.0.1", port);
            unknown.Client.ReceiveTimeout = 3000;
            Task[] registrations = new Task[count];
            for (int index = 0; index < count; ++index)
            {
                int current = index;
                clients[index] = new UdpClient(0);
                clients[index].Client.ReceiveTimeout = 3000;
                clients[index].Client.ReceiveBufferSize = 1024 * 1024;
                clients[index].Connect("127.0.0.1", port);
                registrations[index] = Task.Run(() =>
                {
                    Send(clients[current], Registration(current));
                    Require(Receive(clients[current]) == "SA2|type=REGISTER_ACK|session=" + Id(current), "Registration failed");
                });
            }
            Task.WaitAll(registrations);
            Send(clients[0], State(0, 10));
            for (int index = 1; index < count; ++index) ReceiveState(clients[index], 0, 10);
            ExpectSilence(clients[0], "Sender received its own broadcast");

            // Bad, stale, duplicate and spoofed states must not alter or broadcast stored data.
            Send(clients[0], State(0, 9));
            Send(clients[0], State(0, 10));
            Send(clients[0], State(0, 11).Replace("|pos=11,", "|pos=nan,"));
            Send(clients[0], State(0, 11) + "|extra=1");
            Send(clients[0], State(0, 11).Replace("|state=MOVING", "|state=INVALID"));
            Send(clients[0], State(0, 11).Replace("|seq=11", "|seq=-1"));
            Send(unknown, State(0, 999));
            // A different endpoint must not take over an existing player ID.
            Send(unknown, Registration(0));
            ExpectSilence(unknown, "Unregistered endpoint was acknowledged");
            ExpectSilence(clients[1], "Rejected state was broadcast");
            Send(clients[0], State(0, 11));
            for (int index = 1; index < count; ++index) ReceiveState(clients[index], 0, 11);
            ExpectSilence(clients[0], "Sender echo after invalid packets");

            // A newly registering peer receives metadata and the stored snapshot of others.
            Send(clients[1], Registration(1));
            Require(Receive(clients[1]) == "SA2|type=REGISTER_ACK|session=" + Id(1), "Peer resync ACK missing");
            ReceiveState(clients[1], 0, 11);

            // Metadata retries must preserve the most recent state/sequence.
            Send(clients[0], Registration(0));
            Require(Receive(clients[0]) == "SA2|type=REGISTER_ACK|session=" + Id(0), "Retry ACK missing");
            Send(clients[0], State(0, 10));
            ExpectSilence(clients[1], "Registration retry reset sequence state");

            Send(clients[0], State(0, 12, 1));
            for (int index = 1; index < count; ++index)
                Require(ReceiveState(clients[index], 0, 12).EndsWith("|attack=1", StringComparison.Ordinal), "Attack counter lost");
            Send(clients[0], State(0, 13, 0));
            ExpectSilence(clients[1], "Decreased attack counter was accepted");
            Send(clients[0], State(0, 13, 1));
            for (int index = 1; index < count; ++index) ReceiveState(clients[index], 0, 13);

            // Concurrent senders, each with their own ordered UDP stream.
            const int updates = 30;
            Task[] readers = new Task[count];
            for (int index = 0; index < count; ++index)
            {
                int receiver = index;
                readers[index] = Task.Factory.StartNew(() =>
                {
                    Dictionary<string, int> last = new Dictionary<string, int>();
                    int received = 0;
                    while (received < (count - 1) * updates)
                    {
                        string packet = Receive(clients[receiver]);
                        string[] fields = packet.Split('|');
                        Require(fields.Length == 11 && fields[0] == "SA3", "Invalid state broadcast");
                        string sender = fields[2].Substring("session=".Length);
                        int sequence = int.Parse(fields[3].Substring("seq=".Length), CultureInfo.InvariantCulture);
                        Require(sender != Id(receiver), "Concurrent sender echo");
                        int previous;
                        if (last.TryGetValue(sender, out previous)) Require(sequence > previous, "Out-of-order state broadcast");
                        last[sender] = sequence;
                        ++received;
                    }
                    Require(last.Count == count - 1, "Missing sender");
                    foreach (int sequence in last.Values) Require(sequence == 100 + updates - 1, "Missing latest state");
                }, TaskCreationOptions.LongRunning);
            }
            Task[] writers = new Task[count];
            for (int index = 0; index < count; ++index)
            {
                int sender = index;
                writers[index] = Task.Factory.StartNew(() =>
                {
                    for (int sequence = 100; sequence < 100 + updates; ++sequence)
                    {
                        Send(clients[sender], State(sender, sequence, sender == 0 ? 1 : 0));
                        Thread.Sleep(2);
                    }
                }, TaskCreationOptions.LongRunning);
            }
            Task.WaitAll(writers);
            Task.WaitAll(readers);
            for (int index = 0; index < count; ++index) ExpectSilence(clients[index], "Unexpected extra broadcast");
            Require(MetadataCount > 0, "Peer metadata was never delivered");
        }
        finally
        {
            unknown.Dispose();
            foreach (UdpClient client in clients) if (client != null) client.Dispose();
        }
    }
}
