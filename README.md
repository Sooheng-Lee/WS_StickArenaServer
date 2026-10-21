# StickArena UDP 서버

## 1. git clone 이후 초기 설정

- Visual Studio 2022의 **C++를 사용한 데스크톱 개발**, MSVC v143, Windows SDK **10.0.19041.0**을 설치합니다.
- 복제한 저장소의 `VS_StickArenaServer.sln`을 열고 **Release | x64**로 빌드합니다. C++17과 WinSock2를 사용하며 JSON 라이브러리는 저장소에 포함되어 있습니다.
- 빌드 시 `Main_LevelObjects.json`이 실행 파일 옆으로 자동 복사됩니다.

## 2. 실행 과정

저장소 루트의 PowerShell에서 실행합니다. 기본 포트는 **UDP 7777**입니다.

```powershell
./bin/x64/Release/VS_StickArenaServer.exe
# 포트와 맵 데이터 경로를 직접 지정할 경우
./bin/x64/Release/VS_StickArenaServer.exe 7778 ./Main_LevelObjects.json
```

서버는 맵 데이터 로드 → 소켓 초기화 → 작업 스레드 생성 → 패킷 수신과 월드 갱신 순서로 실행됩니다. Unreal 클라이언트의 `Server Address`와 `Server Port`를 맞추고, 다른 PC에서 접속하면 서버 PC의 IPv4 주소를 사용하며 방화벽에서 해당 UDP 포트를 허용합니다. **Ctrl+C**로 종료하면 대기 작업을 처리하고 스레드와 소켓을 정리합니다.

## 3. 스레드와 저장

- 메인 스레드는 UDP 수신, 작업 큐 전달, 월드 갱신과 비활성 접속 정리를 담당합니다. CPU 수에 따라 **2~8개 작업 스레드**가 패킷을 처리하며, 같은 IP/포트의 패킷은 같은 큐에서 도착 순서대로 처리합니다. 큐당 최대 대기 건수는 **1024건**입니다.
- 플레이어 등록 정보와 최신 상태는 `PlayerRegistry`의 벡터에 저장하고 뮤텍스로 보호합니다. 보물 상자, 크리스탈과 경기 시간도 메모리에서 관리하며 별도 DB나 파일로 저장하지 않아 서버 재시작 시 초기화됩니다.
- `Main_LevelObjects.json`은 시작 시 읽는 맵 설정입니다. 정상 퇴장 요청은 즉시 반영하며, 등록/상태 수신이 **10초** 동안 없으면 주기적으로 접속을 정리합니다.
