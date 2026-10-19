using System;
using System.Net;
using System.Net.Sockets;
using System.Text;

public static class WeaponUdpTests
{
    static string Id(int n) { return n.ToString("x32"); }
    static void Send(UdpClient client, string packet) {
        byte[] bytes = Encoding.ASCII.GetBytes(packet); client.Send(bytes, bytes.Length);
    }
    static string Wait(UdpClient client, Func<string, bool> match) {
        DateTime end = DateTime.UtcNow.AddSeconds(3);
        client.Client.ReceiveTimeout = 150;
        while (DateTime.UtcNow < end) {
            try { IPEndPoint from = null; string p = Encoding.ASCII.GetString(client.Receive(ref from)); if (match(p)) return p; }
            catch (SocketException e) { if (e.SocketErrorCode != SocketError.TimedOut) throw; }
        }
        throw new Exception("Expected weapon snapshot was not received");
    }
    static void Register(UdpClient client, int id) {
        Send(client, "SA2|type=REGISTER|session=" + Id(id) + "|name=41|character=41|mesh=|anim=");
        Wait(client, p => p == "SA2|type=REGISTER_ACK|session=" + Id(id));
    }
    static string State(int seq, string equipment) {
        return "SA3|type=PLAYER_STATE|session=" + Id(1) + "|seq=" + seq
            + "|action=STATE|state=IDLE|input=0,0,0|pos=0,0,100|vel=0,0,0|rot=0,0,0|attack=0" + equipment;
    }
    public static void Run(int port) {
        using (var a = new UdpClient(0)) using (var b = new UdpClient(0)) using (var late = new UdpClient(0)) {
            a.Connect("127.0.0.1", port); b.Connect("127.0.0.1", port); late.Connect("127.0.0.1", port);
            Register(a, 1); Register(b, 2);
            string path = BitConverter.ToString(Encoding.UTF8.GetBytes("/Game/Items/Weapons/Blueprints/BP_WeaponSword.BP_WeaponSword_C")).Replace("-", "");
            string equipped = "|weapon=" + path + "|weaponid=" + Id(100) + "|ammo=3|maxammo=10";
            Send(a, State(1, equipped));
            if (Wait(b, p => p.StartsWith("SA3|") && p.Contains("|seq=1|")) != State(1, equipped))
                throw new Exception("Equipment did not round-trip through server");
            Register(late, 3);
            if (Wait(late, p => p.StartsWith("SA3|") && p.Contains("|session=" + Id(1) + "|")) != State(1, equipped))
                throw new Exception("Late join did not receive stored equipment");
            // Invalid ammo and stale equipment cannot replace the stored snapshot.
            Send(a, State(2, equipped.Replace("|ammo=3", "|ammo=11")));
            Register(b, 2);
            if (Wait(b, p => p.StartsWith("SA3|")) != State(1, equipped)) throw new Exception("Invalid ammo was accepted");
            string unarmed = "|weapon=|weaponid=|ammo=0|maxammo=0";
            Send(a, State(3, unarmed));
            Wait(b, p => p == State(3, unarmed));
            Send(a, State(1, equipped));
            Register(late, 3);
            Wait(late, p => p == State(3, unarmed));
        }
    }
}
