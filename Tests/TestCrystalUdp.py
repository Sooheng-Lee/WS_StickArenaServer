"""Real UDP regression test: shared tower hits, reward, late join, and server relight."""
import json
import socket
import subprocess
import time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
probe.bind(('127.0.0.1', 0))
port = probe.getsockname()[1]
probe.close()
server = subprocess.Popen([str(root/'bin/x64/Debug/VS_StickArenaServer.exe'), str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
clients = []
try:
    tower = json.loads((root/'Main_LevelObjects.json').read_text())['crystal_towers'][0]
    tower_id = tower['actor_name']
    pos = ','.join(str(tower['position'][axis]) for axis in ('x','y','z'))
    def send(sock, packet):
        sock.send(packet.encode('ascii'))
    def register(index):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(('127.0.0.1', port))
        sock.settimeout(.15)
        clients.append(sock)
        session = format(index, '032x')
        send(sock, 'SA2|type=REGISTER|session='+session+'|name=41|character=41|mesh=|anim=')
        return sock, session
    def fields(packet):
        return dict(part.split('=',1) for part in packet.split('|')[1:])
    def wait(sock, condition, timeout=3):
        end = time.monotonic()+timeout
        while time.monotonic()<end:
            try: packet = sock.recv(8192).decode('ascii')
            except socket.timeout: continue
            data = fields(packet)
            if condition(data): return data
        raise AssertionError('Expected UDP state did not arrive')
    def is_tower(data):
        return data.get('type')=='CRYSTAL_STATE' and data.get('id')==tower_id
    def state(sock, session, seq):
        send(sock, f'SA3|type=PLAYER_STATE|session={session}|seq={seq}|action=STATE|state=IDLE|input=0,0,0|pos={pos}|vel=0,0,0|rot=0,0,0|attack=0')
    time.sleep(.3)
    a, sa = register(1)
    initial = wait(a, is_tower)
    assert initial['lit']=='1' and initial['hits']=='0'
    b, sb = register(2)
    wait(b,is_tower)
    state(a,sa,1)
    state(b,sb,1)
    time.sleep(.3)
    def hit(sock,session,seq,cycle='0'):
        send(sock, f'SA5|type=CRYSTAL_HIT|epoch={initial["epoch"]}|session={session}|id={tower_id}|seq={seq}|cycle={cycle}')
    for count in range(1,6):
        hitter, session = (b,sb) if count==5 else (a,sa)
        hit(hitter,session,count)
        hit(hitter,session,count)  # Same-sequence retries count only once.
        seen = wait(a, lambda d:is_tower(d) and d.get('hits')==str(count))
        peer = wait(b, lambda d:is_tower(d) and d.get('hits')==str(count))
        assert seen==peer
        if count<5: assert seen['lit']=='1'
        time.sleep(.3)
    assert seen['lit']=='0' and seen['winner']==sb and seen['effect'] in ('0','1','2')
    dark_at = time.monotonic()-.3
    late, sl = register(3)
    snapshot = wait(late, lambda d:is_tower(d) and d.get('lit')=='0')
    assert snapshot['winner']==sb and snapshot['effect']==seen['effect']
    relit = None
    last_heartbeat = 0
    while time.monotonic()-dark_at < 37:
        now = time.monotonic()
        if now-last_heartbeat>2:
            for sock,session in ((a,sa),(b,sb),(late,sl)):
                send(sock, 'SA2|type=REGISTER|session='+session+'|name=41|character=41|mesh=|anim=')
            last_heartbeat=now
        try: data=fields(late.recv(8192).decode('ascii'))
        except socket.timeout: continue
        if is_tower(data) and data['lit']=='1':
            relit=data
            break
    elapsed=time.monotonic()-dark_at
    assert relit and 24.5<=elapsed<=36 and relit['cycle']=='1' and relit['hits']=='0', (elapsed,relit)
    # Late old-cycle request must not increment the newly lit tower.
    state(a,sa,2)
    hit(a,sa,99)
    current=wait(a,lambda d:is_tower(d) and d['cycle']=='1')
    assert current['hits']=='0'
    print(f'PASS: two peers share five hits and reward; late join restores dark state; server relights after {elapsed:.2f}s; stale cycle rejected')
finally:
    for sock in clients: sock.close()
    server.terminate()
    server.wait(timeout=5)
