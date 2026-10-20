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

## 경기 제한시간 (SA6)

첫 플레이어 등록 시 서버의 `MatchClock`이 180초 공통 경기를 시작합니다. 중간 접속과 재등록은 시간을 초기화하지 않으며, 새 경기는 서버 재시작으로 시작합니다. 서버는 단조 시계를 사용하고 등록 응답 시 및 250ms마다 등록된 클라이언트에 남은 시간을 전송합니다.

```text
SA6|type=MATCH_STATE|epoch=<server-start>|seq=1|remaining_ms=180000
```

UE `USAUdpClientComponent`는 서버 시간으로 남은 시간을 보정하고 수신 사이에도 실제 경과 시간을 차감합니다. 중복/역순 패킷, 잘못된 시간, 이전 서버 epoch를 무시하며 같은 경기의 시간이 늘어나거나 종료된 경기가 다시 열리지 않도록 처리합니다. 서버 시간 수신 전에는 HUD에 `--:--`를 표시합니다.

기본 `USAPlayerWidget`의 화면 상단 중앙에 `03:00`부터 `00:00`까지 표시합니다. 커스텀 Widget Blueprint는 선택 바인딩 이름 `MatchTimeText`의 TextBlock을 추가하면 같은 표시를 사용할 수 있습니다. `GetMatchRemainingSeconds`, `HasMatchTime`, `IsMatchExpired`도 Blueprint에서 조회할 수 있습니다.

0초가 되면 로컬 플레이어의 이동 입력·속도·누적 힘을 초기화하고 이동과 마우스 회전을 차단합니다. 서버도 이후 상태 패킷의 위치를 마지막 승인 위치로 고정하고 이동 입력·속도를 0으로 저장·전달합니다. 서버가 꺼지거나 패킷이 유실되어도 이미 받은 제한시간은 계속 감소합니다. 서버와 클라이언트를 함께 업데이트하고, 실행 중인 서버 및 Unreal Editor를 재시작하여 적용합니다.

검증: `Tests/MatchClockTests.cpp`, `Tests/Test-MatchTimer.ps1`(실제 3분 UDP 경기), Unreal 자동화 `StickArena.Match.Timer`(위젯 표시, 종료 시 정지, 역순/잘못된 패킷, 서버 재시작, 수신 중단).

## 패킷 형식

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

## TreasureBox 동기화

`Source/TreasureWorld.h`의 `MaxTreasureBoxes` 기본값은 5입니다. 서버는 시작 시 실행 파일 옆의 `Main_LevelObjects.json`을 읽고 `treasure_box.spawn_points`의 좌표 112개를 로드합니다. 빌드하면 JSON도 실행 파일 옆에 복사됩니다. 다른 경로를 사용할 때는 `VS_StickArenaServer.exe [port] [spawn-data.json]`으로 지정합니다. 누락·잘못된 JSON, 지원하지 않는 맵/스키마, 중복 ID/XY 좌표, 허용 폴더 밖 지점은 오류를 출력하고 서버를 시작하지 않습니다.

상자가 최대 미만이면 서버 실행 후 초 단위 `BoxSpawnCount`를 누적하여 **20초마다 한 개** 생성합니다. 접속자가 없어도 타이머가 진행되며 최대 개수이면 누적을 초기화합니다. 매 회차 전체 허용 지점을 무작위 순서로 한 번씩 검사합니다. 상태가 도착한 모든 플레이어와 XY 거리 200cm 이상, 활성 상자와 250cm 이상 떨어진 지점만 선택하며 같은 지점에 활성 상자를 두 개 생성하지 않습니다. 플레이어에 가까우면 다른 지점을 검사하고, 전부 막혔으면 해당 회차를 생략하고 다음 20초에 재시도합니다. 임의 XY나 플레이어 주변 좌표로 대체하지 않습니다. 파괴된 상자의 지점은 다시 사용할 수 있습니다. 간격은 `MinimumPlayerSpawnDistance`, `MinimumBoxSpawnDistance`, 주기는 `SpawnInterval`에서 조절합니다.

패킷에는 JSON의 `position` 좌표를 그대로 담습니다. 서버는 Unreal 월드의 실시간 충돌을 조회하지 않으며, UE의 기존 지면 trace는 유지합니다. JSON의 내보내기 시점 지형 검증과 UE의 런타임 물리 검증은 별개입니다. JSON 파서는 포함된 nlohmann/json 3.11.3 (`Source/ThirdParty/nlohmann`, MIT 라이선스)을 사용합니다.

```text
SA4|type=TREASURE_STATE|epoch=<server-start>|id=1|item=0|hits=0|pos=100,200,30
SA4|type=BOX_HIT|epoch=<server-start>|session=<GUID>|id=1|seq=1
SA4|type=HIT_ACK|epoch=<server-start>|id=1|seq=1
```

