# StickArena UDP 서버

Windows / Visual Studio 2022, C++17, WinSock2를 사용합니다.

## 실행

1. `VS_StickArenaServer.sln`의 `Debug | x64` 또는 `Release | x64`를 빌드합니다.
2. `bin/x64/Debug/VS_StickArenaServer.exe`를 실행합니다. 기본 포트는 UDP 7777이며 명령행 인수로 변경할 수 있습니다.
3. Unreal Editor를 종료하고 `UE_StickArenaDemoEditor`를 전체 빌드한 후 다시 엽니다. 새 UCLASS/USTRUCT가 있으므로 Live Coding 대신 전체 빌드로 반영합니다.
4. 별도 게임 인스턴스에서 Play합니다. `UDP Client` 컴포넌트의 `Server Address`, `Server Port`를 동일한 서버로 설정합니다. 다른 PC에서는 서버 PC의 IPv4 주소를 사용합니다.
5. 다른 플레이어가 연결되면 `ASAEnemyCharacter`가 자동 생성되어 이동, 회전, 이동 BlendSpace, 공격 몽타주를 재생합니다.
6. 서버는 Ctrl+C로 종료하며 대기 작업 처리와 작업 스레드 join 후 소켓을 닫습니다.

## 스레드와 저장

메인 스레드는 수신과 작업 큐 전달을 담당하고 CPU 수에 따라 2~8개 작업 스레드를 생성합니다. 접속 인원은 스레드 수로 제한하지 않습니다. 같은 IP/포트의 패킷은 동일한 큐로 전달하여 서버 도착 순서를 유지합니다. 큐별 최대 대기 건수는 1024입니다.

`PlayerRegistry`의 `std::vector<PlayerRegistration>`은 뮤텍스로 보호하며 에셋 정보와 `LatestState`를 저장합니다. 등록 GUID와 IP/포트가 일치하는 상태만 받습니다. 중복/역순 시퀀스, 감소한 공격 카운터, 잘못된 필드, NaN/Infinity를 버립니다. 상태 저장과 수신 대상 복사는 잠금 안에서 수행하고 송신은 잠금을 해제한 뒤 수행합니다. 송신 GUID와 송신 IP/포트 모두 브로드캐스트에서 제외합니다.

10초간 등록/상태 패킷이 없으면 서버에서 제거합니다. 클라이언트도 10초간 정보/상태가 없는 원격 캐릭터를 제거합니다.

## 패킷

데이터그램 하나에 ASCII 텍스트 한 건을 보냅니다. 이름/에셋 경로는 UTF-8 바이트를 hex로 인코딩하여 한글과 구분자를 보존합니다. 최대 데이터그램은 8192바이트, 상태 패킷은 2048바이트입니다. NUL은 전송하지 않습니다.

```text
SA2|type=REGISTER|session=<GUID>|name=<hex>|character=<hex>|mesh=<hex>|anim=<hex>
SA2|type=REGISTER_ACK|session=<GUID>
SA2|type=PLAYER_INFO|session=<GUID>|name=<hex>|character=<hex>|mesh=<hex>|anim=<hex>
SA3|type=PLAYER_STATE|session=<GUID>|seq=1|action=STATE|state=MOVING|input=1,0,0|pos=100,200,90|vel=100,0,0|rot=0,90,0|attack=0
```

GUID는 세션마다 생성하는 32자리 식별자입니다. 등록은 ACK 전에는 1초, 이후에는 3초 간격으로 보냅니다. 서버는 등록을 받을 때 다른 플레이어의 에셋 정보와 최신 상태를 보내고 등록한 플레이어의 정보를 다른 클라이언트에 보냅니다. 주기적 재전송으로 정보 유실과 서버 재시작에서 복구합니다.

상태는 등록 ACK 후 기본 0.1초 간격으로 보내며 정지 중에도 보냅니다. MOVE/STOP/ATTACK 입력 시에도 전송합니다. `input`, `pos`, `vel`은 X,Y,Z이며 단위는 Unreal cm, cm/s입니다. `rot`은 Pitch,Yaw,Roll(도)입니다. `state`는 IDLE/MOVING/FALLING/ATTACKING입니다.

`attack`은 증가하는 공격 횟수이며 이후 STATE 패킷에도 유지합니다. ATTACK 패킷이 유실되어도 다음 상태에서 새 공격을 감지하며 카운터가 증가할 때 한 번 몽타주를 요청합니다. 첫 접속 시 기존 공격 이력은 재생하지 않습니다. 모든 중간 프레임/콤보 입력의 전달 보장을 의미하지는 않습니다.

서버는 클라이언트가 보고한 상태를 저장·전달합니다. 이동/피격/체력 등의 서버 게임 판정은 별도입니다. 기존 SA1 패킷은 콘솔 진단 출력만 지원합니다.

## Unreal 수신과 애니메이션

`USAUdpClientComponent`는 `RemotePlayerRegistrations`, `RemotePlayerStates`, `RemoteCharacters`를 GUID별로 관리합니다. 정보/상태의 도착 순서와 관계없이 두 데이터가 준비되면 `ASAEnemyCharacter`를 하나 생성합니다. `EnemyCharacterClass`로 Blueprint 파생 클래스를 지정할 수 있습니다. `GetRemoteCharacter`, `OnRemotePlayerStateReceived`, `OnRemotePlayerRemoved`를 Blueprint에서 사용할 수 있습니다.

원격 캐릭터는 전달받은 SkeletalMesh와 AnimInstance 클래스를 적용하며 원본 Character 클래스 CDO의 메시 상대 변환, 머티리얼, 캡슐 크기를 가져옵니다. 해당 에셋은 각 클라이언트 프로젝트/패키지에 포함되어 있어야 합니다. 로컬 이동 물리 대신 수신 위치·회전을 보간하고 속도 외삽은 최대 0.1초로 제한하며 600cm 이상 차이는 즉시 보정합니다.

기존 `USAPlayerAnimInstance`는 공통 `ASABaseCharacter::GetMoveVector()`를 사용하여 플레이어와 적 모두 같은 `ABP_Player`의 이동 BlendSpace를 구동합니다. 공격은 공통 `Attack()`을 통해 수신 캐릭터의 현재 AnimInstance에서 실행합니다.

## 검증

```powershell
./Tests/Test-PlayerRegistration.ps1
./Tests/Test-Multiplayer.ps1
```

멀티플레이 테스트는 8개 동시 UDP 클라이언트, 상태 업데이트 240건/다른 클라이언트 수신 1680건, 메타데이터/스냅샷 동기화, 송신자 제외, 중복/역순/잘못된 패킷/다른 주소의 ID 사용을 검사합니다.

Unreal 자동화 테스트: `StickArena.Networking.EnemyCharacter`, `StickArena.Networking.UdpSpawnsEnemy`. 실제 프로젝트 에셋을 로드하여 이동·회전·애니메이션과 UDP 수신에 따른 생성/중복 방지/세션 정리를 검사합니다.

## PCH

공통 헤더는 `Source/pch.h`에서 관리하며 새 `.cpp`의 첫 include는 `pch.h`로 지정합니다. `Source/pch.cpp`에서 PCH를 생성하며 프로젝트는 `Ws2_32.lib`를 링크합니다.
