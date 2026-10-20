using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
public static class TreasureUdpTests
{
    static string Session(int n) { return n.ToString("x32"); }
    static void Send(UdpClient c, string p) { var b=Encoding.ASCII.GetBytes(p); c.Send(b,b.Length); }
    static void Heartbeat(UdpClient c, int id) {
        try { Send(c,"SA2|type=REGISTER|session="+Session(id)+"|name=41|character=41|mesh=|anim="); }
        catch (ObjectDisposedException) { }
        catch (SocketException) { }
    }
    static void Register(UdpClient c, int id) {
        Send(c,"SA2|type=REGISTER|session="+Session(id)+"|name=41|character=41|mesh=|anim=");
        Wait(c,"SA2|type=REGISTER_ACK|",null,3);
        Send(c,"SA3|type=PLAYER_STATE|session="+Session(id)+"|seq=1|action=STATE|state=IDLE|input=0,0,0|pos=0,0,88|vel=0,0,0|rot=0,0,0|attack=0");
    }
    static string Wait(UdpClient c, string prefix, string contains, int seconds) {
        var until=DateTime.UtcNow.AddSeconds(seconds); c.Client.ReceiveTimeout=200;
        while(DateTime.UtcNow<until) {
            try { IPEndPoint from=null; string p=Encoding.ASCII.GetString(c.Receive(ref from));
                if(p.StartsWith(prefix) && (contains==null || p.Contains(contains))) return p;
            } catch(SocketException e) { if(e.SocketErrorCode!=SocketError.TimedOut) throw; }
        }
        throw new Exception("Missing packet: "+prefix+" / "+contains);
    }
    static string Field(string packet,string key) {
        foreach(var f in packet.Split('|')) if(f.StartsWith(key+"=")) return f.Substring(key.Length+1);
        throw new Exception("Missing "+key);
    }
    public static void Run(int port) {
        using(var a=new UdpClient(0)) using(var b=new UdpClient(0)) using(var spoof=new UdpClient(0)) {
            a.Connect("127.0.0.1",port); b.Connect("127.0.0.1",port); spoof.Connect("127.0.0.1",port);
            var clock=Stopwatch.StartNew(); Register(a,1); Register(b,2);
            using(var heartbeatA = new Timer(_ => Heartbeat(a,1), null, 2000, 2000))
            using(var heartbeatB = new Timer(_ => Heartbeat(b,2), null, 2000, 2000)) {
            string first=Wait(a,"SA4|type=TREASURE_STATE|","|id=1|",25);
            if(clock.Elapsed.TotalSeconds<19.0) throw new Exception("Spawn occurred before twenty seconds");
            string peer=Wait(b,"SA4|type=TREASURE_STATE|","|id=1|",2);
            if(first!=peer || Field(first,"item")!="0" || Field(first,"hits")!="0") throw new Exception("Peers disagree on spawn");
            string position=Field(first,"pos");
            Send(a,"SA3|type=PLAYER_STATE|session="+Session(1)+"|seq=2|action=STATE|state=IDLE|input=0,0,0|pos="+position+"|vel=0,0,0|rot=0,0,0|attack=0");
            Thread.Sleep(100);
            string hit="SA4|type=BOX_HIT|epoch="+Field(first,"epoch")+"|session="+Session(1)+"|id=1|seq=";
            Send(spoof,hit+"1"); Thread.Sleep(100);
            Send(a,hit+"1"); Send(a,hit+"1");
            Wait(a,"SA4|type=HIT_ACK|","|seq=1",2);
            Wait(b,"SA4|type=TREASURE_STATE|","|hits=1|",2);
            Thread.Sleep(250); Send(a,hit+"2");
            Wait(b,"SA4|type=TREASURE_STATE|","|hits=2|",2);
            for (int count = 3; count <= 4; ++count) {
                Thread.Sleep(250); Send(a,hit+count);
                Wait(b,"SA4|type=TREASURE_STATE|","|hits="+count+"|",2);
            }
            Thread.Sleep(250); Send(a,hit+"5");
            string item=Wait(b,"SA4|type=TREASURE_STATE|","|hits=5|",2);
            if(Field(item,"pos")!=position) throw new Exception("Drop moved");
            using(var late=new UdpClient(0)) {
                late.Connect("127.0.0.1",port); Register(late,3);
                Wait(late,"SA4|type=TREASURE_STATE|","|hits=5|",2);
            }
            string pickup = "SA4|type=PICKUP|epoch=" + Field(first,"epoch") + "|session=" + Session(1) + "|id=1|seq=1";
            Send(spoof, pickup);
            Send(a, pickup); Send(a, pickup); // Lost grant / duplicate request is idempotent.
            string collected = Wait(b,"SA4|type=TREASURE_STATE|","|owner=" + Session(1),2);
            Wait(a,"SA4|type=TREASURE_STATE|","|owner=" + Session(1),2);
            if (Field(collected,"id") != "1") throw new Exception("Wrong drop was collected");
            Send(b, "SA3|type=PLAYER_STATE|session=" + Session(2) + "|seq=2|action=STATE|state=IDLE|input=0,0,0|pos=" + position + "|vel=0,0,0|rot=0,0,0|attack=0");
            Thread.Sleep(50);
            Send(b, pickup.Replace("session=" + Session(1), "session=" + Session(2)));
            if (Field(Wait(b,"SA4|type=TREASURE_STATE|","|owner=",2),"owner") != Session(1))
                throw new Exception("Competing pickup transferred ownership");
            using(var late=new UdpClient(0)) {
                late.Connect("127.0.0.1",port); Register(late,4);
                string restored = Wait(late,"SA4|type=TREASURE_STATE|","|id=1|",2);
                if (Field(restored,"owner") != Session(1)) throw new Exception("Collected drop revived for late joiner");
            }
            Register(a,1); Register(b,2); // Keep clients alive across the second spawn interval.
            Wait(b,"SA4|type=TREASURE_STATE|","|id=2|",25);
            }
        }
    }
}