`TreasureItem::Sword`와 UE `ESATreasureItem::Sword`의 wire 값은 0입니다. 상자 ID는 서버 실행 중 재사용하지 않습니다. 서버는 등록 세션/송신 endpoint와 거리 350cm, 플레이어별 동일 상자 타격 간격 200ms를 검사합니다. 타격 요청은 200ms마다 최대 2초 동안 같은 시퀀스로 재전송하고, 서버는 중복 요청을 다시 세지 않고 ACK를 보냅니다. UE는 로컬 플레이어의 공격 notify에서만 요청하여 원격 캐릭터의 공격 재생으로 인한 중복 타격을 막습니다.

서버에서 다섯 번째 유효 타격을 받으면 `hits=5`으로 전환하고 동일 ID/좌표의 Sword를 생성하도록 전송합니다. UE는 상자를 파괴하고 `BP_WeaponSword`를 해당 자리에 생성합니다. `SwordItemClass`로 드롭 클래스를 변경할 수 있습니다. 현재 상자와 드롭 상태를 생성/타격 즉시 및 1초마다 전송하여 패킷 유실과 중간 접속을 복구합니다. UE는 역순 상태와 중복 생성을 무시하고, 서버 epoch가 바뀌면 기존 동기화 액터를 정리합니다.

드롭 무기에 Overlap하면 클라이언트는 `SA4|type=PICKUP|epoch=<epoch>|session=<GUID>|id=<drop-id>|seq=<sequence>`를 요청합니다. 서버는 등록 endpoint, 서버 epoch, 드롭 상태와 플레이어 거리 350cm를 검사한 뒤 최초 요청자에게만 소유권을 부여합니다. 획득 상태는 기존 TREASURE_STATE 뒤에 `|owner=<GUID>`를 붙여 즉시 모든 클라이언트에 전송하고, 등록 시점 및 1초 주기로도 다시 보냅니다. 다른 플레이어의 요청과 중복 재시도는 소유권을 바꾸지 않습니다.

클라이언트는 서버 승인을 받은 당사자만 실제 장착하고 나머지는 같은 ID의 바닥 액터를 파괴합니다. 획득 요청은 근처에 머무는 동안 200ms 주기로 재시도합니다. 획득한 ID는 해당 epoch에서 영구적으로 기록하여 오래된 드롭 패킷, 중간 접속, 무기 소진/교체 뒤에도 바닥 아이템이 되살아나지 않습니다. 중복 승인은 탄약을 초기화하지 않으며, 늦게 도착한 이전 획득 승인은 더 최근에 장착한 무기를 교체하지 않습니다. 서버의 새 epoch에서는 드롭 ID 기록을 새로 시작합니다.

## 무기 장착 동기화

최신 클라이언트는 기존 SA3 스냅샷 뒤에 다음 필드를 붙입니다. `weapon`은 무기 Blueprint 클래스 경로를 UTF-8 hex로 인코딩한 값이고 `weaponid`는 무기 인스턴스 GUID입니다. 무장 해제는 두 문자열을 비우고 ammo/maxammo를 0으로 전송합니다.

```text
...|attack=0|weapon=<hex-class-path>|weaponid=<32-digit-GUID>|ammo=3|maxammo=10
...|attack=0|weapon=|weaponid=|ammo=0|maxammo=0
```

서버는 확장 상태를 저장·브로드캐스트하고 중간 접속자의 등록 응답에도 최신 장착 상태를 포함합니다. 기존 11필드 SA3 메시지도 계속 허용합니다. 무기는 100ms 상태 heartbeat마다 재전송하므로 장착 순간의 패킷 유실도 복구됩니다. 동일 클래스의 다른 무기는 GUID로 구분하여 교체하고, 동일 상태를 반복 수신해도 액터를 중복 생성하지 않습니다.

UE 원격 프록시는 수신 클래스로 무기 시각 액터를 만들고 `WeaponSocket`에 장착한 다음 공격을 재생합니다. 프록시는 로컬 월드 아이템을 Overlap으로 획득하거나 자체 공격 재생으로 사용 횟수를 차감하지 않습니다. 프록시가 제거되면 장착 액터도 정리합니다. 클라이언트의 무기 Blueprint는 `/Game/Items/Weapons/` 아래에 두고 양쪽 패키지에 포함해야 합니다.

서버와 모든 클라이언트를 함께 업데이트해야 합니다. 이전 서버는 확장 SA3 메시지를 처리하지 못합니다. 업데이트된 서버 실행 파일은 `bin/x64/Release/VS_StickArenaServer.exe`입니다.

검증: `Tests/Test-Weapons.ps1`, 기존 `Tests/Test-Multiplayer.ps1 -ServerPath ./bin/x64/Release/VS_StickArenaServer.exe`. Unreal 통합 테스트 `StickArena.Networking.WeaponEquipment`는 실제 최신 서버를 별도 포트에서 실행한 뒤 `-SAWeaponTestPort=<port>`로 실행합니다. 두 클라이언트 장착, 무기 인스턴스 교체, 늦은 접속, Overlap 차단, 해제를 검사합니다.

