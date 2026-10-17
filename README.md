# StickArena UDP 콘솔 서버

Windows / Visual Studio 2022의 C++17 및 WinSock2를 사용합니다. 데스크톱 C++ 개발 워크로드와 Windows SDK가 필요합니다.

## 공통 헤더와 PCH

공통 Windows/WinSock 및 C++ 표준 헤더는 `Source/pch.h`에서 관리합니다. `Source/pch.cpp`는 미리 컴파일된 헤더를 생성(`/Yc`)하고, 나머지 C++ 파일은 이를 사용(`/Yu`)합니다. Debug/Release 모두 적용되며 생성 파일은 구성별 `obj` 폴더에 저장됩니다.

새 `.cpp` 파일을 추가할 때 첫 번째 include로 `#include "pch.h"`를 작성합니다. WinSock 라이브러리 `Ws2_32.lib`는 프로젝트의 링크 설정에서 지정합니다.

## 실행

1. `VS_StickArenaServer.sln`을 열고 `Debug | x64` 또는 `Release | x64`로 빌드합니다.
2. `Ctrl+F5`로 서버를 실행합니다. 기본 UDP 수신 포트는 `7777`입니다.
3. Unreal Editor를 종료한 상태에서 `UE_StickArenaDemoEditor`를 빌드한 다음 에디터에서 Play를 실행합니다. C++ 기본 컴포넌트를 추가했으므로 처음에는 Live Coding 대신 전체 빌드를 사용합니다.
4. 이동, 이동 키 해제, 마우스 방향 변경, 공격 시 서버 콘솔에서 패킷을 확인합니다. 공격은 기존 Enhanced Input의 Triggered 발생마다 전송됩니다.
5. 서버는 `Ctrl+C`로 종료합니다.

출력 실행 파일: `bin/x64/Debug/VS_StickArenaServer.exe`

다른 포트 사용: `VS_StickArenaServer.exe 7778`

언리얼 플레이어 블루프린트의 `UDP Client` 컴포넌트에서 `Server Address`, `Server Port`, `State Send Interval`, `Enabled`를 설정합니다. 기본값은 `127.0.0.1`, `7777`, `0.1초`, 활성화입니다. 다른 PC에서 접속할 때는 서버 PC의 IPv4 주소로 바꾸고 서버 PC의 방화벽에서 해당 UDP 포트를 허용합니다.

## 패킷

각 UDP 데이터그램에 UTF-8 텍스트 한 건을 전송합니다. 현재 필드는 모두 ASCII이고, 문자열 끝의 NUL은 전송하지 않습니다. 최대 수신 크기는 1200바이트입니다.

```text
SA1|session=4c09ad2ca9f3464e93906d40f7657fd9|seq=1|action=MOVE|input=1.000,0.000,0.000|pos=100.00,200.00,90.00|vel=0.00,0.00,0.00|yaw=45.00
```

| 필드 | 의미 |
| --- | --- |
| SA1 | 프로토콜 식별자 / 버전 1 |
| session | 플레이어 컴포넌트 수명마다 생성되는 GUID. 재실행 및 여러 PIE 플레이어 구분용 |
| seq | 송신 시도마다 증가하는 순번 |
| action | MOVE: 이동 입력 시작/방향 변경, STOP: 이동 입력 해제, ATTACK: 공격 입력, STATE: 위치/회전 상태 |
| input | 정규화한 월드 이동 입력 X,Y,Z. STOP/ATTACK은 0,0,0 |
| pos | 현재 월드 위치 X,Y,Z (언리얼 cm 단위) |
| vel | 현재 월드 속도 X,Y,Z (cm/s) |
| yaw | 현재 수평 회전각 (도) |

MOVE/STOP/ATTACK은 입력 시 바로 전송합니다. STATE는 위치 또는 회전이 바뀌거나 이동 입력이 유지될 때 기본 초당 최대 10회 전송합니다. STOP은 키를 놓은 순간이며 실제 감속 중 상태는 STATE로 이어집니다. 입력 이벤트의 위치/속도는 해당 입력 처리 시점의 값입니다.

로컬 플레이어만 전송하며 게임 종료 시 소켓을 해제합니다. 서버는 송신 IP/포트, 수신 바이트 수와 패킷 원문을 출력합니다. SA1 접두사, 크기와 출력 가능한 ASCII 여부를 검사하며 필드별 게임 로직 검증은 수행하지 않습니다. 현재 범위는 단방향 진단용 전송입니다. UDP 수신 확인, 재전송, 순서 복원, 서버의 게임 상태 판정이나 클라이언트 응답은 구현하지 않습니다.