검증: `./Tests/Test-Treasure.ps1 -ServerPath ./bin/x64/Release/VS_StickArenaServer.exe`는 실제 UDP 서버에서 생성/타격, 획득 브로드캐스트, 재시도, 경쟁 획득 거부, 중간 접속과 후속 생성을 검사합니다. `Tests/TreasureWorldTests.cpp`는 거리/endpoint 검증과 단일 소유권도 검사합니다. UE 테스트 `StickArena.Networking.TreasureBox`는 중복·역순 패킷, grant-before-drop과 재생성 방지를 검사합니다. `StickArena.Networking.TreasurePickup`은 `-SAWeaponTestPort=<port>`의 실제 서버를 통해 A의 Overlap→승인→장착, B의 바닥 액터 파괴, 늦은 접속과 원격 무기 복구를 검사합니다.

## 정상 종료 시 플레이어 제거

UE `USAUdpClientComponent`는 `FCoreDelegates::OnPreExit` 및 `EndPlay`에서 소켓을 닫기 전에 `SA2|type=LEAVE|session=<GUID>` 패킷을 3회 전송합니다. 게임 종료, PIE 종료 및 해당 컴포넌트의 EndPlay에서 실행합니다. 서버는 세션 ID와 실제 송신 IP/포트를 확인하고 뮤텍스 안에서 `Players` 벡터의 해당 원소를 즉시 지웁니다. 종료한 세션은 10초간 재등록을 거부하여 지연된 등록 패킷에 의한 복구를 방지합니다. 새 접속은 새로운 GUID를 사용합니다.

강제 종료, 크래시, 네트워크 장애 등으로 퇴장 패킷이 도착하지 않으면 마지막 등록/상태 수신 후 10초 비활성 정리가 적용됩니다(정리 주기 1초). 퇴장 패킷 제거는 `reason=LEAVE`, 비활성 제거는 `reason=TIMEOUT`으로 콘솔에 플레이어 ID·이름·IP/포트·삭제 당시 남은 인원을 출력합니다. 삭제 정보는 벡터를 잠근 상태에서 복사하고 로그는 잠금을 해제한 뒤 출력하여 중복 삭제 로그를 방지합니다. 프로세스가 즉시 사라지는 경우에는 종료 패킷을 보장할 수 없으므로 서버 타임아웃이 최종 정리를 담당합니다. 다른 클라이언트의 원격 캐릭터는 기존 10초 수신 타임아웃으로 정리됩니다. 벡터 제거와 송신 주소 검증은 `Tests/DisconnectTests.cpp`에서 검사합니다.

정상 종료와 종료 통지 없는 소켓 단절의 실제 UDP 제거/로그 검증: `./Tests/Test-Disconnect.ps1`.

## Crystal Tower (SA5)

맵 JSON의 `crystal_towers[].actor_name`을 고정 ID로 사용합니다. 서버가 초기 점등, 5회 타격 후 소등, 25~35초 무작위 재점등을 결정합니다. 마지막 타격자와 효과도 서버가 결정하며 HP +20/5초, ATK +5/20초, DEF +5/20초 중 하나를 동일 확률로 부여합니다. 클라이언트는 남은 효과 시간만 적용하며 동일 타워/주기 보상을 다시 적용하지 않습니다.

```text
SA5|type=CRYSTAL_HIT|epoch=<server>|session=<GUID>|id=SACrystalTower_0|seq=1|cycle=0
SA5|type=CRYSTAL_ACK|epoch=<server>|id=SACrystalTower_0|seq=1
SA5|type=CRYSTAL_STATE|epoch=<server>|id=SACrystalTower_0|rev=1|cycle=0|hits=0|lit=1|winner=|effect=0|remaining=0.000000
```

`effect`는 0=HP 회복, 1=ATK, 2=DEF입니다. 클라이언트는 서버 상태를 받아 메시 흔들림과 조명 깜빡임을 재생하며 로컬 타이머로 재점등하지 않습니다. 서버는 등록된 세션/송신 주소, 350cm 거리, 0.2초 타격 간격, 시퀀스와 주기를 검증합니다. 상태 변경 즉시, 접속 시, 1초마다 스냅샷을 보내 중간 접속과 패킷 유실을 복구합니다. UE와 서버를 함께 업데이트해야 합니다.

추가 검증: `Tests/CrystalWorldTests.cpp`, `Tests/TreasureWorldTests.cpp`, `Tests/TestCrystalUdp.py` (Python 3 표준 라이브러리), UE 자동화 `StickArena.LevelObjects.CrystalTowerEffect`, `StickArena.Status.CrystalBlessings`, `StickArena.Networking.TreasureBox`.
